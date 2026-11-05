#include "security/QpdfWorker.h"

#include "i18n/UiLanguage.h"
#include "io/JobContext.h"
#include "security/MeasureExport.h"
#include "recent/PathKey.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#elif defined(Q_OS_LINUX)
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace mervin::qpdfWorker {
namespace {

enum class Message : char { Hello, RequestData, RequestEnd, Ack, Reading, Open,
                           Data, Ready, SplitPlan, Blob, Result };
bool worker = false;
QStringList workerSources;
QStringList workerOutputs;
int workerOutput = 0;
int workerReading = -1;
std::mutex admissionMutex;
std::condition_variable admissionChanged;
int active = 0;
std::mutex hookMutex;
std::function<void(qint64)> startedHook;
std::atomic<quint64> testOutputBytes{kMaxOutputBytes};
std::atomic<int> testWallMilliseconds{kWallMilliseconds};

/// Throws a fixed error without including pipe contents, which can hold passwords.
[[noreturn]] void protocolFailure()
{
    throw std::runtime_error("qpdf worker protocol failure");
}

/// Bounds serialization before growing the parent's request buffer.
class RequestBuffer final : public QBuffer {
protected:
    qint64 writeData(const char *data, qint64 size) override
    {
        if (size < 0 || pos() > kMaxRequestBytes - size)
            return -1;
        return QBuffer::writeData(data, size);
    }
};

/// Lists sources in their original order, preserving merge failure attribution.
QStringList sources(const Request &request)
{
    if (request.operation != Operation::Merge)
        return {request.path};
    QStringList result;
    for (const auto &input : request.inputs)
        result.append(input.path);
    return result;
}

/// Computes Split destinations from the original request, never from child paths.
QStringList splitPaths(const Request &request, int count)
{
    if (count < 0 || count > kMaxSplitParts || request.baseName.isEmpty()
        || QFileInfo(request.baseName).fileName() != request.baseName
        || request.baseName.contains(QLatin1Char('\\')))
        protocolFailure();
    QStringList result;
    for (int i = 0; i < count; ++i)
        result.append(QDir(request.output).filePath(QStringLiteral("%1-%2.pdf")
                          .arg(request.baseName).arg(i + 1, 3, 10, QLatin1Char('0'))));
    return result;
}

/// Writes request values only to a capped, private memory buffer.
QByteArray encode(const Request &request)
{
    RequestBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QDataStream out(&buffer);
    out.setVersion(QDataStream::Qt_6_9);
    out << qint32(request.operation) << i18n::current() << request.path << request.output
        << request.password << request.userPassword << request.ownerPassword << request.baseName
        << request.pages << qint32(request.inputs.size());
    for (const auto &input : request.inputs) {
        out << input.path << input.pages << input.password;
        if (out.status() != QDataStream::Ok)
            protocolFailure();
    }
    out << qint32(request.angle) << request.relative << qint32(request.algorithm)
        << request.permissions.canPrint << request.permissions.canCopy
        << request.permissions.canModify << request.permissions.canAnnotate
        << request.keepStoredBlob << request.failRetarget << qint32(request.marks.size());
    for (const auto &mark : request.marks) {
        out << qint32(mark.page) << qint32(mark.kind) << qint32(mark.pts.size());
        for (const auto &point : mark.pts)
            out << point;
        out << mark.hasLabelPos << mark.labelPos << mark.label << mark.toPdf.a << mark.toPdf.b
            << mark.toPdf.c << mark.toPdf.d << mark.toPdf.e << mark.toPdf.f << mark.lineWidth;
        if (out.status() != QDataStream::Ok)
            protocolFailure();
    }
    out << request.measurements.has_value();
    if (request.measurements) {
        const QByteArray json = serializeMeasurements(*request.measurements);
        if (json.size() > kMaxMeasurementBlobBytes)
            throw std::runtime_error(PageOps::measurementsTooLargeError().toStdString());
        out << json;
    }
    if (out.status() != QDataStream::Ok)
        protocolFailure();
    return buffer.data();
}

/// Reads a bounded parent request inside the already limited child.
Request decode(const QByteArray &bytes)
{
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_6_9);
    Request request;
    qint32 operation, algorithm, angle, count;
    QString language;
    in >> operation >> language >> request.path >> request.output >> request.password
       >> request.userPassword >> request.ownerPassword >> request.baseName >> request.pages >> count;
    if (operation < 0 || operation > int(Operation::Blob) || count < 0 || count > 10000)
        protocolFailure();
    request.operation = Operation(operation);
    for (int i = 0; i < count; ++i) {
        PageOps::MergeInput input;
        in >> input.path >> input.pages >> input.password;
        request.inputs.append(input);
    }
    in >> angle >> request.relative >> algorithm >> request.permissions.canPrint
       >> request.permissions.canCopy >> request.permissions.canModify
       >> request.permissions.canAnnotate >> request.keepStoredBlob >> request.failRetarget >> count;
    if (algorithm < 0 || algorithm > int(QpdfService::Algorithm::RC4_128)
        || count < 0 || count > 100000)
        protocolFailure();
    request.algorithm = QpdfService::Algorithm(algorithm);
    request.angle = angle;
    for (int i = 0; i < count; ++i) {
        RenderMeasurement mark;
        qint32 page, kind, points;
        in >> page >> kind >> points;
        if (points < 0 || points > 1000000)
            protocolFailure();
        mark.page = page;
        mark.kind = MeasureKind(kind);
        for (int j = 0; j < points; ++j) {
            QPointF point;
            in >> point;
            mark.pts.push_back(point);
        }
        in >> mark.hasLabelPos >> mark.labelPos >> mark.label >> mark.toPdf.a >> mark.toPdf.b
           >> mark.toPdf.c >> mark.toPdf.d >> mark.toPdf.e >> mark.toPdf.f >> mark.lineWidth;
        request.marks.push_back(std::move(mark));
    }
    bool hasMeasurements;
    in >> hasMeasurements;
    if (hasMeasurements) {
        QByteArray json;
        in >> json;
        request.measurements.emplace();
        if (json.size() > kMaxMeasurementBlobBytes || !parseMeasurements(json, &*request.measurements))
            protocolFailure();
    }
    if (in.status() != QDataStream::Ok || !in.atEnd())
        protocolFailure();
    i18n::apply(language);
    return request;
}

/// Installs hard limits before QCoreApplication, IPC decoding or any PDF parsing.
bool installLimits()
{
#ifdef Q_OS_LINUX
    const auto limit = [](int resource, rlim_t value) {
        rlimit existing{};
        if (getrlimit(resource, &existing) != 0)
            return false;
        const rlim_t bound = std::min({value, existing.rlim_cur, existing.rlim_max});
        const rlimit requested{bound, bound};
        return setrlimit(resource, &requested) == 0;
    };
    const pid_t parent = getppid();
    return parent != 1 && prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() == parent
        && prctl(PR_SET_DUMPABLE, 0) == 0
        && limit(RLIMIT_AS, kMemoryBytes) && limit(RLIMIT_CPU, kCpuSeconds)
        && limit(RLIMIT_CORE, 0) && limit(RLIMIT_FSIZE, 0) && limit(RLIMIT_NOFILE, 64);
#elif defined(Q_OS_WIN)
    // Keep the job handle until process exit. Closing it earlier kills this process.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY
        | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME
        | JOB_OBJECT_LIMIT_JOB_TIME | JOB_OBJECT_LIMIT_ACTIVE_PROCESS
        | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = LONGLONG(kCpuSeconds) * 10000000;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = LONGLONG(kCpuSeconds) * 10000000;
    limits.ProcessMemoryLimit = SIZE_T(kMemoryBytes);
    limits.JobMemoryLimit = SIZE_T(kMemoryBytes);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))
        || !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return false;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return _setmode(_fileno(stdin), _O_BINARY) != -1
        && _setmode(_fileno(stdout), _O_BINARY) != -1;
