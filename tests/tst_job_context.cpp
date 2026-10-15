#include "io/JobContext.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace mervin;

namespace {

// For waits that should succeed. The tests release every hold before they end.
constexpr int kWaitMs = 10000;

const QByteArray kOriginal("existing file must survive");

// The two writers that replace document files.
enum class Writer
{
    Pdf,     // AtomicPdfWriter, through PageOps
    Replace, // DocumentOutput::replace
};

// The code that opens documents, each through its own open.
enum class Opener
{
    PageOps,
    QpdfService,
    MeasureExport,
    RenderEngine,
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Every file in `dir`, temporary siblings included.
QStringList entries(const QTemporaryDir &dir)
{
    return QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
}

QByteArray versionBytes(int n)
{
    return QByteArray("writer version ") + QByteArray::number(n);
}

// Writes version `n` of a document to `destination`. The PDF writer's version has n
// pages; replace copies the bytes of a source it creates in `sources`.
bool writeVersion(Writer writer, const QTemporaryDir &sources, const QString &destination, int n)
{
    if (writer == Writer::Pdf) {
        QList<int> pages;
        for (int i = 0; i < n; ++i)
            pages.append(i);
        return PageOps::merge(QList<PageOps::MergeInput>{{QStringLiteral(MERVIN_FIXTURE_PDF), pages, {}}},
                              destination)
               == PageOps::Status::Ok;
    }
    const QString source = sources.filePath(QStringLiteral("version-%1.pdf").arg(n));
    return writeFile(source, versionBytes(n)) && DocumentOutput::replace(source, destination, nullptr);
}

// Which version writeVersion() left at `path`, or 0.
int versionAt(Writer writer, const QString &path)
{
    if (writer == Writer::Pdf)
        return std::max(0, PageOps::pageCount(path));
    const QByteArray bytes = readFile(path);
    for (int n = 1; n <= 2; ++n)
        if (bytes == versionBytes(n))
            return n;
    return 0;
}

// Holds the first Writing report for `folder` only, so a later writer of the same
// folder is not held by the hook.
void holdFirstWrite(const QString &folder)
{
    job::testing::holdWhen([folder, first = true](job::Step step, const QString &path) mutable {
        if (step != job::Step::Writing || path != folder || !first)
            return false;
        first = false;
        return true;
    });
}

// Runs `work` on its own thread in a scope for `state`, as a tab's job does. Releases
// the holds and joins the thread when destroyed, so a failed check cannot leave it
// running.
class JobThread
{
public:
    JobThread(job::State &state, std::function<bool()> work)
    {
        std::promise<bool> promise;
        result_ = promise.get_future();
        thread_ = std::thread([&state, work = std::move(work), promise = std::move(promise)]() mutable {
            bool ok = false;
            {
                job::Scope scope(state);
                ok = work();
            }
            promise.set_value(ok);
        });
    }
    ~JobThread()
    {
        job::testing::release();
        thread_.join();
    }

