#pragma once

#include <QList>
#include <QString>

namespace mervin::ipc {

// Stable per-user pipe name hashed to avoid Windows charset/length issues.
// Windows package identity and --profile isolate their own instance groups.
QString hostPipeName();

// Where a single-instance group keeps its lock file and listens.
struct InstancePaths
{
    QString lockFile;    // for QLockFile
    QString server;      // for QLocalServer::listen and QLocalSocket::connectToServer
    QString carriedFile; // launches a quitting primary hands to the next one
};

// The lock file and endpoint of the group `name`, hostPipeName() when empty. On Unix
// they are in the first of these folders that only this user can open and whose socket
// path Qt takes, and `server` is an absolute socket path:
// - /run/user/<uid>, as systemd-logind makes it, if it is this user's 0700 folder and
//   not a link. It does not depend on $XDG_RUNTIME_DIR, which a launch from ssh, cron
//   or `sudo -u` may not have.
// - mervin-pdf/run-<host> in the cache folder, made private, one per machine so that a
//   home folder on a network share does not mix machines
// - mervin-pdf-<uid> in the temp folder, which must already be this user's 0700 folder
//   or be made one
// All are empty when none of them qualifies. On Windows the lock is in %TEMP% and
// `server` is the pipe name.
InstancePaths instancePaths(const QString &name = QString());

#ifdef Q_OS_LINUX
// Upgrade support for copies built before the lock and socket left the temp folder, which
// a launch offers its files to while one runs (see claimOrHandOff). Remove it once such
// copies are no longer expected to run. Returns the socket that such a copy of the group
// `name`, or of its own group when `name` is empty, listens on. That is the group name in
// the temp folder, with the user token hashed from $USERNAME or $USER instead of the uid.
QString legacyEndpoint(const QString &name = QString());
#endif

// Whether `lockFile` holds no lock information and is older than QLockFile needs to write
// it, as a lock file that a process left when it ended between making and writing it.
// QLockFile with a stale lock time of 0 never counts such a file as stale. Only reads it.
bool lockLeftUnwritten(const QString &lockFile);

#ifdef Q_OS_UNIX
// The lock files, endpoints and carry files of the group `name` in the other folders that
// instancePaths() can choose, for when the folders available changed while a primary ran.
// Only folders that already are this user's private folders count, and nothing is created
// in them. The launch's own folder, which instancePaths() makes, is left out.
QList<InstancePaths> otherInstancePaths(const QString &name = QString());

// Whether a live process holds the lock file `lockFile`, which lies in one of this user's
// private folders. Only reads the file, so no other launch can see a lock this check made.
// A file without content counts as held for a moment, while its maker writes it (see
// lockLeftUnwritten). A lock from another machine ID or an earlier boot is not held, and
// neither is one whose process ID now belongs to another user, root or the kernel, as only
// this user's processes write there. On Linux, a process of this user that now runs another
// program under that ID does not count either. One whose program cannot be read, as a
// process whose last thread still waits in a file system call, does.
bool lockHeldByLiveProcess(const QString &lockFile);

// For tests only. Replaces /run/user as the folder that holds the users' runtime folders,
// or restores it when `root` is empty.
void setUserRuntimeRootForTests(const QString &root);

// Whether `dir` is a folder, not a link, that `uid` owns and that only its owner can
// open (mode 0700). Public for tests.
bool isPrivateDir(const QString &dir, uint uid);
#endif

} // namespace mervin::ipc