#else
    return false; // Unsupported operating systems must not parse in the GUI process.
#endif
}

/// Packs a length-checked message. Data frames never accumulate whole output PDFs.
QByteArray frame(Message type, const QByteArray &payload = {})
{
    if (payload.size() >= kMaxFrameBytes)
        protocolFailure();
    QByteArray bytes(5, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(payload.size() + 1), bytes.data());
    bytes[4] = char(type);
    bytes.append(payload);
    return bytes;
}

/// Reads exactly one private-pipe message in the child.
QByteArray childRead()
{
    unsigned char header[4];
    if (std::fread(header, 1, 4, stdin) != 4)
        protocolFailure();
    const quint32 size = qFromBigEndian<quint32>(header);
    if (size < 1 || size > kMaxFrameBytes)
        protocolFailure();
    QByteArray bytes(qsizetype(size), Qt::Uninitialized);
    if (std::fread(bytes.data(), 1, size, stdin) != size)
        protocolFailure();
    return bytes;
}

/// Sends one message and waits for its acknowledgement before producing more data.
void childSend(Message type, const QByteArray &payload = {}, bool acknowledge = true)
{
    const QByteArray bytes = frame(type, payload);
    if (std::fwrite(bytes.data(), 1, size_t(bytes.size()), stdout) != size_t(bytes.size())
        || std::fflush(stdout) != 0)
        protocolFailure();
    if (acknowledge) {
        const QByteArray response = childRead();
        if (response != QByteArray(1, char(Message::Ack)))
            protocolFailure();
    }
}

