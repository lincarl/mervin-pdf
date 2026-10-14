#include "io/JobContext.h"

#include "recent/PathKey.h"

#include <QFileInfo>
#include <QSet>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <utility>

namespace mervin::job {

namespace {

thread_local State *current = nullptr;

// How long a writer waits for a destination before it looks again whether its job
// stopped.
constexpr std::chrono::milliseconds kLockStep{20};

std::mutex locksMutex;
std::condition_variable locksFreed; // notified when a destination lock is released
QSet<QString> taken;                // guarded by locksMutex: keys of the held locks

// The test hooks (see testing).
std::mutex hooksMutex;
std::condition_variable hooksReleased; // notified by release()
std::function<bool(Step, const QString &)> holdMatch;   // guarded by hooksMutex
std::function<bool(const QString &)> commitHoldMatch;   // guarded by hooksMutex
std::function<bool(Step, const QString &)> failMatch;   // guarded by hooksMutex
quint64 releases = 0;                                   // guarded by hooksMutex
int heldNow = 0;                                        // guarded by hooksMutex
std::atomic<int> mainThreadSteps{0};

// Blocks until release() when `match` accepts, except on the GUI thread.
template <class Match, class... Args>
void holdIf(const Match &match, const Args &...args)
{
    if (QThread::isMainThread())
        return;
    std::unique_lock<std::mutex> lock(hooksMutex);
    if (!match || !match(args...))
        return;
    const quint64 release = releases;
    ++heldNow;
    hooksReleased.wait(lock, [release] { return releases != release; });
    --heldNow;
}

bool failing(Step step, const QString &path)
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    return failMatch && failMatch(step, path);
}

// Reports `step` to the current job and applies the test hooks. Whether to go on.
bool step(Step step, const QString &path)
{
    if (QThread::isMainThread())
        ++mainThreadSteps;
    if (current)
        current->report(step, path);
    holdIf(holdMatch, step, path);
    return !failing(step, path) && !stopped();
}

} // namespace

State::Stop State::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    if (ended_)
        return Stop::Done;
    if (committing_)
        return Stop::Committing;
    if (holding_)
        return Stop::Holding;
    return Stop::Stopped;
}

bool State::beginCommit()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return false;
    committing_ = true;
    return true;
}

void State::endCommit()
{
    std::lock_guard<std::mutex> lock(mutex_);
    committing_ = false;
}

void State::report(Step step, const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    step_ = step;
    path_ = path;
}

void State::setHolding(bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    holding_ = on;
}

State::Snapshot State::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {step_, path_, ticks_.load(std::memory_order_relaxed), holding_, stopped_};
}

Scope::Scope(State &state)
    : state_(state)
    , previous_(std::exchange(current, &state))
{
    std::lock_guard<std::mutex> lock(state_.mutex_);
    ++state_.scopes_;
    state_.ended_ = false;
}

Scope::~Scope()
{
    current = previous_;
    std::lock_guard<std::mutex> lock(state_.mutex_);
    if (--state_.scopes_ == 0)
        state_.ended_ = true;
}

bool stopped()
{
    return current && current->stopped();
}

bool reading(const QString &file)
{
    return step(Step::Reading, file);
}

bool writing(const QString &folder)
{
    return step(Step::Writing, folder);
}

bool printing(const QString &printer)
{
    return step(Step::Printing, printer);
}

bool beginCommit(const QString &destination)
{
    if (!step(Step::Commit, destination))
        return false;
    if (current && !current->beginCommit())
        return false;
    holdIf(commitHoldMatch, destination);
    return true;
}

void endCommit()
{
    if (current)
        current->endCommit();
}

void progressed()
{
    if (current)
        current->progressed();
}

QString notRunError()
{
    return QStringLiteral("This step was not run.");
}

DestinationLock::DestinationLock(const QString &path)
{
    if (current)
        current->report(Step::Writing, QFileInfo(path).absolutePath());
    if (stopped())
        return;
    key_ = fileKey(QFileInfo(path));
    std::unique_lock<std::mutex> lock(locksMutex);
    for (;;) {
        if (stopped())
            return;
        if (!taken.contains(key_))
            break;
        locksFreed.wait_for(lock, kLockStep);
    }
    taken.insert(key_);
    owns_ = true;
}

DestinationLock::~DestinationLock()
{
    if (!owns_)
        return;
    {
        std::lock_guard<std::mutex> lock(locksMutex);
        taken.remove(key_);
    }
    locksFreed.notify_all();
}

namespace testing {

void holdWhen(std::function<bool(Step, const QString &)> match)
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    holdMatch = std::move(match);
}

void holdCommitWhen(std::function<bool(const QString &)> match)
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    commitHoldMatch = std::move(match);
}

int held()
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    return heldNow;
}

void release()
{
    {
        std::lock_guard<std::mutex> lock(hooksMutex);
        holdMatch = nullptr;
        commitHoldMatch = nullptr;
        ++releases;
    }
    hooksReleased.notify_all();
}

void failWhen(std::function<bool(Step, const QString &)> match)
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    failMatch = std::move(match);
}

int stepsOnMainThread()
{
    return mainThreadSteps.load();
}

} // namespace testing

} // namespace mervin::job
