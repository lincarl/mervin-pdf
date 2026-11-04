#include "io/JobContext.h"
#include "security/QpdfService.h"
#include "security/QpdfWorker.h"

#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <future>
#include <map>

#ifdef Q_OS_LINUX
#include <signal.h>
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

using namespace mervin;
using Status = QpdfService::Status;

namespace {

/// Writes synthetic fixtures without opening hostile data in the test process.
bool save(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/// Reads the previous destination for atomic-publication assertions.
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Builds a simple multi-page PDF and an optional large uncompressed object array.
QByteArray plainPdf(int pages = 3, int arrayItems = 0)
{
    QByteArray pdf("%PDF-1.7\n");
    QList<qsizetype> offsets{0};
    const auto append = [&](const QByteArray &body) {
        const int id = int(offsets.size());
        offsets.append(pdf.size());
        pdf += QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    append("<< /Type /Catalog /Pages 2 0 R /Test 3 0 R >>");
    QByteArray kids;
    for (int i = 0; i < pages; ++i)
        kids += QByteArray::number(i + 4) + " 0 R ";
    append("<< /Type /Pages /Kids [" + kids + "] /Count " + QByteArray::number(pages) + " >>");
    append("[" + QByteArray("(abc) ").repeated(arrayItems) + "]");
    for (int i = 0; i < pages; ++i)
        append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>");
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (qsizetype offset : offsets.mid(1))
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n"
        + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

/// Builds a required object stream with RunLength output above the whole-worker limit.
/// No Flate decoder is involved. The encoded fixture stays near 16 MiB.
QByteArray runLengthPdf()
{
    QByteArray encoded("\3" "1 0 ", 5);
    const qsizetype runs = qsizetype(qpdfWorker::kMemoryBytes / 128);
    encoded += QByteArray("\201 ", 2).repeated(runs);
    const QByteArray catalog("<< /Type /Catalog /Pages 2 0 R >>");
    encoded += char(catalog.size() - 1);
    encoded += catalog;
    encoded += char(128);
    QByteArray pdf("%PDF-1.7\n");
    std::map<int, qsizetype> offsets;
    const auto append = [&](int id, const QByteArray &body) {
        offsets[id] = pdf.size();
        pdf += QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    append(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    append(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>");
    append(4, "<< /Type /ObjStm /N 1 /First " + QByteArray::number(qpdfWorker::kMemoryBytes + 4)
        + " /Filter /RunLengthDecode /Length " + QByteArray::number(encoded.size())
        + " >>\nstream\n" + encoded + "\nendstream");
    offsets[5] = pdf.size();
    QByteArray xref;
    const auto row = [&](int type, quint32 offset, quint16 generation) {
        xref += char(type);
        for (int shift : {24, 16, 8, 0})
            xref += char(offset >> shift);
        xref += char(generation >> 8);
        xref += char(generation);
    };
    row(0, 0, 65535);
    row(2, 4, 0);
    for (int i = 2; i <= 5; ++i)
        row(1, quint32(offsets.at(i)), 0);
    const qsizetype start = offsets[5];
    append(5, "<< /Type /XRef /Size 6 /W [1 4 2] /Root 1 0 R /Length "
        + QByteArray::number(xref.size()) + " >>\nstream\n" + xref + "\nendstream");
    pdf += "startxref\n" + QByteArray::number(start) + "\n%%EOF\n";
    return pdf;
}

/// Terminates only the observed helper process, never the application under test.
bool killWorker(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!handle)
        return false;
    const bool killed = TerminateProcess(handle, 7);
    CloseHandle(handle);
    return killed;
#else
    return ::kill(pid_t(pid), SIGKILL) == 0;
#endif
}

} // namespace

class TstQpdfWorker : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void normalAndPassword();
    void repeatedSourceFailureIndex();
    void actualLimitsAndKilledWorker();
    void admissionAndQueuedCancellation();
    void runningCancellationKeepsGuiAlive();
    void partialSplitSurvivesHelperDeath();
    void nonFlateMemoryLimit();
    void requestAndSplitBounds();
    void outputBound();
#ifdef Q_OS_LINUX
    void cpuLimit();
    void stalledWorkerDeadline();
    void descriptorLimitIsVisible();
#endif
};

void TstQpdfWorker::cleanup()
{
    job::testing::release();
    job::testing::failWhen({});
    qpdfWorker::testing::setStartedHook({});
    qpdfWorker::testing::setBounds();
    QCOMPARE(qpdfWorker::testing::activeWorkers(), 0);
}

/// Exercises the shipped helper for legitimate metadata, encryption and password failure.
void TstQpdfWorker::normalAndPassword()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("encrypted.pdf");
    QVERIFY(save(source, plainPdf()));
    int starts = 0;
    qpdfWorker::testing::setStartedHook([&](qint64 pid) {
        QVERIFY(pid != QCoreApplication::applicationPid());
        ++starts;
    });
    QString error;
    QCOMPARE(QpdfService().encrypt(source, output, {}, "private-open", "private-owner",
                                  QpdfService::Algorithm::AES256, {}, &error), Status::Ok);
    int count = -1;
    QCOMPARE(PageOps::probe(output, &count, {}, &error), Status::NeedsPassword);
    QCOMPARE(count, -1);
    QCOMPARE(PageOps::probe(output, &count, "private-open", &error), Status::Ok);
    QCOMPARE(count, 3);
    QCOMPARE(starts, 3);
}

/// A repeated path with another password retains the actual input index on failure.
void TstQpdfWorker::repeatedSourceFailureIndex()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), encrypted = folder.filePath("encrypted.pdf");
    const QString output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    QVERIFY(save(output, "previous output"));
    QCOMPARE(QpdfService().encrypt(source, encrypted, {}, "user", "owner",
                                  QpdfService::Algorithm::AES256, {}), Status::Ok);
    int reads = 0;
    job::testing::failWhen([&](job::Step step, const QString &path) {
        return step == job::Step::Reading && path == encrypted && ++reads == 2;
    });
    int failedIndex = -1;
    QString error;
    QCOMPARE(PageOps::merge({{encrypted, {0}, "user"}, {encrypted, {1}, "owner"}},
                            output, &error, &failedIndex), Status::Failed);
    QCOMPARE(reads, 2);
    QCOMPARE(failedIndex, 1);
    QCOMPARE(read(output), QByteArray("previous output"));
}

/// Observes installed kernel limits before any PDF is sent and preserves prior output on death.
void TstQpdfWorker::actualLimitsAndKilledWorker()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    QVERIFY(save(output, "previous output"));
    bool checked = false;
    qpdfWorker::testing::setStartedHook([&](qint64 pid) {
#ifdef Q_OS_LINUX
        rlimit memory{}, cpu{}, files{}, core{}, outputLimit{};
        QVERIFY(::prlimit(pid_t(pid), RLIMIT_AS, nullptr, &memory) == 0);
        QVERIFY(::prlimit(pid_t(pid), RLIMIT_CPU, nullptr, &cpu) == 0);
        QVERIFY(::prlimit(pid_t(pid), RLIMIT_NOFILE, nullptr, &files) == 0);
        QVERIFY(::prlimit(pid_t(pid), RLIMIT_CORE, nullptr, &core) == 0);
        QVERIFY(::prlimit(pid_t(pid), RLIMIT_FSIZE, nullptr, &outputLimit) == 0);
        QCOMPARE(memory.rlim_cur, qpdfWorker::kMemoryBytes);
        QCOMPARE(memory.rlim_max, qpdfWorker::kMemoryBytes);
        QCOMPARE(cpu.rlim_cur, rlim_t(qpdfWorker::kCpuSeconds));
        QCOMPARE(cpu.rlim_max, rlim_t(qpdfWorker::kCpuSeconds));
        QVERIFY(files.rlim_max <= 64);
        QCOMPARE(core.rlim_max, rlim_t(0));
        QCOMPARE(outputLimit.rlim_max, rlim_t(0));
        const QByteArray command = read(QStringLiteral("/proc/%1/cmdline").arg(pid));
        QVERIFY(!command.contains("private-password"));
        QVERIFY(command.contains("--internal-qpdf-worker"));
#elif defined(Q_OS_WIN)
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        QVERIFY(process);
        BOOL assigned = FALSE;
        QVERIFY(IsProcessInJob(process, nullptr, &assigned));
        CloseHandle(process);
        QVERIFY(assigned);
#endif
        checked = true;
        QVERIFY(killWorker(pid));
    });
    QString error;
    QCOMPARE(QpdfService().decrypt(source, output, "private-password", &error), Status::Failed);
    QVERIFY(checked);
    QCOMPARE(error, qpdfWorker::failureText());
    QCOMPARE(read(output), QByteArray("previous output"));
}

