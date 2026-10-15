#pragma once

#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

// Disk work handed off by the UI thread. A file call on a drive or network share
// that stopped answering blocks its thread until the system gives up, which can
// take minutes or never happen, and nothing can cancel it. So the UI thread makes
// no file calls itself and never waits for one of these threads.
namespace mervin::background {

// Runs `work` on a background thread. Nothing ever waits for these threads:
// closing a window or quitting leaves stuck work behind. A stuck call keeps its
// thread, so keep the work bounded, one task per drive or per user action, never
// one task per file.
void start(std::function<void()> work);

// Runs `fn` on the UI thread, from any thread, unless `context` has been
// destroyed by then. `context` must live on the UI thread. Dropped once
// shutDown() has run.
void post(const QPointer<QObject> &context, std::function<void()> fn);

// Runs `work` on a background thread, then `done` with its result on the UI
// thread unless `context` has been destroyed meanwhile. `work` must not use
// `context`; it gets its own copies of what it needs.
template <class Work, class Done>
void run(QObject *context, Work work, Done done)
{
    start([guard = QPointer<QObject>(context), work = std::move(work),
           done = std::move(done)]() mutable {
        if constexpr (std::is_void_v<std::invoke_result_t<Work &>>) {
            work();
            post(guard, std::move(done));
        } else {
            post(guard, [done = std::move(done), result = work()]() mutable {
                done(std::move(result));
            });
        }
    });
}

// Jobs that run one at a time, in the order added, on a background task that ends
// when none is left. Nothing waits for it: a job stuck on a drive that stopped
// answering holds up only the jobs behind it, all on one thread, and the queue can
// go away meanwhile. Safe to use from any thread.
class SerialQueue
{
public:
    SerialQueue();
    // Drops the jobs not started yet. A running job finishes on its own.
    ~SerialQueue();
    SerialQueue(const SerialQueue &) = delete;
    SerialQueue &operator=(const SerialQueue &) = delete;

    using JobId = quint64;
    /// Queues a job and returns its queue-local identity for pending cancellation.
    JobId add(std::function<void()> job);
    /// Drops this job only if it has not started. Releases captures without the queue lock.
    bool remove(JobId id);
    // Drops the jobs not started yet, as when only the newest request counts.
    void clear();

private:
    struct State; // the jobs, shared with the task that runs them
    std::shared_ptr<State> state_;
};

// Background tasks started and not yet finished.
int running();

// Waits up to `ms` milliseconds for every background task to finish, for the end of
// the process, when tasks told to stop are about to. True when none is left; a task
// still running then is stuck, and the caller leaves it behind.
bool settle(int ms);

// Called once the event loop has ended. Later posts are dropped, so no task
// reaches an object that is being torn down.
void shutDown();

} // namespace mervin::background