    // Whether the job ended within `ms` milliseconds.
    bool endsWithin(int ms)
    {
        return result_.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
    }
    // Waits for the job and returns whether its work succeeded.
    bool result() { return result_.get(); }

private:
    std::future<bool> result_;
    std::thread thread_;
};

#ifdef Q_OS_UNIX
// Opens the write end of the FIFO at `path` once `job` opened its read end, as a
// blocking descriptor. -1 if the job ended first or did not open it in time.
int openWriteEnd(const QString &path, JobThread &job)
{
    const QByteArray name = QFile::encodeName(path);
    const QDeadlineTimer deadline(kWaitMs);
    while (!deadline.hasExpired() && !job.endsWithin(10)) {
        const int fd = ::open(name.constData(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            return fd;
        }
    }
    return -1;
}

// Writes all of `bytes` to `fd`. False on an error, such as a pipe whose reader left.
bool writeAll(int fd, const QByteArray &bytes)
{
    qsizetype done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        done += count;
    }
    return true;
}
#endif

// Opens `path` with `opener`. Whether it opened.
bool openWith(Opener opener, const QString &path)
{
    switch (opener) {
    case Opener::PageOps: {
        int count = 0;
        return PageOps::probe(path, &count) == PageOps::Status::Ok;
    }
    case Opener::QpdfService: {
        QpdfService::Info info;
        return QpdfService().readInfo(path, {}, info) == QpdfService::Status::Ok;
    }
    case Opener::MeasureExport: {
        QTemporaryDir dir;
        return MeasureExport::flatten(path, dir.filePath(QStringLiteral("out.pdf")), {})
               == MeasureExport::Status::Ok;
    }
    case Opener::RenderEngine: {
        RenderEngine engine;
        return engine.openDocument(path) != nullptr;
    }
    }
    return false;
}

} // namespace

Q_DECLARE_METATYPE(Writer)
Q_DECLARE_METATYPE(Opener)

// Checks job cancellation, document reservations and destination ordering.
class TstJobContext : public QObject
{
    Q_OBJECT
private slots:
    // Checks in-place output is reserved only for its sole captured tab admission.
    void inPlaceSaveRequiresItsCapturedOwner()
    {
        const QString key = QStringLiteral("/out/owned.pdf");
        auto owner = std::make_unique<job::OpenPath>(key);
        QVERIFY(owner->admitted());
        const auto identity = owner->identity();
        QVERIFY(identity != 0);
        job::State state;
        const job::Scope scope(state);
        {
            const job::OpenPath independent(key);
            QVERIFY(independent.admitted());
            QVERIFY(!job::reserveOwnedDestination(key, identity));
        }
        QVERIFY(!job::destinationInUse(key));
        QVERIFY(!job::reserveOwnedDestination(key, identity + 1));
        QVERIFY(job::reserveOwnedDestination(key, identity));
        QVERIFY(job::destinationInUse(key));
        const job::OpenPath whileSaving(key);
        QVERIFY(!whileSaving.admitted());
        state.stop();
        QVERIFY(!job::destinationInUse(key));
        owner.reset();
        const job::OpenPath replacement(key);
        QVERIFY(replacement.admitted());
        QVERIFY(replacement.identity() != identity);
        job::State later;
        const job::Scope laterScope(later);
        QVERIFY(!job::reserveOwnedDestination(key, identity));
        QVERIFY(job::reserveOwnedDestination(key, replacement.identity()));
    }

    // Checks canonical admission remains protected while write reservations are released.
    void canonicalAdmissionProtectsTheGapAfterWorkerScope()
    {
        const QString original = QStringLiteral("/out/original.pdf");
        const QString canonical = QStringLiteral("/out/retargeted.pdf");
        const job::OpenPath owner(original);
        QVERIFY(owner.admitted());
        job::State save;
        std::unique_ptr<job::OpenPath> result;
        {
            const job::Scope scope(save);
            QVERIFY(job::reserveOwnedDestination(original, owner.identity()));
            QVERIFY(job::reserveDestinations({canonical}));
            QVERIFY(!save.admitReservedDestination(canonical, original, owner.identity() + 1));
            result = save.admitReservedDestination(canonical, original, owner.identity());
            QVERIFY(result && result->admitted());
        }
        // The worker has returned but GUI installation has not run. A new output
        // job must still be refused, even though the reservation itself ended.
        QVERIFY(!job::destinationInUse(canonical));
        job::State later;
        const job::Scope laterScope(later);
        QVERIFY(!job::reserveDestinations({canonical}));
        result.reset();
        QVERIFY(job::reserveDestinations({canonical}));
    }

    // Checks transferring a reserved key refuses competing writers.
    void canonicalAdmissionRefusesAnotherWriter()
    {
        const QString original = QStringLiteral("/out/owner.pdf");
        const QString canonical = QStringLiteral("/out/contended.pdf");
        const job::OpenPath owner(original);
        job::State other;
        const job::Scope otherScope(other);
        QVERIFY(job::reserveDestinations({canonical}));
        job::State save;
        const job::Scope saveScope(save);
        QVERIFY(job::reserveOwnedDestination(original, owner.identity()));
        QVERIFY(job::reserveDestinations({canonical}));
        QVERIFY(!save.admitReservedDestination(canonical, original, owner.identity()));
        other.stop();
        QVERIFY(save.admitReservedDestination(canonical, original, owner.identity()));
    }