/// Held writers occupy both helper slots; a stopped waiter never launches.
void TstQpdfWorker::admissionAndQueuedCancellation()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    std::atomic<int> starts{0};
    qpdfWorker::testing::setStartedHook([&](qint64) { ++starts; });
    job::State first, second, waiting;
    job::testing::holdWhen([](job::Step step, const QString &) { return step == job::Step::Writing; });
    auto running = std::async(std::launch::async, [&] {
        job::Scope scope(first);
        return QpdfService().decrypt(source, output, {});
    });
    const auto release = qScopeGuard([&] { first.stop(); second.stop(); waiting.stop(); job::testing::release(); });
    QTRY_COMPARE(job::testing::held(), 1);
    auto other = std::async(std::launch::async, [&] {
        job::Scope scope(second);
        return QpdfService().decrypt(source, folder.filePath("other.pdf"), {});
    });
    const auto releaseOther = qScopeGuard([&] { second.stop(); job::testing::release(); });
    QTRY_COMPARE(job::testing::held(), qpdfWorker::kMaxWorkers);
    QCOMPARE(qpdfWorker::testing::activeWorkers(), qpdfWorker::kMaxWorkers);
    auto queued = std::async(std::launch::async, [&] {
        job::Scope scope(waiting);
        return PageOps::pageCount(source);
    });
    const auto stopQueued = qScopeGuard([&] { waiting.stop(); });
    QTest::qWait(80);
    QCOMPARE(starts.load(), 2);
    waiting.stop();
    QTRY_VERIFY(queued.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    QCOMPARE(queued.get(), -1);
    QCOMPARE(starts.load(), 2);
    first.stop();
    second.stop();
    job::testing::release();
    QTRY_VERIFY(running.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    QCOMPARE(running.get(), Status::Failed);
    QCOMPARE(other.get(), Status::Failed);
    QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(PageOps::pageCount(source), 3);
    QCOMPARE(starts.load(), 3);
}

/// Stop kills a helper awaiting source bytes while the parent's widget remains interactive.
void TstQpdfWorker::runningCancellationKeepsGuiAlive()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("slow.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf(3, 1000000)));
    QVERIFY(save(output, "previous output"));
    job::State state;
    std::atomic<bool> launched{false};
    qpdfWorker::testing::setStartedHook([&](qint64) { launched = true; });
    auto task = std::async(std::launch::async, [&] {
        job::Scope scope(state);
        return QpdfService().decrypt(source, output, {});
    });
    const auto stop = qScopeGuard([&] { state.stop(); });
    QTRY_VERIFY(launched.load());
    QPushButton button("Responsive");
    int clicks = 0;
    connect(&button, &QPushButton::clicked, [&] { ++clicks; });
    button.show();
    QTest::mouseClick(&button, Qt::LeftButton);
    QCOMPARE(clicks, 1);
    state.stop();
    QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
    QCOMPARE(task.get(), Status::Failed);
    QCOMPARE(read(output), QByteArray("previous output"));
    QCOMPARE(PageOps::pageCount(folder.filePath("missing.pdf")), -1);
    QTest::mouseClick(&button, Qt::LeftButton);
    QCOMPARE(clicks, 2);
}

/// A dead helper cannot erase the parent's ledger of already committed Split parts.
void TstQpdfWorker::partialSplitSurvivesHelperDeath()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf");
    QVERIFY(save(source, plainPdf()));
    const QString first = folder.filePath("part-001.pdf"), second = folder.filePath("part-002.pdf");
    QVERIFY(save(second, "previous second"));
    qint64 pid = 0;
    qpdfWorker::testing::setStartedHook([&](qint64 id) { pid = id; });
    int commits = 0;
    job::testing::failWhen([&](job::Step step, const QString &) {
        if (step == job::Step::Writing && commits == 1) {
            killWorker(pid);
            return false;
        }
        return false;
    });
    QStringList outputs;
    QString error;
    QCOMPARE(PageOps::split(source, folder.path(), "part", {}, &outputs, &error,
        [&](const QStringList &paths) { return paths.size() == 3; },
        [&](const QString &) { ++commits; return true; }), Status::Failed);
    QCOMPARE(outputs, QStringList{first});
    QVERIFY(read(first).startsWith("%PDF-"));
    QCOMPARE(read(second), QByteArray("previous second"));
    QVERIFY(!QFileInfo::exists(folder.filePath("part-003.pdf")));
    QCOMPARE(error, qpdfWorker::failureText());
}

