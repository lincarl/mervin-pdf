#pragma once

#include <QList>
#include <QString>

#include <memory>

namespace mervin::ipc {

// Stable per-user pipe name hashed to avoid Windows charset/length issues.
// Windows package identity and --profile isolate their own instance groups.
QString hostPipeName();

// Where a single-instance group keeps its lock file and listens.
struct InstancePaths
{
    QString lockFile;     // for QLockFile
    QString server;       // for QLocalServer::listen and QLocalSocket::connectToServer
    QString carriedFile;  // launches a quitting primary hands to the next one
    QString endpointFile; // where the primary records `server`, if anywhere (see endpointOf)
};

// The lock file and endpoint of the group `name`, hostPipeName() when empty. On Unix the
// lock is in the first of these folders, and `server` is an absolute socket path:
// - /run/user/<uid>, as systemd-logind makes it, if it is this user's 0700 folder and
//   not a link, and Qt takes the socket path there. It does not depend on
//   $XDG_RUNTIME_DIR, which a launch from ssh, cron or `sudo -u` may not have.
// - instance-<host> in the profile folder (ConfigPaths::configDir()), which holds the
//   files the lock guards. It is made with mode 0700 if missing, and refused if it is
//   not this user's 0700 folder or is a link. The profile folder's own mode is left as
//   it is. The host part keeps machines that share the profile folder apart. Where Qt
//   does not take the socket path there, the socket goes to mervin-pdf-<uid> in /tmp,
//   on the same terms (see socketFolderServer), as it does when the profile folder's
//   file system holds no sockets (see SingleInstanceServer::start). With no usable
//   folder for it, `server` is empty, and the lock alone keeps the profile's files to
//   one copy.
// All are empty when no folder takes the lock. Windows keeps the lock in the
// profile's instance-<host> folder with an owner-and-SYSTEM DACL. Existing
// nonprivate folders and reparse points are refused. `server` is the SID-based
// pipe name. Profile links resolve to the same group.
InstancePaths instancePaths(const QString &name = QString());

// Where the primary of `paths` listens: the path it recorded in `paths.endpointFile`, or
// else `paths.server`. Launches that reach the same profile folder through different
// paths can compute different sockets, so they read the primary's own record.
QString endpointOf(const InstancePaths &paths);

// Records `paths.server` in `paths.endpointFile`, for the primary that listens there, or
// removes the record when it listens nowhere. Does nothing without an endpoint file.
void recordEndpoint(const InstancePaths &paths);

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
// Upgrade support for copies built before the lock and socket left the temp folder, which
// a launch offers its files to while one runs (see claimOrHandOff). Remove it once such
// copies are no longer expected to run. Returns the endpoint that such a copy of the group
// `name`, or of its own group when `name` is empty, listens on. Windows returns its
// pipe name; Linux returns the socket in the temp folder. Both use the token
// hashed from USERNAME or USER instead of the OS identity.
QString legacyEndpoint(const QString &name = QString());
#endif

#ifdef Q_OS_WIN
// The old username-based group's paths in this launch's TEMP. Creates nothing.
InstancePaths legacyInstancePaths(const QString &name = QString());

class LegacyCarryGuard;

// Locks a legacy carry file's group while its current-user-owned TEMP folder is
// pinned against renames. Refuses reparse points and ACLs granting access beyond
// this user, SYSTEM and Administrators. Inherited legacy ACLs are accepted without
// changing the stricter policy for new instance folders. Empty if unsafe or held.
// Keep the guard alive until takeCarried() has finished consuming paths.carriedFile.
std::shared_ptr<LegacyCarryGuard> guardLegacyCarry(const InstancePaths &paths);
#endif

// Whether `lockFile` holds no lock information and is older than QLockFile needs to write
// it, as a lock file that a process left when it ended between making and writing it.
// QLockFile with a stale lock time of 0 never counts such a file as stale. Only reads it.
bool lockLeftUnwritten(const QString &lockFile);

#ifdef Q_OS_UNIX
// The group's other paths, including the preceding noncanonical profile identity when
// `name` is empty, for upgrades or when /run/user/<uid> changed while a primary ran.
// Only existing private folders count. No alternate folder is created, and the launch's
// own lock path, which instancePaths() chooses, is left out.
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

// The socket of the group `name` in this user's folder in /tmp, which is made if it is
// missing, for a primary whose profile folder holds no sockets. Empty if that folder is
// refused or Qt does not take the path. See instancePaths() for the folder's terms.
QString socketFolderServer(const QString &name = QString());

// For tests only. Replaces /tmp as the folder that holds this user's socket folder, or
// restores it when `root` is empty.
void setSocketRootForTests(const QString &root);

// Whether `dir` is a folder, not a link, that `uid` owns and that only its owner can
// open (mode 0700). Public for tests.
bool isPrivateDir(const QString &dir, uint uid);
#endif

} // namespace mervin::ipc
