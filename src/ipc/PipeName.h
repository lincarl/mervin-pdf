#pragma once

#include <QString>

namespace mervin::ipc {

// Stable per-user pipe name hashed to avoid Windows charset/length issues.
// Windows package identity and --profile isolate their own instance groups.
QString hostPipeName();

// Where a single-instance group keeps its lock file and listens.
struct InstancePaths
{
    QString lockFile; // for QLockFile
    QString server;   // for QLocalServer::listen and QLocalSocket::connectToServer
};

// The lock file and endpoint of the group `name`, hostPipeName() when empty. On Unix
// both are in $XDG_RUNTIME_DIR, which only this user can open, and `server` is an
// absolute socket path. Without a usable runtime directory they are in a private
// folder in the cache directory, never in /tmp, and both are empty when that folder
// cannot be made private. On Windows the lock is in %TEMP% and `server` is the pipe
// name.
InstancePaths instancePaths(const QString &name = QString());

} // namespace mervin::ipc