/// The process memory bound rejects a codec that has no per-Flate decoder limit.
void TstQpdfWorker::nonFlateMemoryLimit()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("runlength.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, runLengthPdf()));
    QVERIFY(save(output, "previous output"));
    QString error;
    job::State state;
    auto task = std::async(std::launch::async, [&] {
        job::Scope scope(state);
        return QpdfService().decrypt(source, output, {}, &error);
    });
    const auto stop = qScopeGuard([&] { state.stop(); });
    QPushButton button("Responsive during memory exhaustion");
    button.show();
    int clicks = 0, ticks = 0;
    connect(&button, &QPushButton::clicked, [&] { ++clicks; });
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start(10);
    QTRY_VERIFY(ticks > 0);
    QTest::mouseClick(&button, Qt::LeftButton);
    QCOMPARE(clicks, 1);
    QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
    QCOMPARE(task.get(), Status::Failed);
    QVERIFY(ticks > 1);
    QCOMPARE(error, qpdfWorker::failureText());
    QVERIFY2(!error.contains("PL_Flate"), qPrintable(error));
    QCOMPARE(read(output), QByteArray("previous output"));
    QVERIFY(save(source, plainPdf()));
    QCOMPARE(QpdfService().decrypt(source, output, {}, &error), Status::Ok);
}

