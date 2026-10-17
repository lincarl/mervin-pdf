#pragma once

#include "ipc/Message.h"

#include <QList>
#include <QMutex>
#include <QObject>

#include <functional>
#include <memory>

class QLockFile;
class QThread;

namespace mervin::ipc {

// The primary's endpoint for later launches, guarded by a per-user lock file. Windows
// permits multiple named-pipe servers, so listen() alone cannot enforce one primary.
// A thread of its own accepts the connections, decodes their frames and acknowledges
// each Open as it arrives, so a primary whose GUI thread is busy still answers within
// milliseconds. The opens wait here until the GUI thread takes them. Process exit
// releases the lock and endpoint.
class SingleInstanceServer : public QObject
{
    Q_OBJECT

public:
    // How start() ended.
    enum class Start {
        Started, // listening; this process is the primary
        Running, // a live process holds the lock, or a server answers at the endpoint
        Failed,  // the lock file or the endpoint could not be made; waiting will not help
    };

    explicit SingleInstanceServer(QObject *parent = nullptr);
    // Closes the endpoint before it releases the lock, so a launch that takes the
    // lock never loses its new socket to this one's cleanup.
    ~SingleInstanceServer() override;

    // Become the primary of the group `name`. An empty name uses the per-user host
    // pipe; a custom name is used by tests. Anything but Started leaves this server
    // without the lock, and the caller hands its files to the primary.
    Start start(const QString &name = QString());

    bool isListening() const;

    // Closes the endpoint and keeps the lock, for a primary that is quitting. Later
    // launches get no acknowledgement and wait until the lock is free. Opens
    // acknowledged before remain for takeOpens().
    void stopListening();

    // The opens acknowledged so far and not taken yet, oldest first. An Open from a
    // launch that closed its connection before the server read it is dropped instead,
    // as that launch opened its files in a copy of its own.
    QList<Message> takeOpens();

    // For tests only: runs `work` on the server thread, which handles no connection
    // until it returns. Does nothing unless the server is listening.
    void runOnServerThreadForTests(std::function<void()> work);

signals:
    // Opens are waiting for takeOpens(). Emitted on this object's thread. A receiver
    // that connects later calls takeOpens() once itself.
    void opensReceived();

private:
    class Listener;

    // Called by the Listener on the server thread, before it acknowledges `open`.
    void queueOpen(const Message &open);

    std::unique_ptr<QLockFile> lock_; // single-instance guard
    std::unique_ptr<QThread> thread_; // runs the Listener while listening
    Listener *listener_ = nullptr;    // lives on thread_, which deletes it
    QMutex opensMutex_;
    QList<Message> opens_; // guarded by opensMutex_
};

} // namespace mervin::ipc
