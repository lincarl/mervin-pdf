#pragma once

#include "io/AppFiles.h"
#include "ipc/Message.h"

#include <QByteArray>
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

// Who runs the process at the other end of a connected local socket. On Unix this is
// the uid the kernel recorded when that process listened, on Windows the user SID in
// the pipe server process's token. Empty when the system does not say.
std::optional<QByteArray> peerUser(const QLocalSocket &socket);

// The same identity for this process.
QByteArray currentUser();

// Finds who runs a socket's peer. Tests replace peerUser with one that names another
// user, so they run the same check as a real launch.
using UserLookup = std::function<std::optional<QByteArray>(const QLocalSocket &)>;

// True when `lookup` names this process's user for the peer.
bool peerIsCurrentUser(const QLocalSocket &socket, const UserLookup &lookup = peerUser);

// How offering an open to the running primary ended.
enum class Handoff {
    Delivered, // the primary acknowledged it
    NoPrimary, // no primary of this user took it: nothing answered, the connection
               // closed without an acknowledgement, or another user's process answered
    Busy,      // a primary has the open but did not acknowledge it by the deadline
};

// Offers `open` over one connection to the endpoint `server` and waits until
// `deadline` for its acknowledgement. It writes the open at most once, and nothing
// unless `lookup` names this user for the peer. `deadline` must not be forever.
Handoff handOff(const QString &server, const Message &open, QDeadlineTimer deadline,
                const UserLookup &lookup = peerUser);

// What a launch does after trying to reach its single-instance group.
enum class Launch {
    Primary,       // `server` started; this process is the primary
    Delivered,     // the running primary took the open
    Standalone,    // a live process holds the lock but did not take the open in time
    Uncoordinated, // no lock or endpoint could be made and no primary answered
};

// Starts `server` for the group `name` (hostPipeName() when empty), or hands `open`
// to that group's primary. On Unix, while nothing holds the lock, it first offers `open`
// to a primary of this user in another folder (see otherEndpoints), and on Linux to a
// copy from before the lock and socket moved (see legacyEndpoint). While a live process
// holds the lock, as a starting or quitting primary does, it tries again until `waitMs`
// has passed. An open that a primary accepted over a connection is not written again.
// If that primary reads it only after this launch gave up, it drops it, because the
// launch then opens the files itself.
Launch claimOrHandOff(SingleInstanceServer &server, const Message &open, int waitMs,
                      const QString &name = QString(), const UserLookup &lookup = peerUser);

} // namespace mervin::ipc
