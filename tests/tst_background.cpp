#include "io/Background.h"

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
};

QTEST_GUILESS_MAIN(TstBackground)
#include "tst_background.moc"
