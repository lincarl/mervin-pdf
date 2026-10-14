#include "io/JobContext.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

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

class TstJobContext : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
    }

    void stopAnswersWhatTheJobIsDoing();
    void stoppedAtTheCommitStepLeavesTheDestination_data() { writers(); }
    void stoppedAtTheCommitStepLeavesTheDestination();
    void outsideAScopeWritesCommit_data() { writers(); }
    void outsideAScopeWritesCommit();
    void stopWhileCommittingLetsTheCommitFinish_data() { writers(); }
    void stopWhileCommittingLetsTheCommitFinish();
    void laterWriterWaitsForTheStoppedOne_data() { writers(); }
    void laterWriterWaitsForTheStoppedOne();
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

void TstJobContext::stopAnswersWhatTheJobIsDoing()
{
    job::State holding;
    holding.setHolding(true);
    QCOMPARE(holding.stop(), job::State::Stop::Holding);
    holding.setHolding(false);
    QCOMPARE(holding.stop(), job::State::Stop::Stopped);
    QVERIFY(holding.stopped());
    QVERIFY(!holding.beginCommit());

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
    QCOMPARE(job::testing::stepsOnMainThread(), before + 1);
    int pages = 0;
    std::thread([&] { pages = PageOps::pageCount(file); }).join();
    QCOMPARE(pages, 4);
    QCOMPARE(job::testing::stepsOnMainThread(), before + 1);
}

QTEST_GUILESS_MAIN(TstJobContext)
#include "tst_job_context.moc"
