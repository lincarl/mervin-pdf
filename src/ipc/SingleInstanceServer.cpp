#include "ipc/SingleInstanceServer.h"

#include "ipc/PipeName.h"

#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMutexLocker>
#include <QThread>
#include <QtGlobal>

#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#  include <sys/socket.h>
#  include <sys/stat.h>

#  include <cerrno>
#endif

namespace mervin::ipc {

namespace {

// The locks of primaries that quit with writes in flight (see keepLockUntilExit). Never
// destroyed, so even static destruction does not release them. The process's end does.
std::vector<std::unique_ptr<QLockFile>> &locksKeptUntilExit()
{
    static auto *locks = new std::vector<std::unique_ptr<QLockFile>>;
    return *locks;
}

// Whether the lock file `path`, which tryLock() found taken, has no live holder after all.
// QLockFile cannot tell that of a file without content, as a process leaves one that ended
// between making and writing it, and with a stale lock time of 0 it never counts it as
// stale. On Unix any lock without a live holder counts (see lockHeldByLiveProcess). The
// caller removes such a file once, with removeStaleLockFile(), which leaves a file that a
// live QLockFile holds, through its native lock on Unix and its open handle on Windows.
bool leftBehind(const QString &path)
{
#ifdef Q_OS_UNIX
    return !lockHeldByLiveProcess(path);
#else
    return lockLeftUnwritten(path);
#endif
}

#ifdef Q_OS_UNIX
// The XDG base directory spec lets the system remove files from $XDG_RUNTIME_DIR
// that nothing has opened for 6 hours, unless their sticky bit is set.
void keepFromCleanup(const QString &path)
{
    const QByteArray native = QFile::encodeName(path);
    struct stat st{};
    if (::stat(native.constData(), &st) == 0)
        ::chmod(native.constData(), (st.st_mode & 07777) | S_ISVTX);
}

// Whether a server accepts connections at `server`. The kernel completes a Unix
// connect to a listening socket, so a primary whose threads are all busy answers too,
// and a full backlog leaves the attempt pending.
bool answers(const QString &server)
{
    QLocalSocket probe;
    probe.connectToServer(server);
    const bool live =
        probe.waitForConnected(200) || probe.state() == QLocalSocket::ConnectingState;
    probe.abort();
    return live;
}
#endif

// Whether the launch at the other end has closed its connection, as one does that
// stopped waiting for its acknowledgement and opened its files in a copy of its own.
// On Unix the kernel keeps the frame of a launch that is gone, so this looks for the
// end of the stream behind it. Qt's Windows pipes have no buffer, so a launch's write
// stays pending until the server reads it, and closing the launch cancels it.
bool senderLeft(const QLocalSocket &socket)
{
    if (socket.state() != QLocalSocket::ConnectedState)
        return true;
#ifdef Q_OS_UNIX
    const int fd = int(socket.socketDescriptor());
    if (fd < 0)
        return true;
    char next = 0;
    ssize_t peeked = 0;
    do
        peeked = ::recv(fd, &next, 1, MSG_PEEK | MSG_DONTWAIT);
    while (peeked < 0 && errno == EINTR);
    return peeked == 0; // a launch still waiting gives EAGAIN
#else
    return false;
#endif
}

} // namespace

// Owns the QLocalServer and its connections on the server thread.
class SingleInstanceServer::Listener : public QObject
{
public:
    explicit Listener(SingleInstanceServer *owner)
        : owner_(owner)
        , server_(new QLocalServer(this))
    {
        // Per-user pipe DACL only - never WorldAccessOption, which would expose the
        // pipe to other logged-in users.
        server_->setSocketOptions(QLocalServer::UserAccessOption);
        connect(server_, &QLocalServer::newConnection, this, &Listener::onNewConnection);
    }

    // Runs on the server thread. False with `error` set when listening failed.
    bool listen(const QString &name, QString *error)
    {
        if (server_->listen(name))
            return true;
        *error = server_->errorString();
        return false;
    }

private:
    void onNewConnection()
    {
        // Each connection keeps its own decoder; the server owns the socket.
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this,
                    [this, socket, decoder = MessageDecoder()]() mutable {
                        onReadyRead(socket, decoder);
                    });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void onReadyRead(QLocalSocket *socket, MessageDecoder &decoder)
    {
        bool overflow = false;
        const QList<Message> msgs = decoder.feed(socket->readAll(), &overflow);
        if (overflow) {
            qWarning("MervinPDF: dropping client with oversized unframed buffer.");
            socket->abort();
            return;
        }
        QList<Message> opens;
        for (const Message &m : msgs)
            if (m.cmd == Message::Cmd::Open) // tolerate other commands from other versions
                opens.append(m);
        if (opens.isEmpty() || senderLeft(*socket))
            return;
        for (const Message &open : std::as_const(opens)) {
            // Queued first, so an acknowledged open is always there for the GUI thread.
            // The acknowledgement goes out before anything opens, because nested password
            // or error dialogs can block the GUI thread indefinitely.
            owner_->queueOpen(open);
            socket->write(Message::ack(QStringLiteral("open")).encode());
        }
        socket->flush();
        QMetaObject::invokeMethod(owner_, &SingleInstanceServer::opensReceived,
                                  Qt::QueuedConnection);
    }

