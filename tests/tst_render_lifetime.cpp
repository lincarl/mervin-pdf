#include "render/Document.h"
#include "render/DocumentSearch.h"
#include "render/RenderEngine.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace mervin;

namespace {

// Holds a document's lifetime lock on another thread, as a render or a search stuck
// reading the document from a share that stopped answering does, until released or
// for at most `ms` milliseconds.
class Stall
{
public:
    explicit Stall(std::mutex &held, int ms = 3000)
        : thread_([this, &held, ms] {
            std::lock_guard<std::mutex> holding(held);
            std::unique_lock<std::mutex> lock(mutex_);
            locked_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return released_; });
        })
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return locked_; });
    }
    ~Stall()
    {
        release();
        thread_.join();
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool locked_ = false;
    bool released_ = false;
    std::thread thread_; // last: it uses the members above
};

bool closed(const std::shared_ptr<DocumentLifetime> &lifetime)
{
    std::lock_guard<std::mutex> lock(lifetime->mutex);
    return !lifetime->document;
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
};

QTEST_GUILESS_MAIN(TstRenderLifetime)
#include "tst_render_lifetime.moc"
