#include "ipc/Handoff.h"

#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"

#include <QLocalSocket>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <vector>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace mervin::ipc {

namespace {

// How long a connect may take. A Unix connect to a listening socket completes in the
// kernel at once, and Windows waits inside connectToServer while every pipe
// instance is busy.
constexpr int kConnectMs = 200;
// Pause between attempts while the lock holder has no endpoint.
constexpr int kRetryMs = 50;

// Milliseconds left until `deadline`, for the Qt calls that take a timeout.
int msLeft(const QDeadlineTimer &deadline)
{
    return int(std::max<qint64>(0, deadline.remainingTime()));
}

#ifdef Q_OS_WIN
// The user SID in `process`'s token, or empty when it cannot be read.
QByteArray tokenUser(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    QByteArray sid;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const PSID user = reinterpret_cast<const TOKEN_USER *>(buffer.data())->User.Sid;
        if (IsValidSid(user))
            sid = QByteArray(static_cast<const char *>(user), int(GetLengthSid(user)));
    }
    CloseHandle(token);
    return sid;
}
#endif

} // namespace

#ifdef Q_OS_UNIX
std::optional<uint> peerUid(const QLocalSocket &socket)
{
    const int fd = int(socket.socketDescriptor());
    if (fd < 0)
        return std::nullopt;
#  ifdef Q_OS_LINUX
    ucred cred{};
    socklen_t size = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 || size != sizeof(cred))
        return std::nullopt;
    return uint(cred.uid);
#  else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0)
        return std::nullopt;
    return uint(uid);
#  endif
}

bool peerIsCurrentUser(const QLocalSocket &socket, const UidLookup &lookup)
{
    const std::optional<uint> uid = lookup(socket);
    return uid && *uid == uint(::getuid());
}
#else
bool peerIsCurrentUser(const QLocalSocket &socket)
{
    // The pipe namespace is shared by every session on the machine, so the name alone
    // does not tell whose process created it.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &pid))
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    const QByteArray peer = tokenUser(process);
    CloseHandle(process);
    return !peer.isEmpty() && peer == tokenUser(GetCurrentProcess());
}
#endif

Handoff handOff(const QString &server, const Message &open, QDeadlineTimer deadline,
                const PeerCheck &check)
{
    QLocalSocket sock;
    sock.connectToServer(server);
    if (!sock.waitForConnected(std::min(kConnectMs, msLeft(deadline))))
        return Handoff::NoPrimary;
    if (!(check ? check(sock) : peerIsCurrentUser(sock))) {
        qWarning("MervinPDF: the process at '%s' belongs to another user; not sending it files.",
                 qUtf8Printable(server));
        return Handoff::NoPrimary;
    }

#ifdef Q_OS_WIN
    // Pass this launch's foreground rights to the primary before delivery, so it can
    // restore and activate the target window.
    ULONG pid = 0;
    AllowSetForegroundWindow(
        GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(sock.socketDescriptor()), &pid)
            ? DWORD(pid)
            : ASFW_ANY);
#endif

    // The acknowledgement is the receipt. A bare flush only proves that this side's
    // buffer drained, and on Windows can report failure for a write that already went.
    sock.write(open.encode());
    sock.flush();

    MessageDecoder decoder;
    bool overflow = false;
    const auto acknowledged = [&] {
        for (const Message &m : decoder.feed(sock.readAll(), &overflow))
            if (m.cmd == Message::Cmd::Ack)
                return true;
        return false;
    };
    while (sock.state() == QLocalSocket::ConnectedState && !deadline.hasExpired()) {
        if (sock.waitForReadyRead(msLeft(deadline)) && acknowledged())
            return Handoff::Delivered;
        if (overflow)
            return Handoff::Busy;
    }
    // An acknowledgement that arrived with the end of the connection is still buffered.
    // Only read when there is something, as Unix closes the inner socket on that end.
    if (sock.bytesAvailable() > 0 && acknowledged())
        return Handoff::Delivered;
    // A primary that closes the connection without an acknowledgement did not take the
    // open: it stopped answering because it is quitting.
    return sock.state() == QLocalSocket::ConnectedState ? Handoff::Busy : Handoff::NoPrimary;
}

Launch claimOrHandOff(SingleInstanceServer &server, const Message &open, int waitMs,
                      const QString &name, const PeerCheck &check)
{
    const QDeadlineTimer deadline(waitMs);
    const QString endpoint = instancePaths(name).server;
    for (;;) {
        const SingleInstanceServer::Start started = server.start(name);
        if (started == SingleInstanceServer::Start::Started)
            return Launch::Primary;
        // A failed start does not get better by waiting, but a primary may still answer.
        const bool lockHeld = started == SingleInstanceServer::Start::Running;
        switch (endpoint.isEmpty() ? Handoff::NoPrimary : handOff(endpoint, open, deadline, check)) {
        case Handoff::Delivered:
            return Launch::Delivered;
        case Handoff::Busy:
            return Launch::Standalone;
        case Handoff::NoPrimary:
            break;
        }
        // The lock holder is starting, or quitting and waiting for its last writes.
        if (!lockHeld || deadline.hasExpired())
            return Launch::Standalone;
        QThread::msleep(kRetryMs);
    }
}

} // namespace mervin::ipc