    SingleInstanceServer *owner_; // outlives this; its destructor stops the thread
    QLocalServer *server_;
};

SingleInstanceServer::SingleInstanceServer(QObject *parent)
    : QObject(parent)
{
}

SingleInstanceServer::~SingleInstanceServer()
{
    // Closing the server removes whatever file is at the socket path on Unix. With
    // the lock still held, no other process can have put its own socket there.
    stopListening();
    lock_.reset();
}

SingleInstanceServer::Start SingleInstanceServer::start(const QString &serverName)
{
    if (thread_ || lock_)
        return Start::Started;
    const InstancePaths paths = instancePaths(serverName);
    if (paths.lockFile.isEmpty()) {
        qWarning("MervinPDF: no private folder for the single-instance lock.");
        return Start::Failed;
    }

    // Single-instance guard: a per-user lock file (NOT listen() - see header).
    // A crashed holder's lock is reclaimed via stale detection.
    lock_ = std::make_unique<QLockFile>(paths.lockFile);
    lock_->setStaleLockTime(0);
    bool locked = lock_->tryLock(0);
    if (!locked && lock_->error() == QLockFile::LockFailedError && leftBehind(paths.lockFile)
        && lock_->removeStaleLockFile())
        locked = lock_->tryLock(0);
    if (!locked) {
        const bool held = lock_->error() == QLockFile::LockFailedError;
        lock_.reset();
        if (held)
            return Start::Running;
        qWarning("MervinPDF: cannot create the lock file '%s'.", qUtf8Printable(paths.lockFile));
        return Start::Failed;
    }

#ifdef Q_OS_UNIX
    keepFromCleanup(paths.lockFile);
    // A live primary keeps its lock, so tryLock succeeds beside one only if something
    // deleted that primary's lock file. Its socket must stay, and this launch hands off.
    if (const QString live = endpointOf(paths); !live.isEmpty() && answers(live)) {
        lock_.reset();
        return Start::Running;
    }
    // A crashed primary can leave a socket file that nothing listens on. On Windows the
    // pipe vanishes with the owning process.
    if (!paths.server.isEmpty())
        QLocalServer::removeServer(paths.server);
#endif
    name_ = serverName;
    carriedFile_ = paths.carriedFile;
    // Without an endpoint the lock still keeps the profile's files to this copy. Later
    // launches wait for it as for a primary that does not answer, and then open their
    // files on their own.
    InstancePaths unreachable = paths;
    unreachable.server.clear();
    if (paths.server.isEmpty()) {
        qWarning("MervinPDF: no folder takes the single-instance socket; later launches "
                 "open their files on their own.");
        recordEndpoint(unreachable);
        return Start::Started;
    }

    auto thread = std::make_unique<QThread>();
    thread->setObjectName(QStringLiteral("SingleInstanceServer"));
    auto *listener = new Listener(this);
    listener->moveToThread(thread.get());
    // The listener, its server and its sockets are deleted on their own thread, once
    // its event loop has ended and before wait() returns.
    connect(thread.get(), &QThread::finished, listener, &QObject::deleteLater);
    thread->start();
    thread_ = std::move(thread);
    listener_ = listener;

    bool listening = false;
    QString error;
    QMetaObject::invokeMethod(
        listener, [&] { listening = listener->listen(paths.server, &error); },
        Qt::BlockingQueuedConnection);
    if (listening) {
#ifdef Q_OS_UNIX
        keepFromCleanup(paths.server);
#endif
        recordEndpoint(paths);
        return Start::Started;
    }

    qWarning("MervinPDF: failed to listen on '%s': %s; later launches open their files on "
             "their own.",
             qUtf8Printable(paths.server), qUtf8Printable(error));
    stopListening();
    recordEndpoint(unreachable);
    return Start::Started;
}

QStringList SingleInstanceServer::carriedFiles() const
{
    if (carriedFile_.isEmpty())
        return {};
    QStringList files{carriedFile_};
#ifdef Q_OS_UNIX
    for (const InstancePaths &other : otherInstancePaths(name_))
        if (other.carriedFile != carriedFile_ && !lockHeldByLiveProcess(other.lockFile))
            files.append(other.carriedFile);
#endif
    return files;
}

void SingleInstanceServer::keepLockUntilExit()
{
    stopListening();
    if (lock_)
        locksKeptUntilExit().push_back(std::move(lock_));
}

void SingleInstanceServer::releaseKeptLocksForTests()
{
    locksKeptUntilExit().clear();
}

bool SingleInstanceServer::isListening() const
{
    return thread_ != nullptr;
}

void SingleInstanceServer::stopListening()
{
    if (!thread_)
        return;
    // Ends the server thread after the event it is handling. A frame read by then has
    // been queued and acknowledged; a connection still open closes without an
    // acknowledgement, and its launch tries again.
    listener_ = nullptr;
    thread_->quit();
    thread_->wait();
    thread_.reset();
}

void SingleInstanceServer::runOnServerThreadForTests(std::function<void()> work)
{
    if (listener_)
        QMetaObject::invokeMethod(listener_, std::move(work), Qt::QueuedConnection);
}

QList<Message> SingleInstanceServer::takeOpens()
{
    const QMutexLocker locker(&opensMutex_);
    return std::exchange(opens_, {});
}

void SingleInstanceServer::queueOpen(const Message &open)
{
    const QMutexLocker locker(&opensMutex_);
    opens_.append(open);
}

} // namespace mervin::ipc
