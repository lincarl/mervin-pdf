#pragma once

#include "ipc/Message.h"

#include <QHash>
#include <QObject>

#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

namespace mervin::ipc {

// The resident host's IPC endpoint and single-instance lock in one object.
//
// start() first acquires a per-user lock file: that, not the pipe, is the
// single-instance guard. (On Windows a named pipe permits MULTIPLE server
// instances on the same name, so QLocalServer::listen() does NOT fail for a
// duplicate - it can't enforce single-instance.) If the lock is already held,
// another host owns it and the caller should exit. Otherwise we listen on the
// pipe and serve. Each accepted connection gets its own MessageDecoder; whole
// frames are emitted via messageReceived(). The lock file is released (and on
// Windows the pipe auto-vanishes) when this process dies, so a crashed host
// leaves nothing stale behind.
class SingleInstanceServer : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstanceServer(QObject *parent = nullptr);
    ~SingleInstanceServer() override;

    // Become the single instance. Returns false if another host owns the pipe
    // (or listen otherwise fails) - caller should exit in that case. An empty
    // name uses the per-user host pipe; a custom name is used by tests.
    bool start(const QString &name = QString());

    bool isListening() const;

    // Write a message frame to a connected client. Static so callers that only
    // hold a socket (e.g. the router) can use it without the server.
    static void send(QLocalSocket *socket, const Message &msg);

signals:
    void messageReceived(QLocalSocket *socket, const mervin::ipc::Message &msg);
    void clientDisconnected(QLocalSocket *socket);

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);

    QLocalServer *server_ = nullptr;
    std::unique_ptr<QLockFile> lock_; // single-instance guard
    QHash<QLocalSocket *, MessageDecoder> decoders_;
};

} // namespace mervin::ipc