/// Request and Split admission limits preserve outputs and reject traversal names.
void TstQpdfWorker::requestAndSplitBounds()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    QVERIFY(save(output, "previous output"));
    QString error;
    QCOMPARE(QpdfService().decrypt(source, output, QString(qpdfWorker::kMaxRequestBytes, QChar('p')), &error), Status::Failed);
    QCOMPARE(error, qpdfWorker::failureText());
    QCOMPARE(read(output), QByteArray("previous output"));
    int consent = 0;
    QStringList paths;
    QCOMPARE(PageOps::split(source, folder.path(), "../escape", {}, &paths, &error,
                            [&](const QStringList &) { ++consent; return true; }), Status::Failed);
    QCOMPARE(consent, 0);
    QVERIFY(paths.isEmpty());
}

#ifdef Q_OS_LINUX
/// Descriptor exhaustion keeps source attribution and the visible resource reason.
void TstQpdfWorker::descriptorLimitIsVisible()
{
    QTemporaryDir folder;
    QList<PageOps::MergeInput> inputs;
    for (int i = 0; i < 72; ++i) {
        const QString path = folder.filePath(QStringLiteral("source-%1.pdf").arg(i));
        QVERIFY(save(path, plainPdf(1)));
        inputs.append({path, {}, {}});
    }
    const QString output = folder.filePath("out.pdf");
    QVERIFY(save(output, "previous output"));
    QString error;
    int failedIndex = -1;
    QCOMPARE(PageOps::merge(inputs, output, &error, &failedIndex), Status::Failed);
    QCOMPARE(error, qpdfWorker::failureText());
    QVERIFY(failedIndex >= 0 && failedIndex < inputs.size());
    QCOMPARE(read(output), QByteArray("previous output"));
}

/// A stopped helper cannot evade the parent watchdog by consuming no CPU.
void TstQpdfWorker::stalledWorkerDeadline()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    QVERIFY(save(output, "previous output"));
    qpdfWorker::testing::setBounds(0, 300);
    qpdfWorker::testing::setStartedHook([](qint64 pid) { QVERIFY(::kill(pid_t(pid), SIGSTOP) == 0); });
    QString error;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(QpdfService().decrypt(source, output, {}, &error), Status::Failed);
    QVERIFY(timer.elapsed() < 3000);
    QCOMPARE(error, qpdfWorker::failureText());
    QCOMPARE(read(output), QByteArray("previous output"));
}

/// Tightens the real installed CPU budget to keep the enforcement check short.
void TstQpdfWorker::cpuLimit()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("cpu.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf(3, 3000000)));
    QVERIFY(save(output, "previous output"));
    bool limited = false;
    qpdfWorker::testing::setStartedHook([&](qint64 pid) {
        const rlimit limit{1, 1};
        limited = ::prlimit(pid_t(pid), RLIMIT_CPU, &limit, nullptr) == 0;
    });
    QString error;
    QCOMPARE(QpdfService().decrypt(source, output, {}, &error), Status::Failed);
    QVERIFY(limited);
    QCOMPARE(error, qpdfWorker::failureText());
    QCOMPARE(read(output), QByteArray("previous output"));
}
#endif

/// A tightened output budget tests the shipped byte-count check without a 1 GiB fixture.
void TstQpdfWorker::outputBound()
{
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf"), output = folder.filePath("out.pdf");
    QVERIFY(save(source, plainPdf()));
    QVERIFY(save(output, "previous output"));
    qpdfWorker::testing::setBounds(64);
    QString error;
    QCOMPARE(QpdfService().decrypt(source, output, {}, &error), Status::Failed);
    QCOMPARE(error, qpdfWorker::failureText());
    QCOMPARE(read(output), QByteArray("previous output"));
    qpdfWorker::testing::setBounds();
    QCOMPARE(QpdfService().decrypt(source, output, {}, &error), Status::Ok);
}

QTEST_MAIN(TstQpdfWorker)
#include "tst_qpdf_worker.moc"
