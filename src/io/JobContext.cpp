#include "io/JobContext.h"

#include "recent/PathKey.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <utility>

namespace mervin::job {

namespace {

thread_local State *current = nullptr;
// The last step reported on this thread in the current scope, for switchTo().
thread_local Step lastStep = Step::None;
thread_local QString lastPath;

// How long a writer waits for a destination before it looks again whether its job
// stopped.
constexpr std::chrono::milliseconds kLockStep{20};

std::mutex locksMutex;
std::condition_variable locksFreed; // notified when a destination lock is released
QSet<QString> taken;                // guarded by locksMutex: keys of the held locks

// These locks cover memory only. A tab never waits for an output writer or performs
// filesystem work while holding them. State always takes its own mutex first.
std::mutex pathsMutex;
QHash<QString, int> openPaths;
QHash<QString, int> reservedPaths;

void releaseDestinations(QSet<QString> &keys)
{
    std::lock_guard<std::mutex> lock(pathsMutex);
    for (const QString &key : keys)
        if (--reservedPaths[key] == 0)
            reservedPaths.remove(key);
    keys.clear();
}

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
    const quint64 seen = releases;
    ++heldNow;
    hooksReleased.wait(lock, [seen] { return releases != seen; });
    --heldNow;
}

bool failing(Step kind, const QString &path)
{
    std::lock_guard<std::mutex> lock(hooksMutex);
    return failMatch && failMatch(kind, path);
}

// Makes a step the last one on this thread and reports it to the current job.
void record(Step kind, const QString &path)
{
    lastStep = kind;
    lastPath = path;
    if (current)
        current->report(kind, path);
}

// Records a step and applies the test hooks. Whether to go on.
bool reportStep(Step kind, const QString &path)
{
    if (QThread::isMainThread())
        ++mainThreadSteps;
    record(kind, path);
    holdIf(holdMatch, kind, path);
    return !failing(kind, path) && !stopped();
}

// A new scope starts with no step, so its first report always reaches its state.
void forgetLastStep()
{
    lastStep = Step::None;
    lastPath.clear();
}

} // namespace

State::~State()
{
    releaseDestinations(destinations_);
}

State::Stop State::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    if (ended_)
        return Stop::Done;
    if (committing_)
        return Stop::Committing;
    releaseDestinations(destinations_);
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
    if (stopped_)
        releaseDestinations(destinations_);
}

bool State::reserveDestinations(const QSet<QString> &keys)
{
    std::lock_guard<std::mutex> stateLock(mutex_);
    if (stopped_)
        return false;
    std::lock_guard<std::mutex> pathLock(pathsMutex);
    for (const QString &key : keys)
        if (openPaths.contains(key))
            return false;
    for (const QString &key : keys) {
        if (!destinations_.contains(key)) {
            destinations_.insert(key);
            ++reservedPaths[key];
        }
    }
    return true;
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
    forgetLastStep();
    std::lock_guard<std::mutex> lock(state_.mutex_);
    ++state_.scopes_;
    state_.ended_ = false;
}

Scope::~Scope()
{
    current = previous_;
    forgetLastStep();
    std::lock_guard<std::mutex> lock(state_.mutex_);
    if (--state_.scopes_ == 0) {
        state_.ended_ = true;
        releaseDestinations(state_.destinations_);
    }
}

bool stopped()
{
    return current && current->stopped();
}

OpenPath::OpenPath(const QString &key) : key_(key)
{
    std::lock_guard<std::mutex> lock(pathsMutex);
    if (!reservedPaths.contains(key_)) {
        ++openPaths[key_];
        admitted_ = true;
    }
}

OpenPath::~OpenPath()
{
    if (!admitted_)
        return;
    std::lock_guard<std::mutex> lock(pathsMutex);
    if (--openPaths[key_] == 0)
        openPaths.remove(key_);
}

bool reserveDestinations(const QSet<QString> &keys)
{
    return current && current->reserveDestinations(keys);
}

bool reading(const QString &file)
{
    return reportStep(Step::Reading, file);
}

bool writing(const QString &folder)
{
    return reportStep(Step::Writing, folder);
}

bool printing(const QString &printer)
{
    return reportStep(Step::Printing, printer);
}

void switchTo(Step step, const QString &path)
{
    if (step != lastStep || path != lastPath)
        reportStep(step, path);
}

bool beginCommit(const QString &destination)
{
    if (!reportStep(Step::Commit, destination))
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
    // Without the test hooks, so a test that holds the writer's own Writing report
    // holds it once the writer owns the destination.
    record(Step::Writing, QFileInfo(path).absolutePath());
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
