#include "render/Document.h"
#include "render/DocumentSearch.h"
#include "render/RenderEngine.h"
#include "render/TextIndex.h"
#include "Stalls.h"

#include <QElapsedTimer>
#include <QSet>
#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

using namespace mervin;
using mervin::testing::Stall;

namespace {

bool closed(const std::shared_ptr<DocumentLifetime> &lifetime)
{
    std::lock_guard lock(lifetime->mutex);
    return !lifetime->document;
}

// Pages rendered without error for `requester` so far.
QSet<int> renderedPages(const QSignalSpy &results, quint64 requester)
{
    QSet<int> pages;
    for (const auto &args : results) {
        const auto result = args[0].value<RenderResult>();
        if (result.requester == requester && result.ok)
            pages.insert(result.pageNo);
    }
    return pages;
}

void submitAllPages(RenderEngine &engine, Document &document, quint64 requester)
{
    for (int page = 0; page < document.pageCount(); ++page) {
        RenderRequest request;
        request.document = &document;
        request.requester = requester;
        request.pageNo = page;
        engine.submit(request);
    }
}

// Takes a document the way a save job does, as a lockable for Stall. On the Stall's
// thread it takes Document::Hold, then the access lock, which savePdfTo keeps while
// it writes.
struct SavingJob
{
    std::shared_ptr<DocumentLifetime> lifetime;
    std::optional<Document::Hold> hold;
    bool answered = false; // the job holds the document and its own call was answered

    void lock()
    {
        hold.emplace(lifetime);
        Document *document = hold->document();
        answered = document && document->isPdf();
        if (document)
            document->accessMutex().lock();
    }
    void unlock()
    {
        if (Document *document = hold->document())
            document->accessMutex().unlock();
        hold.reset();
    }
};

// Tells QTest to expect the warning that a refused screen-thread call logs.
void expectRefusal(const char *method)
{
    const QByteArray warning = QByteArray("Mervin: ") + method
                               + " refused on the screen thread while a job holds the document.";
    QTest::ignoreMessage(QtWarningMsg, warning.constData());
}

QString took(const char *call, const QElapsedTimer &timer)
{
    return QStringLiteral("%1 took %2 ms").arg(QLatin1String(call)).arg(timer.elapsed());
}

} // namespace

