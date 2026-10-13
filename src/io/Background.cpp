#include "io/Background.h"

#include <QCoreApplication>
#include <QThreadPool>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace mervin::background {

namespace {

std::atomic<int> active{0};
std::mutex activeMutex;
std::condition_variable idle; // notified when `active` reaches zero
std::mutex postMutex;
bool closed = false; // guarded by postMutex

// Never destroyed, because its destructor would wait for every task, stuck ones too.
// Tasks are bounded by their callers, so the limit only stops a runaway.
QThreadPool *pool()
{
    static QThreadPool *threads = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(128);
        return p;
    }();
    return threads;
}

} // namespace

void start(std::function<void()> work)
{
    ++active;
    pool()->start([work = std::move(work)]() mutable {
        work();
        work = nullptr; // release what it holds before it counts as finished
        std::lock_guard<std::mutex> lock(activeMutex);
        if (--active == 0)
            idle.notify_all();
    });
}

void post(const QPointer<QObject> &context, std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(postMutex);
    QCoreApplication *app = QCoreApplication::instance();
    if (closed || !app)
        return;
    QMetaObject::invokeMethod(
        app,
        [context, fn = std::move(fn)] {
            if (context)
                fn();
        },
        Qt::QueuedConnection);
}

int running()
{
    return active.load();
}

bool settle(int ms)
{
    std::unique_lock<std::mutex> lock(activeMutex);
    return idle.wait_for(lock, std::chrono::milliseconds(ms), [] { return active.load() == 0; });
}

void shutDown()
{
    std::lock_guard<std::mutex> lock(postMutex);
    closed = true;
}

} // namespace mervin::background
