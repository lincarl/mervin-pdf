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
// both are in the first of these folders that only this user can open and whose socket
// path Qt takes, and `server` is an absolute socket path:
// - $XDG_RUNTIME_DIR, if Qt accepts it
// - mervin-pdf/run-<host> in the cache folder, made private, one per machine so that a
//   home folder on a network share does not mix machines
// - mervin-pdf-<uid> in the temp folder, which must already be this user's 0700 folder
//   or be made one
// Both are empty when none of them qualifies. On Windows the lock is in %TEMP% and
// `server` is the pipe name.
InstancePaths instancePaths(const QString &name = QString());

#ifdef Q_OS_UNIX
// Whether `dir` is a folder, not a link, that `uid` owns and that only its owner can
// open (mode 0700). Public for tests.
bool isPrivateDir(const QString &dir, uint uid);
#endif

} // namespace mervin::ipc