/// Encodes fixed metadata; caller-provided documents never become JSON commands.
QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

/// Reads bounded child metadata without trusting declared container lengths.
QJsonObject object(const QByteArray &bytes)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        protocolFailure();
    return document.object();
}

/// Checks integer fields before indexing parent-owned requests or outputs.
int integer(const QJsonObject &value, const char *key, int minimum, int maximum)
{
    const auto entry = value.value(QLatin1String(key));
    const double number = entry.toDouble(double(minimum) - 1);
    if (number < minimum || number > maximum || number != int(number))
        protocolFailure();
    return int(number);
}

/// Executes algorithms only after the worker mode installed its process limits.
Result execute(const Request &request)
{
    Result result;
    QpdfService service;
    if (request.failRetarget)
        PageOps::testing::failNextRetarget();
    switch (request.operation) {
    case Operation::Info:
        result.status = service.readInfo(request.path, request.password, result.info, &result.error);
        break;
    case Operation::Decrypt:
        result.status = service.decrypt(request.path, request.output, request.password, &result.error);
        break;
    case Operation::Encrypt:
        result.status = service.encrypt(request.path, request.output, request.password,
            request.userPassword, request.ownerPassword, request.algorithm, request.permissions, &result.error);
        break;
    case Operation::Probe:
        result.status = PageOps::probe(request.path, &result.count, request.password, &result.error,
                                      &result.encrypted, &result.sourceKey);
        break;
    case Operation::Delete:
        result.status = PageOps::deletePages(request.path, request.output, request.pages,
            request.password, &result.error, &result.failedIndex);
        break;
    case Operation::Rotate:
        result.status = PageOps::rotatePages(request.path, request.output, request.pages,
            request.angle, request.relative, request.password, &result.error);
        break;
    case Operation::Merge:
        result.status = PageOps::merge(request.inputs, request.output, &result.error, &result.failedIndex);
        break;
    case Operation::Split:
        result.status = PageOps::split(request.path, request.output, request.baseName, request.password,
            nullptr, &result.error, [&](const QStringList &paths) {
                if (paths.size() > kMaxSplitParts)
                    protocolFailure();
                workerOutputs = splitPaths(request, int(paths.size()));
                if (paths != workerOutputs)
                    protocolFailure();
                childSend(Message::SplitPlan, json({{"count", paths.size()}}));
                return true;
            });
        result.projection = PageOps::testing::lastSplitProjection();
        break;
    case Operation::Flatten:
        result.status = MeasureExport::flatten(request.path, request.output, request.marks,
            request.password, &result.error, request.keepStoredBlob);
        break;
    case Operation::Embed:
        if (!request.measurements)
            protocolFailure();
        result.status = MeasureExport::embedMervin(request.path, request.output, *request.measurements,
                                                  request.password, &result.error);
        break;
    case Operation::Keep:
        result.status = MeasureExport::keepMervin(request.path, request.output, request.password, &result.error);
        break;
    case Operation::Blob:
        result.blob = MeasureExport::readMervinBlob(request.path, request.password);
        result.status = QpdfService::Status::Ok;
        break;
    }
    return result;
}

/// Owns parent publication state. No child-chosen filename reaches QSaveFile.
struct Output {
    job::DestinationLock lock;
    QSaveFile file;
    bool ready = false;