    void tabAdmissionAndOutputReservations()
    {
        const QSet<QString> keys{QStringLiteral("/out/one.pdf"), QStringLiteral("/out/two.pdf")};
        job::State state;
        const job::Scope scope(state);
        {
            const job::OpenPath open(QStringLiteral("/out/two.pdf"));
            QVERIFY(open.admitted());
            QVERIFY(!job::reserveDestinations(keys));
            const job::OpenPath other(QStringLiteral("/out/one.pdf"));
            QVERIFY(other.admitted()); // failure reserves none of the outputs
        }
        QVERIFY(job::reserveDestinations(keys));
        const job::OpenPath refused(QStringLiteral("/out/two.pdf"));
        QVERIFY(!refused.admitted());
        QCOMPARE(state.stop(), job::State::Stop::Stopped);
        const job::OpenPath afterStop(QStringLiteral("/out/two.pdf"));
        QVERIFY(afterStop.admitted());
        QVERIFY(!job::reserveDestinations(keys)); // Stop wins over a late reservation
    }

    void committingReservationLastsUntilCommitReturns()
    {
        const QString path = QStringLiteral("/out/committing.pdf");
        job::State state;
        const job::Scope scope(state);
        QVERIFY(job::reserveDestinations({path}));
        QVERIFY(state.beginCommit());
        QCOMPARE(state.stop(), job::State::Stop::Committing);
        const job::OpenPath duringCommit(path);
        QVERIFY(!duringCommit.admitted());
        state.endCommit();
        const job::OpenPath afterCommit(path);
        QVERIFY(afterCommit.admitted());
    }

    void completedJobReleasesOutputReservations()
    {
        const QString path = QStringLiteral("/out/completed.pdf");
        job::State state;
        {
            const job::Scope scope(state);
            QVERIFY(job::reserveDestinations({path}));
            const job::OpenPath duringWork(path);
            QVERIFY(!duringWork.admitted());
        }
        const job::OpenPath afterWork(path);
        QVERIFY(afterWork.admitted());
    }
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
    }

    void stopAnswersWhatTheJobIsDoing();
    void printerCommitKeepsItsStep();
    void scopesNest();
    void switchToReportsOnlyAChange();
    void stoppedAtTheCommitStepLeavesTheDestination_data() { writers(); }
    void stoppedAtTheCommitStepLeavesTheDestination();
    void outsideAScopeWritesCommit_data() { writers(); }
    void outsideAScopeWritesCommit();
    void stopWhileCommittingLetsTheCommitFinish_data() { writers(); }
    void stopWhileCommittingLetsTheCommitFinish();
    void laterWriterWaitsForTheStoppedOne_data() { writers(); }
    void laterWriterWaitsForTheStoppedOne();
    void laterWriterWaitsForACommitThatBegan_data() { writers(); }
    void laterWriterWaitsForACommitThatBegan();
    void differentFilesDoNotWait_data() { writers(); }
    void differentFilesDoNotWait();
#ifdef Q_OS_WIN
    void differentlyCasedNamesWaitForEachOther();
#endif
    void waitingWriterGivesUpWhenStopped_data() { writers(); }
    void waitingWriterGivesUpWhenStopped();
    void splitStoppedAfterTwoPagesLeavesTwoFiles();
    void ticksAdvanceWhileWriting_data() { writers(); }
    void ticksAdvanceWhileWriting();
    void stopWhileTheWriteReadsItsSource();
#ifdef Q_OS_UNIX
    void stoppedCopyGivesUpAtItsNextChunk();
#endif
    void readsAreReported_data();
    void readsAreReported();
    void stepsOnTheGuiThreadAreCounted();

private:
    static void writers()
    {
        QTest::addColumn<Writer>("writer");
        QTest::newRow("pdf writer") << Writer::Pdf;
        QTest::newRow("replace") << Writer::Replace;
    }
};

