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

#ifdef Q_OS_UNIX
#  include <sys/socket.h>
#  include <sys/stat.h>

#  include <cerrno>
#endif

namespace mervin::ipc {

namespace {

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
    if (thread_)
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
    if (!lock_->tryLock(0)) {
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
    if (answers(paths.server)) {
        lock_.reset();
        return Start::Running;
    }
    // A crashed primary can leave a socket file that nothing listens on. On Windows the
    // pipe vanishes with the owning process.
    QLocalServer::removeServer(paths.server);
#endif

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
        return Start::Started;
    }

    qWarning("MervinPDF: failed to listen on '%s': %s", qUtf8Printable(paths.server),
             qUtf8Printable(error));
    stopListening();
    lock_.reset();
    return Start::Failed;
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