    explicit Output(const QString &path) : lock(path), file(path)
    {
        if (!lock.owns() || !job::writing(QFileInfo(path).absolutePath()))
            throw std::runtime_error(job::notRunError().toStdString());
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error(file.errorString().toStdString());
    }

    /// Flushes, revalidates consent under the destination lock, and admits commit.
    void commit(const Callbacks &callbacks)
    {
        if (!ready)
            protocolFailure();
        if (!file.flush())
            throw std::runtime_error(file.errorString().toStdString());
        if ((callbacks.mayCommit && !callbacks.mayCommit(file.fileName()))
            || !job::beginCommit(file.fileName()))
            throw std::runtime_error(job::notRunError().toStdString());
        const auto end = qScopeGuard([] { job::endCommit(); });
        if (!file.commit())
            throw std::runtime_error(file.errorString().toStdString());
    }
};

/// Keeps one helper slot until process cleanup completes. Waiting honors Stop.
class Admission {
public:
    Admission()
    {
        std::unique_lock lock(admissionMutex);
        while (active >= kMaxWorkers && !job::stopped())
            admissionChanged.wait_for(lock, std::chrono::milliseconds(20));
        if (!job::stopped()) {
            ++active;
            acquired_ = true;
        }
    }
    ~Admission()
    {
        if (acquired_) {
            std::lock_guard lock(admissionMutex);
            --active;
            admissionChanged.notify_all();
        }
    }
    bool acquired() const { return acquired_; }
private:
    bool acquired_ = false;
};

/// Synchronous, bounded pipe transport on the calling worker thread.
class Process {
public:
    QProcess process;
    QElapsedTimer elapsed;
    qint64 deadline = testWallMilliseconds.load();
    bool excessOutput = false;

    Process()
    {
        elapsed.start();
        // Warnings stay inside the child's memory budget. Never buffer stderr in Qt.
        process.setStandardErrorFile(QProcess::nullDevice());
        process.setProcessChannelMode(QProcess::SeparateChannels);
        // QProcess also drains stdout while waiting for writes or process exit.
        // Enforce the cap on those reads too, before Qt can accumulate more frames.
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [this] {
            if (process.bytesAvailable() > kMaxFrameBytes + 4) {
                excessOutput = true;
                process.closeReadChannel(QProcess::StandardOutput);
                process.kill();
            }
        });
#ifdef MERVIN_TEST_QPDF_EXECUTABLE
        const QString executable = QStringLiteral(MERVIN_TEST_QPDF_EXECUTABLE);
#else
        const QString name = QStringLiteral(MERVIN_QPDF_EXECUTABLE);
        QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(name);
        // Tests are built in a child directory of the application target.
        if (!QFileInfo::exists(executable))
            executable = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../" + name);
#endif
        process.start(executable, {QStringLiteral("--internal-qpdf-worker")});
    }
    ~Process()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            // Reap before releasing admission. A process stuck in a kernel drive call
            // may keep this background thread, but never admits another live helper.
            process.waitForFinished(-1);
        }
    }

    /// Checks cancellation and elapsed helper time, excluding parent callbacks/I/O.
    void check()
    {
        if (job::stopped())
            throw std::runtime_error(job::notRunError().toStdString());
        if (excessOutput || elapsed.elapsed() > deadline)
            protocolFailure();
    }

    /// Runs parent-owned I/O or consent without charging a waiting child wall time.
    template<class Function> void parent(Function function)
    {
        const qint64 start = elapsed.elapsed();
        const auto restore = qScopeGuard([&] { deadline += elapsed.elapsed() - start; });
        function();
    }

    /// Sends at most one bounded frame; the peer acknowledges before the next.
    void send(Message type, const QByteArray &payload = {})
    {
        check();
        const QByteArray bytes = frame(type, payload);
        if (process.write(bytes) != bytes.size())
            protocolFailure();
        while (process.bytesToWrite()) {
            check();
            if (!process.waitForBytesWritten(20) && process.state() == QProcess::NotRunning)
                protocolFailure();
            if (process.bytesAvailable() > kMaxFrameBytes + 4)
                protocolFailure();
        }
    }

    /// Reads one frame without trusting its size or letting stdout accumulate.
    QByteArray receive()
    {
        QByteArray bytes;
        qsizetype needed = 4;
        while (bytes.size() < needed) {
            check();
            if (process.bytesAvailable() > kMaxFrameBytes + 4)
                protocolFailure();
            bytes.append(process.read(needed - bytes.size()));
            if (bytes.size() == 4 && needed == 4) {
                const quint32 size = qFromBigEndian<quint32>(bytes.constData());
                if (size < 1 || size > kMaxFrameBytes)
                    protocolFailure();
                needed += size;
            }
            if (bytes.size() < needed && !process.bytesAvailable() && !process.waitForReadyRead(20)
                && process.state() == QProcess::NotRunning && !process.bytesAvailable())
                protocolFailure();
        }
        return bytes.mid(4);
    }
};

} // namespace