// Checks printer completion keeps its reported step and rejects commits after Stop.
void TstJobContext::printerCommitKeepsItsStep()
{
    job::State state;
    const job::Scope scope(state);
    const QString printer = QStringLiteral("Office Laser");
    QVERIFY(job::beginPrintingCommit(printer));
    QCOMPARE(state.snapshot().step, job::Step::Printing);
    QCOMPARE(state.snapshot().path, printer);
    QCOMPARE(state.stop(), job::State::Stop::Committing);
    job::endCommit();
    QVERIFY(!job::beginPrintingCommit(printer));
}

// Checks Stop outcomes and rejects document reservations after cancellation.
void TstJobContext::stopAnswersWhatTheJobIsDoing()
{
    job::State holding;
    QVERIFY(holding.beginHolding());
    QCOMPARE(holding.stop(), job::State::Stop::Holding);
    holding.setHolding(false);
    QCOMPARE(holding.stop(), job::State::Stop::Stopped);
    QVERIFY(holding.stopped());
    QVERIFY(!holding.beginCommit());
    QVERIFY(!holding.beginHolding());

    job::State committing;
    QVERIFY(committing.beginCommit());
    QCOMPARE(committing.stop(), job::State::Stop::Committing);
    committing.endCommit();
    QCOMPARE(committing.stop(), job::State::Stop::Stopped);

    job::State done;
    {
        job::Scope scope(done);
        QVERIFY(!job::stopped());
    }
    QCOMPARE(done.stop(), job::State::Stop::Done);
    QVERIFY(done.snapshot().stopped);
}

void TstJobContext::scopesNest()
{
    job::State outer, inner;
    {
        job::Scope outerScope(outer);
        {
            job::Scope innerScope(inner);
            inner.stop();
            QVERIFY(job::stopped());
        }
        // The outer state is current again.
        QVERIFY(!job::stopped());
        QCOMPARE(outer.stop(), job::State::Stop::Stopped);
        QVERIFY(job::stopped());
    }
    QVERIFY(!job::stopped());
    QCOMPARE(outer.stop(), job::State::Stop::Done);
}

void TstJobContext::switchToReportsOnlyAChange()
{
    const QString file = QStringLiteral("source.pdf");
    const QString folder = QStringLiteral("out");
    job::State outer, inner;
    job::Scope outerScope(outer);
    const int before = job::testing::stepsOnMainThread();
    job::switchTo(job::Step::Writing, folder);
    job::switchTo(job::Step::Writing, folder);
    QCOMPARE(job::testing::stepsOnMainThread(), before + 1);
    {
        // A new scope hears the step that the thread reported last.
        job::Scope innerScope(inner);
        job::switchTo(job::Step::Writing, folder);
        QCOMPARE(inner.snapshot().step, job::Step::Writing);
        job::switchTo(job::Step::Reading, file);
    }
    // So does the outer one once the inner one ended.
    job::switchTo(job::Step::Reading, file);
    QCOMPARE(outer.snapshot().step, job::Step::Reading);
    QCOMPARE(outer.snapshot().path, file);
}

void TstJobContext::stoppedAtTheCommitStepLeavesTheDestination()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    job::testing::holdWhen([path](job::Step step, const QString &file) {
        return step == job::Step::Commit && file == path;
    });
    job::State state;
    JobThread job(state, [&] { return writeVersion(writer, sources, path, 2); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    QCOMPARE(state.snapshot().step, job::Step::Commit);
    QCOMPARE(state.stop(), job::State::Stop::Stopped);
    job::testing::release();
    QVERIFY(!job.result());
    QCOMPARE(readFile(path), kOriginal);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
}

void TstJobContext::outsideAScopeWritesCommit()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    // A stopped job on no thread's scope changes nothing here.
    job::State elsewhere;
    elsewhere.stop();
    QVERIFY(!job::stopped());
    QVERIFY(writeVersion(writer, sources, path, 2));
    QCOMPARE(versionAt(writer, path), 2);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
    QVERIFY(job::beginCommit(path));
    job::endCommit();
    QCOMPARE(elsewhere.snapshot().step, job::Step::None);
}

