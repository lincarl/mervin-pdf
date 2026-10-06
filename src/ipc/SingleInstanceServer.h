#pragma once

#include "ipc/Message.h"

#include <QHash>
#include <QObject>

#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

namespace mervin::ipc {

// IPC endpoint guarded by a per-user lock file. Windows permits multiple named-pipe servers, so
// listen() alone cannot enforce one primary. Each connection owns a MessageDecoder and emits
// complete frames. Process exit releases the lock and endpoint.
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