QString failureText()
{
    //: Visible failure reason for PDF security, page or measurement operations.
    //: The isolated PDF worker could not start safely, stopped unexpectedly, or
    //: exceeded its memory, time, request or output limit. This is not a diagnosis
    //: that the PDF is damaged. Suggest processing a smaller document.
    return QCoreApplication::translate("mervin::QpdfWorker",
        "PDF processing stopped or exceeded a safety limit. Try a smaller document.");
}

bool inWorker()
{
    return worker;
}

bool reading(const QString &path, bool again, int sourceIndex)
{
    if (!worker) {
        if (again) {
            job::switchTo(job::Step::Reading, path);
            return !job::stopped();
        }
        return job::reading(path);
    }
    const int index = sourceIndex >= 0 ? sourceIndex : workerSources.indexOf(path);
    if (index < 0 || index >= workerSources.size() || workerSources.at(index) != path)
        protocolFailure();
    if (!again || workerReading != index) {
        childSend(Message::Reading, json({{"index", index}, {"again", again}}));
        workerReading = index;
    }
    return true;
}

void openOutput(const QString &path)
{
    if (!worker || workerOutput >= workerOutputs.size() || workerOutputs.at(workerOutput) != path)
        protocolFailure();
    childSend(Message::Open, json({{"index", workerOutput}}));
    workerReading = -1;
}

void writeOutput(const unsigned char *data, size_t length)
{
    if (!worker)
        protocolFailure();
    while (length) {
        const size_t size = std::min(length, size_t(kMaxFrameBytes - 1));
        childSend(Message::Data, QByteArray(reinterpret_cast<const char *>(data), qsizetype(size)));
        data += size;
        length -= size;
    }
    workerReading = -1;
}

void finishOutput()
{
    childSend(Message::Ready);
    ++workerOutput;
    workerReading = -1;
}

int main(int argc, char **argv)
{
    if (!installLimits())
        return 125;
    // qpdf can catch allocation exceptions and recover by dropping PDF objects.
    // Allocation exhaustion must instead terminate this disposable worker, so a
    // partially recovered document cannot be reported as a successful output.
    std::set_new_handler([] { std::_Exit(123); });
    worker = true;
    try {
        QCoreApplication app(argc, argv);
        childSend(Message::Hello, QByteArrayLiteral("mervin-qpdf-1"), false);
        QByteArray requestBytes;
        while (true) {
            const QByteArray message = childRead();
            if (Message(message[0]) == Message::RequestEnd && message.size() == 1)
                break;
            if (Message(message[0]) != Message::RequestData
                || requestBytes.size() > kMaxRequestBytes - (message.size() - 1))
                protocolFailure();
            requestBytes.append(message.constData() + 1, message.size() - 1);
            childSend(Message::Ack, {}, false);
        }
        Request request = decode(requestBytes);
        requestBytes.fill('\0');
        requestBytes.clear();
        workerSources = sources(request);
        if (request.operation != Operation::Split && !request.output.isEmpty())
            workerOutputs = {request.output};
        Result result = execute(request);
        if (result.error == QLatin1String("std::bad_alloc")
            || result.error == QLatin1String("bad allocation"))
            result.error = failureText();
        if (result.blob) {
            if (result.blob->size() > kMaxMeasurementBlobBytes)
                protocolFailure();
            for (qsizetype offset = 0; offset < result.blob->size(); offset += kMaxFrameBytes - 1)
                childSend(Message::Blob, result.blob->mid(offset, kMaxFrameBytes - 1));
        }
        const auto &info = result.info;
        childSend(Message::Result, json({
            {"status", int(result.status)}, {"error", result.error.left(8192)},
            {"failed", result.failedIndex}, {"count", result.count}, {"encrypted", result.encrypted},
            {"key", result.sourceKey.left(32768)}, {"blob", result.blob.has_value()},
            {"projectionPages", qint64(result.projection.pages)},
            {"projectionFields", qint64(result.projection.fields)},
            {"infoEncrypted", info.encrypted}, {"algorithm", info.algorithm},
            {"bits", info.keyLengthBits}, {"revision", info.revision},
            {"print", info.permissions.canPrint}, {"copy", info.permissions.canCopy},
            {"modify", info.permissions.canModify}, {"annotate", info.permissions.canAnnotate},
            {"lowRes", info.lowResolutionPrintOnly}, {"assembly", info.pageAssemblyOnly}
        }), false);
        return 0;
    } catch (...) {
        // Never print request contents or exception text from the pipe decoder.
        return 124;
    }
}

