#include "ipc/SingleInstanceServer.h"

#include "ipc/PipeName.h"

#include <QAbstractSocket>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QtGlobal>

namespace mervin::ipc {

SingleInstanceServer::SingleInstanceServer(QObject *parent)
    : QObject(parent)
{
    server_ = new QLocalServer(this);
    // Per-user pipe DACL only - never WorldAccessOption, which would expose the
    // pipe to other logged-in users.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &SingleInstanceServer::onNewConnection);
}

SingleInstanceServer::~SingleInstanceServer() = default;

bool SingleInstanceServer::start(const QString &serverName)
{
    const QString name = serverName.isEmpty() ? hostPipeName() : serverName;

    // Single-instance guard: a per-user lock file (NOT listen() - see header).
    // A crashed holder's lock is reclaimed via stale detection.
    lock_ = std::make_unique<QLockFile>(
        QDir(QDir::tempPath()).filePath(name + QStringLiteral(".lock")));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        lock_.reset();
        qInfo("MervinPDF: another host already running; exiting.");
        return false;
    }

    // Cross-platform hygiene: on Unix a crashed server can leave a stale socket
    // file that blocks listen(); removeServer clears it. On Windows the pipe
    // auto-vanishes with the owning process, so this is a harmless no-op.
    QLocalServer::removeServer(name);

    if (server_->listen(name))
        return true;

    qWarning("MervinPDF: failed to listen on pipe '%s': %s",
             qUtf8Printable(name), qUtf8Printable(server_->errorString()));
    lock_->unlock();
    lock_.reset();
    return false;
}

bool SingleInstanceServer::isListening() const
{
    return server_ && server_->isListening();
}

void SingleInstanceServer::send(QLocalSocket *socket, const Message &msg)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    socket->write(msg.encode());
    socket->flush();
}

void SingleInstanceServer::onNewConnection()
{
    while (QLocalSocket *socket = server_->nextPendingConnection()) {
        socket->setParent(this);
        decoders_.insert(socket, MessageDecoder{});

        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            emit clientDisconnected(socket);
            decoders_.remove(socket);
            socket->deleteLater();
        });
    }
}

void SingleInstanceServer::onReadyRead(QLocalSocket *socket)
{
    auto it = decoders_.find(socket);
    if (it == decoders_.end())
        return;

    bool overflow = false;
    const QList<Message> msgs = it->feed(socket->readAll(), &overflow);
    if (overflow) {
        qWarning("MervinPDF: dropping client with oversized unframed buffer.");
        socket->abort();
        return;
    }
    for (const Message &m : msgs)
        emit messageReceived(socket, m);
}

} // namespace mervin::ipc
