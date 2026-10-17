#include "ipc/Handoff.h"

#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"

#include <QFileInfo>
#include <QLocalSocket>
#include <QLockFile>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <atomic>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <vector>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include <cerrno>
#  include <csignal>
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

#ifdef Q_OS_UNIX
// Whether a live process holds the lock file `path`. Only reads the file, so no other
// launch can see a lock this check made. A file that is still being written, or that
// names another host, counts as held.
bool heldByLiveProcess(const QString &path)
{
    if (!QFileInfo::exists(path))
        return false;
    qint64 pid = 0;
    QString host;
    QString app;
    if (!QLockFile(path).getLockInfo(&pid, &host, &app) || pid <= 0)
        return true;
    if (!host.isEmpty() && host != QSysInfo::machineHostName())
        return true;
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
}
#endif

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
std::optional<QByteArray> peerUser(const QLocalSocket &socket)
{
    const int fd = int(socket.socketDescriptor());
    if (fd < 0)
        return std::nullopt;
#  ifdef Q_OS_LINUX
    ucred cred{};
    socklen_t size = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 || size != sizeof(cred))
        return std::nullopt;
    return QByteArray::number(uint(cred.uid));
#  else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0)
        return std::nullopt;
    return QByteArray::number(uint(uid));
#  endif
}

QByteArray currentUser()
{
    return QByteArray::number(uint(::getuid()));
}
#else
std::optional<QByteArray> peerUser(const QLocalSocket &socket)
{
    // The pipe namespace is shared by every session on the machine, so the name alone
    // does not tell whose process created it.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &pid))
        return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return std::nullopt;
    const QByteArray sid = tokenUser(process);
    CloseHandle(process);
    if (sid.isEmpty())
        return std::nullopt;
    return sid;
}

QByteArray currentUser()
{
    return tokenUser(GetCurrentProcess());
}
#endif

bool peerIsCurrentUser(const QLocalSocket &socket, const UserLookup &lookup)
{
    const std::optional<QByteArray> peer = lookup(socket);
    return peer && !peer->isEmpty() && *peer == currentUser();
}

Handoff handOff(const QString &server, const Message &open, QDeadlineTimer deadline,
                const UserLookup &lookup)
{
    QLocalSocket sock;
    sock.connectToServer(server);
    if (!sock.waitForConnected(std::min(kConnectMs, msLeft(deadline))))
        return Handoff::NoPrimary;
    if (!peerIsCurrentUser(sock, lookup)) {
        // Once per process, as a launch tries again while it waits.
        static std::atomic<bool> warned = false;
        if (!warned.exchange(true))
            qWarning("MervinPDF: the process at '%s' belongs to another user; not sending it "
                     "files.",
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
    // One last look for an acknowledgement that came in as the deadline passed. Once
    // this launch closes, a primary that reads the open later drops it.
    if (sock.state() == QLocalSocket::ConnectedState)
        sock.waitForReadyRead(0);
    // Only read when there is something, as Unix closes the inner socket once the
    // connection has ended.
    if (sock.bytesAvailable() > 0 && acknowledged())
        return Handoff::Delivered;
    // A primary closes a connection without an acknowledgement only while it quits, and
    // then it did not take the open.
    return sock.state() == QLocalSocket::ConnectedState ? Handoff::Busy : Handoff::NoPrimary;
}

Launch claimOrHandOff(SingleInstanceServer &server, const Message &open, int waitMs,
                      const QString &name, const UserLookup &lookup)
{
    const QDeadlineTimer deadline(waitMs);
    const InstancePaths paths = instancePaths(name);
#ifdef Q_OS_UNIX
    // While nothing holds this group's lock, a primary may still run elsewhere. One of
    // this version can have chosen another folder, as when the folders available changed
    // since it started. One from before the lock and socket left the temp folder does not
    // see this lock at all (Linux only, upgrade support removed with legacyEndpoint()).
    // A primary there that takes the open but does not answer in time keeps it, so this
    // launch opens the files standalone. One that closes the connection, as when it is
    // quitting, is passed over.
    if (paths.lockFile.isEmpty() || !heldByLiveProcess(paths.lockFile)) {
        QStringList elsewhere = otherEndpoints(name);
#  ifdef Q_OS_LINUX
        elsewhere.append(legacyEndpoint(name));
#  endif
        for (const QString &other : std::as_const(elsewhere)) {
            switch (handOff(other, open, deadline, lookup)) {
            case Handoff::Delivered:
                return Launch::Delivered;
            case Handoff::Busy:
                return Launch::Standalone;
            case Handoff::NoPrimary:
                break;
            }
        }
    }
#endif
    const QString &endpoint = paths.server;
    for (;;) {
        const SingleInstanceServer::Start started = server.start(name);
        if (started == SingleInstanceServer::Start::Started)
            return Launch::Primary;
        // A failed start does not get better by waiting, but a primary may still answer.
        const bool lockHeld = started == SingleInstanceServer::Start::Running;
        switch (endpoint.isEmpty() ? Handoff::NoPrimary
                                   : handOff(endpoint, open, deadline, lookup)) {
        case Handoff::Delivered:
            return Launch::Delivered;
        case Handoff::Busy:
            return Launch::Standalone;
        case Handoff::NoPrimary:
            break;
        }
        // Without a lock holder nothing owns the session and the shared files.
        if (!lockHeld)
            return Launch::Uncoordinated;
        // The lock holder is starting, or quitting and waiting for its last writes.
        if (deadline.hasExpired())
            return Launch::Standalone;
        QThread::msleep(kRetryMs);
    }
}

} // namespace mervin::ipc