Result run(const Request &request, const Callbacks &callbacks)
{
    Result result;
    int activeSource = -1;
    try {
        Admission admission;
        if (!admission.acquired()) {
            result.error = job::notRunError();
            return result;
        }
        QByteArray requestBytes = encode(request);
        const auto wipe = qScopeGuard([&] { requestBytes.fill('\0'); });
        Process child;
        if (child.receive() != QByteArray(1, char(Message::Hello)) + "mervin-qpdf-1")
            protocolFailure();
        {
            std::lock_guard lock(hookMutex);
            if (startedHook)
                startedHook(child.process.processId());
        }
        for (qsizetype offset = 0; offset < requestBytes.size(); offset += kMaxFrameBytes - 1) {
            child.send(Message::RequestData, requestBytes.mid(offset, kMaxFrameBytes - 1));
            if (child.receive() != QByteArray(1, char(Message::Ack)))
                protocolFailure();
        }
        requestBytes.fill('\0');
        requestBytes.clear();
        child.send(Message::RequestEnd);

        const QStringList inputPaths = sources(request);
        const bool split = request.operation == Operation::Split;
        QStringList outputPaths = split || request.output.isEmpty() ? QStringList{} : QStringList{request.output};
        bool splitPlanned = false;
        std::unique_ptr<Output> output;
        int nextOutput = 0;
        quint64 outputBytes = 0;
        const quint64 outputLimit = testOutputBytes.load();
        QByteArray blob;
        bool finished = false;
        while (!finished) {
            const QByteArray message = child.receive();
            const Message type = Message(message[0]);
            const QByteArray payload = message.mid(1);
            switch (type) {
            case Message::Reading: {
                const auto meta = object(payload);
                const int index = integer(meta, "index", 0, int(inputPaths.size()) - 1);
                activeSource = index;
                child.parent([&] {
                    if (meta.value("again").toBool())
                        job::switchTo(job::Step::Reading, inputPaths.at(index));
                    else if (!job::reading(inputPaths.at(index)))
                        throw std::runtime_error(job::notRunError().toStdString());
                });
                break;
            }
            case Message::SplitPlan: {
                if (!split || splitPlanned || output || nextOutput)
                    protocolFailure();
                outputPaths = splitPaths(request, integer(object(payload), "count", 0, kMaxSplitParts));
                splitPlanned = true;
                bool consent = true;
                child.parent([&] { consent = !callbacks.mayWrite || callbacks.mayWrite(outputPaths); });
                if (!consent)
                    return result; // Preserve the existing empty-error consent refusal contract.
                break;
            }
            case Message::Open: {
                if (output || (split && !splitPlanned)
                    || integer(object(payload), "index", 0, int(outputPaths.size()) - 1) != nextOutput)
                    protocolFailure();
                activeSource = -1;
                child.parent([&] { output = std::make_unique<Output>(outputPaths.at(nextOutput)); });
                break;
            }
            case Message::Data: {
                if (!output || output->ready || quint64(payload.size()) > outputLimit - outputBytes)
                    protocolFailure();
                outputBytes += quint64(payload.size());
                activeSource = -1;
                child.parent([&] {
                    job::switchTo(job::Step::Writing, QFileInfo(outputPaths.at(nextOutput)).absolutePath());
                    if (job::stopped())
                        throw std::runtime_error(job::notRunError().toStdString());
                    if (output->file.write(payload) != payload.size())
                        throw std::runtime_error(output->file.errorString().toStdString());
                    job::progressed();
                });
                break;
            }
            case Message::Ready:
                if (!payload.isEmpty() || !output || output->ready)
                    protocolFailure();
                output->ready = true;
                activeSource = -1;
                if (split) {
                    child.parent([&] { output->commit(callbacks); });
                    result.outputs.append(outputPaths.at(nextOutput++));
                    output.reset();
                }
                break;
            case Message::Blob:
                if (request.operation != Operation::Blob || blob.size() > kMaxMeasurementBlobBytes - payload.size())
                    protocolFailure();
                blob.append(payload);
                break;
            case Message::Result: {
                const auto meta = object(payload);
                result.status = QpdfService::Status(integer(meta, "status", 0, 2));
                result.error = meta.value("error").toString();
                result.failedIndex = integer(meta, "failed", -1, int(inputPaths.size()) - 1);
                result.count = integer(meta, "count", -1, std::numeric_limits<int>::max());
                result.encrypted = meta.value("encrypted").toBool();
                result.projection.pages = quint64(integer(meta, "projectionPages", 0, std::numeric_limits<int>::max()));
                result.projection.fields = quint64(integer(meta, "projectionFields", 0, std::numeric_limits<int>::max()));
                auto &info = result.info;
                info.encrypted = meta.value("infoEncrypted").toBool();
                info.algorithm = meta.value("algorithm").toString();
                info.keyLengthBits = integer(meta, "bits", 0, 4096);
                info.revision = integer(meta, "revision", 0, std::numeric_limits<int>::max());
                info.permissions = {meta.value("print").toBool(), meta.value("copy").toBool(),
                                    meta.value("modify").toBool(), meta.value("annotate").toBool()};
                info.lowResolutionPrintOnly = meta.value("lowRes").toBool();
                info.pageAssemblyOnly = meta.value("assembly").toBool();
                if (request.operation == Operation::Blob && meta.value("blob").toBool())
                    result.blob = blob;
                finished = true;
                break;
            }
            default:
                protocolFailure();
            }
            if (!finished)
                child.send(Message::Ack);
        }
        child.process.closeWriteChannel();
        while (child.process.state() != QProcess::NotRunning) {
            child.check();
            child.process.waitForFinished(20);
            if (child.process.bytesAvailable())
                protocolFailure();
        }
        if (child.process.bytesAvailable() || child.process.exitStatus() != QProcess::NormalExit
            || child.process.exitCode() != 0)
            protocolFailure();
        if (result.status == QpdfService::Status::Ok) {
            if (request.operation == Operation::Probe)
                child.parent([&] { result.sourceKey = fileKey(QFileInfo(request.path)); });
            if (split) {
                if (!splitPlanned || output || nextOutput != outputPaths.size())
                    protocolFailure();
            } else if (!outputPaths.isEmpty()) {
                if (!output || !output->ready)
                    protocolFailure();
                child.parent([&] { output->commit(callbacks); });
                result.outputs.append(outputPaths.front());
            }
        }
    } catch (const std::exception &error) {
        result.status = QpdfService::Status::Failed;
        result.blob.reset();
        result.failedIndex = activeSource;
        const QString detail = QString::fromUtf8(error.what());
        result.error = detail == QLatin1String("qpdf worker protocol failure") ? failureText() : detail;
    } catch (...) {
        result.status = QpdfService::Status::Failed;
        result.blob.reset();
        result.error = failureText();
        result.failedIndex = activeSource;
    }
    return result;
}

void testing::setStartedHook(std::function<void(qint64)> hook)
{
    std::lock_guard lock(hookMutex);
    startedHook = std::move(hook);
}

void testing::setBounds(quint64 outputBytes, int wallMilliseconds)
{
    testOutputBytes = outputBytes ? std::min(outputBytes, kMaxOutputBytes) : kMaxOutputBytes;
    testWallMilliseconds = wallMilliseconds > 0 ? std::min(wallMilliseconds, kWallMilliseconds)
                                              : kWallMilliseconds;
}

int testing::activeWorkers()
{
    std::lock_guard lock(admissionMutex);
    return active;
}

} // namespace mervin::qpdfWorker