class TstRenderLifetime : public QObject
{
    Q_OBJECT
private slots:
    void closeWhileJobsAreQueued()
    {
        RenderEngine engine;
        for (int i = 0; i < 100; ++i) {
            auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
            QVERIFY(doc);
            auto lifetime = doc->lifetime();
            for (int page = 0; page < doc->pageCount(); ++page) {
                RenderRequest request;
                request.document = doc.get();
                request.requester = i + 1;
                request.pageNo = page;
                request.scale = 2;
                engine.submit(request);
            }
            doc.reset();
            std::lock_guard lock(lifetime->mutex);
            QVERIFY(!lifetime->document);
        }
        // The engine continues rendering after individual documents close.
        auto survivor = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        RenderRequest request;
        request.document = survivor.get();
        request.requester = 1000;
        engine.submit(request);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto &args : results)
                if (args[0].value<RenderResult>().requester == 1000)
                    return args[0].value<RenderResult>().ok;
            return false;
        }(), 5000);
    }

    void canceledJobsDoNotRasterize()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        {
            std::lock_guard gate(doc->lifetime()->mutex);
            RenderRequest request;
            request.document = doc.get();
            request.requester = 1;
            engine.submit(request);
            engine.cancelRequests(1);
        }
        engine.shutdown();
        QCoreApplication::processEvents();
        QCOMPARE(results.size(), 0);
    }

    void closingDoesNotWaitForAStuckRender()
    {
        auto engine = std::make_unique<RenderEngine>();
        auto doc = engine->openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        Stall stall(lifetime->mutex);
        RenderRequest request;
        request.document = doc.get();
        request.requester = 1;
        engine->submit(request);
        QTest::qWait(200); // a worker takes the request and waits for the document

        QElapsedTimer timer;
        timer.start();
        engine.reset();
        Document::closeInBackground(std::move(doc));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
    }

    // Quitting waits for a close whose follow-up removes a file, such as a tab's
    // recovery copy, and not for one without.
    void closesWithFollowUpsCanBeAwaited()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto release = std::make_shared<std::atomic<bool>>(false);
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        Document::closeInBackground(std::move(doc), [release, ran] {
            while (!release->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ran->store(true);
        });
        QVERIFY(!Document::waitForCloses(100));
        release->store(true);
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(ran->load());

        auto other = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(other);
        const auto lifetime = other->lifetime();
        Stall stall(lifetime->mutex);
        Document::closeInBackground(std::move(other));
        QVERIFY(Document::waitForCloses(100));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
    }

    void closingDoesNotWaitForAStuckSearch()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        auto search = std::make_unique<DocumentSearch>(&engine);
        Stall stall(lifetime->mutex);
        search->start(doc.get(), QStringLiteral("Test"), false, false, [](std::vector<TextMatch>) {
            QFAIL("a canceled search reported matches");
        });
        QTest::qWait(200); // the search starts on the first page and waits for the document

        QElapsedTimer timer;
        timer.start();
        search.reset();
        Document::closeInBackground(std::move(doc));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("closing took %1 ms")
                                                        .arg(timer.elapsed())));
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(closed(lifetime), 10000);
        QCoreApplication::processEvents();
    }

    // A tab whose job holds its document cancels its viewer's and thumbnails'
    // requests. The workers that already wait for that document then let go of
    // them, so another tab keeps rendering. Without the stepped wait every worker
    // waits out the stall, about 3 s.
    void otherDocumentsRenderWhileOneIsHeld()
    {
        RenderEngine engine;
        auto held = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        auto other = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(held && other);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        Stall stall(held->lifetime()->mutex);
        submitAllPages(engine, *held, 1);
        submitAllPages(engine, *held, 2);
        QTest::qWait(200); // every worker takes a request for `held` and waits for it

        engine.cancelRequests(1);
        engine.cancelRequests(2);
        RenderRequest request;
        request.document = other.get();
        request.requester = 3;
        engine.submit(request);
        QTRY_VERIFY_WITH_TIMEOUT(renderedPages(results, 3).contains(0), 1000);
    }

    // The opposite case: requests that were not canceled wait for the document and
    // render once the job lets go of it.
    void uncanceledJobsRenderAfterTheHold()
    {
        RenderEngine engine;
        auto held = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(held);
        QSignalSpy results(&engine, &RenderEngine::resultReady);
        {
            Stall stall(held->lifetime()->mutex);
            submitAllPages(engine, *held, 1);
            QTest::qWait(200); // the workers wait for the document in steps
            QCOMPARE(results.size(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(renderedPages(results, 1).size(), held->pageCount(), 10000);
    }

    // A query that slips through on the screen thread while a job holds the
    // document returns its empty value at once instead of waiting for the job.
    // Without the check each call waits for the job's access lock, about 3 s.
    void screenQueriesRefuseWhileAJobHoldsTheDocument()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered); // only the screen thread is refused

            QElapsedTimer timer;
            expectRefusal("Document::outline");
            timer.start();
            QVERIFY(doc->outline().empty());
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("outline", timer)));

            expectRefusal("Document::pageMeasurement");
            timer.restart();
            QVERIFY(!doc->pageMeasurement(0).hasEmbedded);
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("pageMeasurement", timer)));

            expectRefusal("Document::linkTargetAt");
            timer.restart();
            QVERIFY(!doc->linkTargetAt(0, QPointF(40, 40)));
            QVERIFY2(timer.elapsed() < 50, qPrintable(took("linkTargetAt", timer)));
            QCOMPARE(Document::testing::refusals() - before, 3);
        }
        // Once the job lets go, the same thread is answered again.
        QVERIFY(doc->isPdf());
        QCOMPARE(Document::testing::refusals() - before, 3);
    }

    // A refused page is not cached as empty: the text index reads it once the job
    // lets go of the document.
    void textIndexReadsAPageRefusedDuringAHold()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        TextIndex index(engine.baseContext(), doc.get());
        const int before = Document::testing::refusals();
        {
            SavingJob job{doc->lifetime()};
            Stall stall(job);
            QVERIFY(job.answered);
            expectRefusal("TextIndex::ensure");
            QVERIFY(index.pageText(0).isEmpty());
            QCOMPARE(Document::testing::refusals() - before, 1);
        }
        QVERIFY2(index.pageText(0).contains(QStringLiteral("STANDARD TEST")),
                 qPrintable(index.pageText(0)));
    }

    // A job that is stopped while another thread holds the gate stops waiting for
    // it within a step, and the screen thread is answered again at once.
    void holdGivesUpWhenStopped()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        const int before = Document::testing::refusals();
        {
            Stall stall(lifetime->mutex);
            std::atomic<bool> stop{false};
            std::atomic<int> asked{0};
            std::atomic<bool> got{true};
            std::thread job([&] {
                const Document::Hold hold(lifetime, [&] {
                    ++asked;
                    return stop.load();
                });
                got = hold.document() != nullptr;
            });
            const auto joinJob = qScopeGuard([&] {
                stop = true;
                if (job.joinable())
                    job.join();
            });
            QTRY_VERIFY_WITH_TIMEOUT(asked.load() >= 2, 10000); // it waits in steps
            QElapsedTimer timer;
            timer.start();
            stop = true;
            job.join();
            QVERIFY2(timer.elapsed() < 100, qPrintable(QStringLiteral("giving up took %1 ms")
                                                           .arg(timer.elapsed())));
            QVERIFY(!got.load());
            QVERIFY(doc->isPdf()); // not refused, though the gate is still taken
            QCOMPARE(Document::testing::refusals(), before);
        }

        // A document that has closed is not held either.
        doc.reset();
        bool gotClosed = true;
        std::thread([&] {
            const Document::Hold hold(lifetime);
            gotClosed = hold.document() != nullptr;
        }).join();
        QVERIFY(!gotClosed);
        QCOMPARE(lifetime->holds.load(), 0);
    }

    // The screen thread checks for a hold before it locks the document. A hold that
    // starts between the check and the lock waits for the query to end, so the
    // query cannot end up waiting for the job behind it.
    void holdWaitsForAScreenQueryInFlight()
    {
        RenderEngine engine;
        auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(doc);
        const auto lifetime = doc->lifetime();
        std::atomic<int> asked{0};
        std::atomic<bool> got{false};
        std::thread job;
        const auto joinJob = qScopeGuard([&] {
            if (job.joinable())
                job.join();
        });
        {
            const Document::ScreenQuery query(*lifetime, "test");
            QVERIFY(!query.refused());
            job = std::thread([&] {
                const Document::Hold hold(lifetime, [&] {
                    ++asked;
                    return false;
                });
                got = hold.document() != nullptr;
            });
            QTRY_VERIFY_WITH_TIMEOUT(asked.load() >= 3 || got.load(), 10000);
            QVERIFY(!got.load());
        }
        job.join();
        QVERIFY(got.load());
    }
};

QTEST_GUILESS_MAIN(TstRenderLifetime)
#include "tst_render_lifetime.moc"
