#pragma once

#include "io/AppFiles.h"
#include "ipc/Message.h"

#include <QDeadlineTimer>
#include <QString>

#include <functional>
#include <optional>

class QLocalSocket;

namespace mervin::ipc {

class SingleInstanceServer;

// How long a launch keeps trying while a live process holds the single-instance lock
// without taking its open. A quitting primary closes its endpoint and keeps the lock
// through its exit wait (appfiles::kExitWaitMs); the rest covers its last windows and
// its teardown.
constexpr int kLaunchWaitMs = appfiles::kExitWaitMs + 5000;

// Whether the process at the other end of a connected socket runs as this user.
using PeerCheck = std::function<bool(const QLocalSocket &)>;

#ifdef Q_OS_UNIX
// The uid of the process at the other end of a connected local socket, as the kernel
// recorded it when that process listened. Empty when the kernel does not say.
std::optional<uint> peerUid(const QLocalSocket &socket);

// Looks up a peer's uid. Tests replace peerUid with one that names another user.
using UidLookup = std::function<std::optional<uint>(const QLocalSocket &)>;

// True when `lookup` gives this process's uid for the peer.
bool peerIsCurrentUser(const QLocalSocket &socket, const UidLookup &lookup = peerUid);
#else
// True when the pipe server's process token has this process's user SID.
bool peerIsCurrentUser(const QLocalSocket &socket);
#endif

// How offering an open to the running primary ended.
enum class Handoff {
    Delivered, // the primary acknowledged it
    NoPrimary, // no primary of this user took it: nothing answered, the connection
               // closed without an acknowledgement, or another user's process answered
    Busy,      // a primary has the open but did not acknowledge it by the deadline
};

// Offers `open` over one connection to the endpoint `server` and waits until
// `deadline` for its acknowledgement. It writes the open at most once, and nothing
// unless `check` (peerIsCurrentUser when empty) accepts the peer. `deadline` must
// not be forever.
Handoff handOff(const QString &server, const Message &open, QDeadlineTimer deadline,
                const PeerCheck &check = {});

// What a launch does after trying to reach its single-instance group.
enum class Launch {
    Primary,    // `server` started; this process is the primary
    Delivered,  // the running primary took the open
    Standalone, // no primary could be reached in time
};

// Starts `server` for the group `name` (hostPipeName() when empty), or hands `open`
// to that group's primary. While a live process holds the lock, as a starting or
// quitting primary does, it tries both again until `waitMs` has passed. An open that
// a primary has without acknowledging it is not sent again, as that primary may still
// show it.
Launch claimOrHandOff(SingleInstanceServer &server, const Message &open, int waitMs,
                      const QString &name = QString(), const PeerCheck &check = {});

} // namespace mervin::ipc
