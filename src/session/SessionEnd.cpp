#include "session/SessionEnd.h"

#include <QDeadlineTimer>
#include <QThread>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace mervin::sessionend {

namespace {
struct Registry {
    std::mutex mutex;
    std::condition_variable changed;
    int pending = 0;
    bool active = false;
    bool ended = false;
    Result result = Result::Settled;
};

Registry &registry()
{
    static Registry value;
    return value;
}
}

struct RecoveryWrite::State {
    bool pending = true; // protected by the registry mutex

    ~State() { finish(); }

    void finish()
    {
        auto &r = registry();
        std::lock_guard lock(r.mutex);
        if (std::exchange(pending, false))
            --r.pending;
    }
};

RecoveryWrite::RecoveryWrite(std::shared_ptr<State> state)
    : state_(std::move(state))
{
}

std::optional<RecoveryWrite> RecoveryWrite::begin()
{
    auto &r = registry();
    std::lock_guard lock(r.mutex);
    if (r.active)
        return std::nullopt;
    auto state = std::make_shared<State>();
    ++r.pending;
    return RecoveryWrite(std::move(state));
}

void RecoveryWrite::finish() const
{
    if (state_)
        state_->finish();
}

bool RecoveryWrite::pending() const
{
    auto &r = registry();
    std::lock_guard lock(r.mutex);
    return state_ && state_->pending;
}

bool RecoveryWrite::beginDiscard() const
{
    auto &r = registry();
    std::unique_lock lock(r.mutex);
    // Only the worker waits. A committed copy remains discoverable while the
    // direct session callback waits for other snapshots, or Windows decides.
    r.changed.wait(lock, [&] { return !r.active || r.ended; });
    if (r.ended)
        return false;
    if (!std::exchange(state_->pending, true))
        ++r.pending;
    return true;
}

Result waitForRecovery(int graceMs)
{
    auto &r = registry();
    QDeadlineTimer deadline(std::clamp(graceMs, 0, kGraceMs), Qt::PreciseTimer);
    {
        std::lock_guard lock(r.mutex);
        if (r.active)
            return r.result;
        r.active = true;
    }
    for (;;) {
        {
            std::lock_guard lock(r.mutex);
            if (r.pending == 0)
                return r.result = Result::Settled;
            if (deadline.hasExpired())
                return r.result = Result::TimedOut;
        }
        QThread::msleep(static_cast<unsigned long>(
            std::min<qint64>(10, std::max<qint64>(1, deadline.remainingTime()))));
    }
}

bool active()
{
    auto &r = registry();
    std::lock_guard lock(r.mutex);
    return r.active;
}

void finishRequest(bool ended)
{
    auto &r = registry();
    std::lock_guard lock(r.mutex);
    r.ended = ended;
    if (ended) {
        r.active = true;
    } else {
        r.active = false;
        r.result = Result::Settled;
    }
    r.changed.notify_all();
}

bool beginRecoveryRemoval()
{
    auto &r = registry();
    std::unique_lock lock(r.mutex);
    r.changed.wait(lock, [&] { return !r.active || r.ended; });
    return !r.ended;
}

} // namespace mervin::sessionend
