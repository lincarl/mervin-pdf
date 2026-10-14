#pragma once

#include <memory>
#include <optional>

namespace mervin::sessionend {

// Leave time for the callback to return within the four-second logoff limit.
inline constexpr int kGraceMs = 3900;

// Tracks snapshot work independently of the tab and its current GUI job slot.
// Copies share one reservation. Completing or destroying the last copy releases it.
class RecoveryWrite
{
public:
    // Atomically admits recovery work unless a session request already closed admission.
    static std::optional<RecoveryWrite> begin();
    // Releases protection after manifest durability or terminal work completion.
    void finish() const;
    // Reports whether this reservation still adds to the recovery grace count.
    bool pending() const;

    // A worker calls this before removing its committed recovery. It rearms the
    // reservation before deletion, or retains the copy until the OS outcome.
    // False means the session ended and the discoverable copy must remain.
    bool beginDiscard() const;

private:
    struct State;
    // Retains the shared reservation acquired by begin().
    explicit RecoveryWrite(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

enum class Result { Settled, TimedOut };

// Close snapshot admission, then wait only for tracked recovery writes. This
// processes no events, performs no file work and never cancels session end.
// A repeated request returns the previous answer without starting another wait.
// Call only on the GUI thread, from the direct session callback. Concurrent
// requests are unsupported, and the callback must not process GUI events.
Result waitForRecovery(int graceMs = kGraceMs);
// Reports whether session end still gates new snapshots and recovery cleanup.
bool active();

// A canceled OS session restores admission. A successful end keeps it closed.
void finishRequest(bool ended);

// Worker-only permission for adopted recovery cleanup. An active OS request
// retains the file until its outcome. Previously admitted cleanup adds no grace.
bool beginRecoveryRemoval();

} // namespace mervin::sessionend
