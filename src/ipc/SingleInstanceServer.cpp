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
#  include <sys/stat.h>
#  include <sys/un.h>
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
        bool queued = false;
        for (const Message &m : msgs) {
            if (m.cmd != Message::Cmd::Open)
                continue; // tolerate any other command a future/older client might send
            // Queued first, so an acknowledged open is always there for the GUI thread.
            // It acknowledges before opening anything: nested password or error dialogs
            // can block the GUI thread indefinitely and would make the sender wait.
            owner_->queueOpen(m);
            socket->write(Message::ack(QStringLiteral("open")).encode());
            queued = true;
        }
        if (!queued)
            return;
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
    // The path and its terminating zero must fit into sockaddr_un.
    if (QFile::encodeName(paths.server).size() >= qsizetype(sizeof(sockaddr_un::sun_path))) {
        qWarning("MervinPDF: the socket path '%s' is too long.", qUtf8Printable(paths.server));
        lock_.reset();
        return Start::Failed;
    }
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
    thread_->quit();
    thread_->wait();
    thread_.reset();
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