void TstJobContext::stopWhileCommittingLetsTheCommitFinish()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    job::testing::holdCommitWhen([path](const QString &file) { return file == path; });
    job::State state;
    JobThread job(state, [&] { return writeVersion(writer, sources, path, 2); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    QCOMPARE(state.stop(), job::State::Stop::Committing);
    job::testing::release();
    QVERIFY(job.result());
    QCOMPARE(versionAt(writer, path), 2);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
    QCOMPARE(state.stop(), job::State::Stop::Done);
}

void TstJobContext::laterWriterWaitsForTheStoppedOne()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    const QString folder = QFileInfo(path).absolutePath();
    holdFirstWrite(folder);
    job::State first, second;
    JobThread one(first, [&] { return writeVersion(writer, sources, path, 1); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    QCOMPARE(first.stop(), job::State::Stop::Stopped);

    // The second writer cannot finish while the first one owns the file, and waits
    // for it at the destination.
    JobThread two(second, [&] { return writeVersion(writer, sources, path, 2); });
    QVERIFY(!two.endsWithin(300));
    QTRY_COMPARE_WITH_TIMEOUT(second.snapshot().step, job::Step::Writing, kWaitMs);
    QCOMPARE(second.snapshot().path, folder);
    QCOMPARE(job::testing::held(), 1);

    job::testing::release();
    QVERIFY(!one.result());
    QVERIFY(two.result());
    QCOMPARE(versionAt(writer, path), 2);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
}

void TstJobContext::laterWriterWaitsForACommitThatBegan()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    job::testing::holdCommitWhen([path, first = true](const QString &file) mutable {
        if (file != path || !first)
            return false;
        first = false;
        return true;
    });
    job::State first, second;
    JobThread one(first, [&] { return writeVersion(writer, sources, path, 1); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    QCOMPARE(first.stop(), job::State::Stop::Committing);

    // The first commit still replaces the file, so a retry must not finish before it.
    JobThread two(second, [&] { return writeVersion(writer, sources, path, 2); });
    QVERIFY(!two.endsWithin(300));
    QTRY_COMPARE_WITH_TIMEOUT(second.snapshot().step, job::Step::Writing, kWaitMs);

    job::testing::release();
    QVERIFY(one.result());
    QVERIFY(two.result());
    QCOMPARE(versionAt(writer, path), 2);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
}

void TstJobContext::differentFilesDoNotWait()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString first = dir.filePath(QStringLiteral("first.pdf"));
    const QString second = dir.filePath(QStringLiteral("second.pdf"));
    holdFirstWrite(QFileInfo(first).absolutePath());
    job::State holder, other;
    JobThread one(holder, [&] { return writeVersion(writer, sources, first, 1); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);

    JobThread two(other, [&] { return writeVersion(writer, sources, second, 2); });
    QVERIFY(two.endsWithin(kWaitMs));
    QVERIFY(two.result());
    QCOMPARE(versionAt(writer, second), 2);
    QCOMPARE(job::testing::held(), 1);

    job::testing::release();
    QVERIFY(one.result());
    QCOMPARE(versionAt(writer, first), 1);
}

#ifdef Q_OS_WIN
void TstJobContext::differentlyCasedNamesWaitForEachOther()
{
    // Windows compares file names without case, so both name one file.
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    holdFirstWrite(QFileInfo(path).absolutePath());
    job::State first, second;
    JobThread one(first, [&] { return writeVersion(Writer::Replace, sources, path, 1); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);

    const QString shouted = dir.filePath(QStringLiteral("RESULT.PDF"));
    JobThread two(second, [&] { return writeVersion(Writer::Replace, sources, shouted, 2); });
    QVERIFY(!two.endsWithin(300));
    QTRY_COMPARE_WITH_TIMEOUT(second.snapshot().step, job::Step::Writing, kWaitMs);

    job::testing::release();
    QVERIFY(one.result());
    QVERIFY(two.result());
    QCOMPARE(versionAt(Writer::Replace, path), 2);
    QCOMPARE(entries(dir).size(), 1);
}
#endif

void TstJobContext::waitingWriterGivesUpWhenStopped()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    holdFirstWrite(QFileInfo(path).absolutePath());
    job::State first, second;
    JobThread one(first, [&] { return writeVersion(writer, sources, path, 1); });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);

    JobThread two(second, [&] { return writeVersion(writer, sources, path, 2); });
    QVERIFY(!two.endsWithin(100));
    QTRY_COMPARE_WITH_TIMEOUT(second.snapshot().step, job::Step::Writing, kWaitMs);
    QCOMPARE(second.stop(), job::State::Stop::Stopped);
    QVERIFY(two.endsWithin(200));
    QVERIFY(!two.result());
    QCOMPARE(readFile(path), kOriginal);

    // The first writer was not stopped, so it still replaces the file.
    job::testing::release();
    QVERIFY(one.result());
    QCOMPARE(versionAt(writer, path), 1);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
}

void TstJobContext::splitStoppedAfterTwoPagesLeavesTwoFiles()
{
    QTemporaryDir dir;
    QCOMPARE(PageOps::pageCount(QStringLiteral(MERVIN_FIXTURE_PDF)), 4);
    job::testing::holdWhen([](job::Step step, const QString &file) {
        return step == job::Step::Commit && file.endsWith(QStringLiteral("part-003.pdf"));
    });
    job::State state;
    QStringList written;
    JobThread job(state, [&] {
        return PageOps::split(QStringLiteral(MERVIN_FIXTURE_PDF), dir.path(), QStringLiteral("part"),
                              {}, &written)
               == PageOps::Status::Ok;
    });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    QCOMPARE(state.stop(), job::State::Stop::Stopped);
    job::testing::release();
    QVERIFY(!job.result());
    const QStringList kept{QStringLiteral("part-001.pdf"), QStringLiteral("part-002.pdf")};
    QCOMPARE(entries(dir), kept);
    QCOMPARE(written, (QStringList{dir.filePath(kept[0]), dir.filePath(kept[1])}));
}

void TstJobContext::ticksAdvanceWhileWriting()
{
    QFETCH(Writer, writer);
    QTemporaryDir sources, dir;
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    // Replace ticks once per 1 MiB chunk: four for 3.5 MiB.
    const QString large = sources.filePath(QStringLiteral("large.pdf"));
    QVERIFY(writeFile(large, QByteArray(7 * 512 * 1024, 'x')));
    job::testing::holdWhen([path](job::Step step, const QString &file) {
        return step == job::Step::Commit && file == path;
    });
    job::State state;
    JobThread job(state, [&] {
        return writer == Writer::Pdf ? writeVersion(writer, sources, path, 2)
                                     : DocumentOutput::replace(large, path, nullptr);
    });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    const job::State::Snapshot during = state.snapshot();
    QCOMPARE(during.step, job::Step::Commit);
    QCOMPARE(during.path, path);
    if (writer == Writer::Replace)
        QCOMPARE(during.ticks, quint64(4));
    else
        QVERIFY(during.ticks > 0);
    job::testing::release();
    QVERIFY(job.result());
}

void TstJobContext::stopWhileTheWriteReadsItsSource()
{
    // qpdf reads most of its source while it writes, as here the pages' content.
    QTemporaryDir sources, dir;
    const QString source = sources.filePath(QStringLiteral("source.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    const QString folder = QFileInfo(path).absolutePath();
    // Holds the first read of the source after the writer reported its folder.
    job::testing::holdWhen([source, folder, writing = false](job::Step step, const QString &file) mutable {
        writing = writing || (step == job::Step::Writing && file == folder);
        return writing && step == job::Step::Reading && file == source;
    });
    job::State state;
    JobThread job(state, [&] {
        return PageOps::rotatePages(source, path, {0}, 90, true, {}, nullptr) == PageOps::Status::Ok;
    });
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, kWaitMs);
    const job::State::Snapshot during = state.snapshot();
    QCOMPARE(during.step, job::Step::Reading);
    QCOMPARE(during.path, source);

    // Stopped there, the writer reports its folder again at its next write and gives
    // up before it writes anything more.
    QCOMPARE(state.stop(), job::State::Stop::Stopped);
    job::testing::release();
    QVERIFY(!job.result());
    const job::State::Snapshot after = state.snapshot();
    QCOMPARE(after.step, job::Step::Writing);
    QCOMPARE(after.path, folder);
    QCOMPARE(after.ticks, during.ticks);
    QVERIFY(entries(dir).isEmpty());
}

#ifdef Q_OS_UNIX
void TstJobContext::stoppedCopyGivesUpAtItsNextChunk()
{
    // A write to a pipe whose reader left must fail instead of ending the test.
    const auto previous = std::signal(SIGPIPE, SIG_IGN);
    const auto restore = qScopeGuard([previous] { std::signal(SIGPIPE, previous); });
    QTemporaryDir sources, dir;
    const QString source = sources.filePath(QStringLiteral("slow.pdf"));
    QCOMPARE(::mkfifo(QFile::encodeName(source).constData(), 0600), 0);
    const QString path = dir.filePath(QStringLiteral("result.pdf"));
    QVERIFY(writeFile(path, kOriginal));
    job::State state;
    JobThread job(state, [&] { return DocumentOutput::replace(source, path, nullptr); });
    const int pipe = openWriteEnd(source, job);
    // Closed before the job is joined, so a copy that waits for more ends then.
    const auto closePipe = qScopeGuard([pipe] {
        if (pipe >= 0)
            ::close(pipe);
    });
    QVERIFY(pipe >= 0);

    // The source answers one chunk, and the job is stopped while it waits for more.
    const QByteArray chunk(1024 * 1024, 'x');
    QVERIFY(writeAll(pipe, chunk));
    QTRY_COMPARE_WITH_TIMEOUT(state.snapshot().ticks, quint64(1), kWaitMs);
    QCOMPARE(state.stop(), job::State::Stop::Stopped);
    // One more chunk, with the source still open. Writing it fails if the copy saw the
    // stop before it read again. Either way the copy must end without the rest.
    writeAll(pipe, chunk);
    QVERIFY(job.endsWithin(kWaitMs));
    QVERIFY(!job.result());
    QCOMPARE(readFile(path), kOriginal);
    QCOMPARE(entries(dir), QStringList{QStringLiteral("result.pdf")});
}
#endif

void TstJobContext::readsAreReported_data()
{
    QTest::addColumn<Opener>("opener");
    QTest::newRow("page operations") << Opener::PageOps;
    QTest::newRow("security") << Opener::QpdfService;
    QTest::newRow("measurement export") << Opener::MeasureExport;
    QTest::newRow("render engine") << Opener::RenderEngine;
}

void TstJobContext::readsAreReported()
{
    QFETCH(Opener, opener);
    const QString file = QStringLiteral(MERVIN_FIXTURE_PDF);
    QVERIFY(openWith(opener, file));
    job::testing::failWhen([file](job::Step step, const QString &path) {
        return step == job::Step::Reading && path == file;
    });
    QVERIFY(!openWith(opener, file));
    job::testing::failWhen({});

    job::State state;
    job::Scope scope(state);
    QVERIFY(openWith(opener, file));
    state.stop();
    QVERIFY(!openWith(opener, file));
}

void TstJobContext::stepsOnTheGuiThreadAreCounted()
{
    const QString file = QStringLiteral(MERVIN_FIXTURE_PDF);
    const int before = job::testing::stepsOnMainThread();
    QCOMPARE(PageOps::pageCount(file), 4);
    // One step for the open. The reads it makes continue that step.
    QCOMPARE(job::testing::stepsOnMainThread(), before + 1);
    int pages = 0;
    std::thread([&] { pages = PageOps::pageCount(file); }).join();
    QCOMPARE(pages, 4);
    QCOMPARE(job::testing::stepsOnMainThread(), before + 1);
}

QTEST_GUILESS_MAIN(TstJobContext)
#include "tst_job_context.moc"
