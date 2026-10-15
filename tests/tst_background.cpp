#include "io/Background.h"

#include <QScopeGuard>
#include <QtTest>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <numeric>
#include <vector>

using namespace mervin;

namespace {

// Where a job waits, as on a drive that stopped answering, until the test lets it go.
class Gate
{
public:
    void pass()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        reached_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }
    bool reached(int ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return reached_; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool reached_ = false;
    bool open_ = false;
};

// The jobs that ran, in order, from any thread.
struct Log
{
    std::mutex mutex;
    std::vector<int> ran;
    void add(int job)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ran.push_back(job);
    }
    std::vector<int> all()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return ran;
    }
};

} // namespace

class TstBackground : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        QVERIFY(background::settle(10000));
    }

    void serialJobsRunInOrderOneAtATime()
    {
        const auto log = std::make_shared<Log>();
        const auto running = std::make_shared<std::atomic<int>>(0);
        const auto overlapped = std::make_shared<std::atomic<bool>>(false);
        background::SerialQueue queue;
        for (int job = 0; job < 50; ++job) {
            queue.add([log, running, overlapped, job] {
                if (running->fetch_add(1) != 0)
                    overlapped->store(true);
                log->add(job);
                running->fetch_sub(1);
            });
        }
        QTRY_COMPARE_WITH_TIMEOUT(log->all().size(), 50u, 10000);
        std::vector<int> expected(50);
        std::iota(expected.begin(), expected.end(), 0);
        QCOMPARE(log->all(), expected);
        QVERIFY(!overlapped->load());
    }

    void clearDropsTheJobsNotStarted()
    {
        const auto log = std::make_shared<Log>();
        const auto gate = std::make_shared<Gate>();
        background::SerialQueue queue;
        queue.add([log, gate] {
            gate->pass();
            log->add(1);
        });
        QVERIFY(gate->reached(10000));
        queue.add([log] { log->add(2); });
        queue.add([log] { log->add(3); });
        queue.clear();
        queue.add([log] { log->add(4); });
        gate->open();
        QTRY_COMPARE_WITH_TIMEOUT(log->all().size(), 2u, 10000);
        QCOMPARE(log->all(), (std::vector<int>{1, 4}));
    }

    void aQueueGoesAwayWithoutItsStuckJob()
    {
        const auto log = std::make_shared<Log>();
        const auto gate = std::make_shared<Gate>();
        auto queue = std::make_unique<background::SerialQueue>();
        queue->add([log, gate] {
            gate->pass();
            log->add(1);
        });
        queue->add([log] { log->add(2); });
        QVERIFY(gate->reached(10000));
        queue.reset(); // the stuck job keeps running; the one behind it never starts
        gate->open();
        QVERIFY(background::settle(10000));
        QCOMPARE(log->all(), std::vector<int>{1});
    }

    /// Removes one pending job, releasing reentrant captures while other work survives.
    void removeDropsOnlyThePendingJobWithoutHoldingTheLock()
    {
        const auto log = std::make_shared<Log>();
        const auto gate = std::make_shared<Gate>();
        const auto release = qScopeGuard([gate] { gate->open(); });
        const auto queue = std::make_shared<background::SerialQueue>();
        const auto running = queue->add([log, gate] {
            gate->pass();
            log->add(1);
        });
        QVERIFY(gate->reached(10000));
        auto capture = std::shared_ptr<int>(new int(0), [queue, log](int *value) {
            delete value;
            queue->add([log] { log->add(4); });
        });
        const std::weak_ptr<int> dropped = capture;
        const auto pending = queue->add([log, capture] { log->add(2); });
        queue->add([log] { log->add(3); });
        capture.reset();
        const auto removed = std::make_shared<std::atomic<bool>>(false);
        const auto answered = std::make_shared<std::atomic<bool>>(false);
        // A bad lock lifetime fails within the timeout instead of blocking the test thread.
        background::start([queue, pending, removed, answered] {
            removed->store(queue->remove(pending));
            answered->store(true);
        });
        QTRY_VERIFY_WITH_TIMEOUT(answered->load(), 5000);
        QVERIFY(removed->load());
        QVERIFY(dropped.expired());
        QVERIFY(!queue->remove(pending));
        QVERIFY(!queue->remove(running));
        QVERIFY(!queue->remove(0));
        gate->open();
        QVERIFY(background::settle(10000));
        QCOMPARE(log->all(), (std::vector<int>{1, 3, 4}));
    }

    /// Releases a queue held only by its final running job after that job returns.
    void runningJobReleasesItsQueueKeepalive()
    {
        const auto gate = std::make_shared<Gate>();
        const auto release = qScopeGuard([gate] { gate->open(); });
        auto queue = std::make_shared<background::SerialQueue>();
        const std::weak_ptr<background::SerialQueue> retained = queue;
        queue->add([queue, gate] {
            Q_UNUSED(queue);
            gate->pass();
        });
        QVERIFY(gate->reached(10000));
        queue.reset();
        QVERIFY(!retained.expired());
        gate->open();
        QVERIFY(background::settle(10000));
        QVERIFY(retained.expired());
    }
};

QTEST_GUILESS_MAIN(TstBackground)
#include "tst_background.moc"
