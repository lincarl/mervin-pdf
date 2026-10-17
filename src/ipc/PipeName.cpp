#include "ipc/PipeName.h"

#include "config/ConfigPaths.h"
#include "platform/PlatformIntegration.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>

#ifdef Q_OS_UNIX
#  include <QSysInfo>

#  include <csignal>

#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace mervin::ipc {

namespace {

// How long a lock file may be without content while its maker writes it.
constexpr qint64 kUnwrittenLockMs = 2000;

// A short, stable, filesystem/pipe-safe token for `user`.
QString tokenOf(const QByteArray &user)
{
    const QByteArray digest = QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(16));
}

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
// The user's name as the environment gives it.
QByteArray userName()
{
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("USER");
    if (name.isEmpty())
        name = QStringLiteral("default");
    return name.toUtf8();
}
#endif

// The token identifying the current user.
QString userToken()
{
#ifdef Q_OS_UNIX
    // The uid, as anyone can set $USER to another user's name.
    return tokenOf(QByteArray::number(uint(::getuid())));
#else
    return tokenOf(userName());
#endif
}

// The single-instance group of the user with `token`.
QString groupName(const QString &token)
{
    QString name = QStringLiteral("MervinPDF-") + token;
    // Store and MSI copies can coexist. A Store activation must not hand its
    // document to an MSI process with a different updater and default-app identity.
    if (PlatformIntegration::hasPackageIdentity())
        name += QStringLiteral("-msix");

    // A --profile instance forms its own single-instance group: hash the
    // profile directory into the pipe name so it never hands its files to (or
    // gets blocked by) the installed app, while two launches on the SAME
    // profile still coordinate normally.
    const QString profile = ConfigPaths::overrideDir();
    if (!profile.isEmpty()) {
        QString key = QDir::cleanPath(profile);
#ifdef Q_OS_WIN
        key = key.toLower(); // Windows paths are case-insensitive
#endif
        const QByteArray digest =
            QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
        name += QLatin1Char('-') + QString::fromLatin1(digest.left(16));
    }
    return name;
}

#ifdef Q_OS_UNIX
// The bytes of sockaddr_un that QLocalServer and QLocalSocket keep free behind a socket
// path. One is for the terminating zero, and Linux, which has abstract socket names,
// needs one more. Qt refuses a longer path in listen and connect.
#  ifdef Q_OS_LINUX
constexpr qsizetype kQtSpareBytes = 2;
#  else
constexpr qsizetype kQtSpareBytes = 1;
#  endif

// Whether Qt takes the socket path of `group` in `dir`.
bool socketFits(const QString &dir, const QString &group)
{
    return QFile::encodeName(QDir(dir).filePath(group)).size() + kQtSpareBytes
        <= qsizetype(sizeof(sockaddr_un::sun_path));
}

// The folder that holds the users' runtime folders, as systemd-logind makes them.
QString g_userRuntimeRoot = QStringLiteral("/run/user");

// The folder that holds this user's socket folder when the profile folder makes the
// socket path too long. Fixed, unlike $TMPDIR, so that every launch finds the same one.
QString g_socketRoot = QStringLiteral("/tmp");

// This user's runtime folder from the file system, when it is this user's own 0700 folder
// and not a link, else empty. Unlike $XDG_RUNTIME_DIR it is the same for every launch of
// this user, from a desktop session, ssh, cron or `sudo -u`.
QString userRuntimeDir()
{
    const QString dir = QDir(g_userRuntimeRoot).filePath(QString::number(uint(::geteuid())));
    return isPrivateDir(dir, uint(::geteuid())) ? dir : QString();
}

// A short token for this machine. QLockFile names the host of a lock the same way.
QString hostToken()
{
    QByteArray id = QSysInfo::machineUniqueId();
    if (id.isEmpty())
        id = QSysInfo::machineHostName().toUtf8();
    const QByteArray digest = QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(8));
}

// This machine's folder in the profile folder, for the lock of the files there. Only
// computes the path.
QString profileInstanceDirPath()
{
    return QDir(ConfigPaths::configDirPath()).filePath(QStringLiteral("instance-") + hostToken());
}

// This user's folder in the socket root, for a socket the profile folder cannot hold.
// Only computes the path.
QString socketDirPath()
{
    return QDir(g_socketRoot).filePath(QStringLiteral("mervin-pdf-%1").arg(uint(::geteuid())));
}

// Makes `dir` if it is missing, and returns it if it is this user's own 0700 folder and
// not a link, else empty. A folder this call made is this user's, whatever the umask made
// of its mode. One that already exists is refused if it is anything else, not repaired.
// The folder that holds `dir` is left as it is.
QString ownPrivateDir(const QString &dir)
{
    const QByteArray native = QFile::encodeName(dir);
    if (::mkdir(native.constData(), S_IRWXU) == 0)
        ::chmod(native.constData(), S_IRWXU);
    return isPrivateDir(dir, uint(::geteuid())) ? dir : QString();
}

// The lock file, endpoint and carry file of `group` in `dir`.
InstancePaths pathsIn(const QString &dir, const QString &group)
{
    const QDir folder(dir);
    return {folder.filePath(group + QStringLiteral(".lock")), folder.filePath(group),
            folder.filePath(group + QStringLiteral(".carried")), QString()};
}

// The paths of `group` in the profile's instance folder `dir`. The socket goes there if Qt
// takes its path, else to this user's socket folder, which is made only if `make` is
// true. Without a usable socket folder `server` is empty. The primary records where it
// listens in the endpoint file.
InstancePaths instanceFolderPaths(const QString &dir, const QString &group, bool make)
{
    InstancePaths paths = pathsIn(dir, group);
    paths.endpointFile = QDir(dir).filePath(group + QStringLiteral(".endpoint"));
    if (!socketFits(dir, group)) {
        const QString socketDir = socketDirPath();
        const bool usable =
            make ? !ownPrivateDir(socketDir).isEmpty() : isPrivateDir(socketDir, uint(::geteuid()));
        paths.server =
            usable && socketFits(socketDir, group) ? QDir(socketDir).filePath(group) : QString();
    }
    return paths;
}
#endif

} // namespace

QString hostPipeName()
{
    return groupName(userToken());
}

InstancePaths instancePaths(const QString &name)
{
    const QString group = name.isEmpty() ? hostPipeName() : name;
#ifdef Q_OS_UNIX
    // Chosen from what is on disk, not from the environment, so that every launch of
    // this user and group on this host takes the same lock.
    const QString runtime = userRuntimeDir();
    if (!runtime.isEmpty() && socketFits(runtime, group))
        return pathsIn(runtime, group);
    // Without it, the lock goes with the files it guards. Launches that use the same
    // profile folder take the same lock, whatever their environment.
    ConfigPaths::configDir(); // makes the profile folder if it is missing
    const QString instance = ownPrivateDir(profileInstanceDirPath());
    if (instance.isEmpty())
        return {};
    return instanceFolderPaths(instance, group, true);
#else
    // The lock is in the per-user %TEMP%. The pipe namespace is shared by the machine,
    // so a launch checks whose process answers (see peerIsCurrentUser).
    const QDir folder(QDir::tempPath());
    return {folder.filePath(group + QStringLiteral(".lock")), group,
            folder.filePath(group + QStringLiteral(".carried")), QString()};
#endif
}

QString endpointOf(const InstancePaths &paths)
{
    if (paths.endpointFile.isEmpty())
        return paths.server;
    QFile file(paths.endpointFile);
    if (!file.open(QIODevice::ReadOnly))
        return paths.server;
    const QString recorded = QString::fromUtf8(file.read(4096)).trimmed();
    return QDir::isAbsolutePath(recorded) ? recorded : paths.server;
}

void recordEndpoint(const InstancePaths &paths)
{
    if (paths.endpointFile.isEmpty())
        return;
    if (paths.server.isEmpty()) {
        QFile::remove(paths.endpointFile);
        return;
    }
    QSaveFile out(paths.endpointFile);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write(paths.server.toUtf8());
    out.commit();
}

#ifdef Q_OS_UNIX
QList<InstancePaths> otherInstancePaths(const QString &name)
{
    const QString group = name.isEmpty() ? hostPipeName() : name;
    const QString own = instancePaths(name).lockFile;
    QList<InstancePaths> others;
    // Each folder is only checked with lstat. Nothing is created in any of them.
    const QString runtime = userRuntimeDir();
    if (!runtime.isEmpty() && socketFits(runtime, group)) {
        const InstancePaths paths = pathsIn(runtime, group);
        if (paths.lockFile != own)
            others.append(paths);
    }
    const QString instance = profileInstanceDirPath();
    if (isPrivateDir(instance, uint(::geteuid()))) {
        const InstancePaths paths = instanceFolderPaths(instance, group, false);
        if (paths.lockFile != own)
            others.append(paths);
    }
    return others;
}

bool lockHeldByLiveProcess(const QString &lockFile)
{
    QFile file(lockFile);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // QLockFile writes the process ID, the program's file name, the host name, the machine
    // ID and the boot ID, one per line.
    const QList<QByteArray> lines = file.read(4096).split('\n');
    file.close();
    bool numeric = false;
    const qint64 pid = lines.value(0).toLongLong(&numeric);
    if (!numeric || pid <= 0)
        return !lockLeftUnwritten(lockFile);
    // Each folder instancePaths() chooses belongs to this machine. A lock made under another
    // machine ID, or in an earlier boot of this machine, has no live holder here.
    const QByteArray machine = QSysInfo::machineUniqueId();
    if (!lines.value(3).isEmpty() && !machine.isEmpty() && lines.value(3) != machine)
        return false;
    const QByteArray boot = QSysInfo::bootUniqueId();
    if (!lines.value(4).isEmpty() && !boot.isEmpty() && lines.value(4) != boot)
        return false;
    // Only this user's processes make locks in this user's private folders, and this user can
    // signal each of them. Any failure, EPERM included, means that the ID now belongs to
    // another user, root or the kernel, and the process that made the lock has ended.
    if (::kill(pid_t(pid), 0) != 0)
        return false;
#  ifdef Q_OS_LINUX
    // A process of this user that now runs another program under that ID does not hold the
    // lock. One whose program cannot be read still does, as a process whose last thread
    // still waits in a file system call after the rest of it ended. A program file that was
    // replaced while it ran, as by an update, counts as the same.
    const QString app = QString::fromUtf8(lines.value(1));
    QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    exe.remove(QStringLiteral(" (deleted)"));
    if (!exe.isEmpty() && !app.isEmpty() && QFileInfo(exe).fileName() != app)
        return false;
#  endif
    return true;
}

void setUserRuntimeRootForTests(const QString &root)
{
    g_userRuntimeRoot = root.isEmpty() ? QStringLiteral("/run/user") : root;
}

void setSocketRootForTests(const QString &root)
{
    g_socketRoot = root.isEmpty() ? QStringLiteral("/tmp") : root;
}
#endif

bool lockLeftUnwritten(const QString &lockFile)
{
    const QFileInfo file(lockFile);
    qint64 pid = 0;
    QString host;
    QString app;
    if (!file.exists() || (QLockFile(lockFile).getLockInfo(&pid, &host, &app) && pid > 0))
        return false;
    // QLockFile writes the file right after it made it. One still without content after
    // that was left by a process that ended in between.
    const qint64 age = file.lastModified().msecsTo(QDateTime::currentDateTime());
    return age < -kUnwrittenLockMs || age >= kUnwrittenLockMs;
}

#ifdef Q_OS_LINUX
QString legacyEndpoint(const QString &name)
{
    // Older copies hashed the name from the environment, and Qt put a name that is not
    // an absolute path in the temp folder.
    const QString group = name.isEmpty() ? groupName(tokenOf(userName())) : name;
    return QDir::cleanPath(QDir::tempPath()) + QLatin1Char('/') + group;
}
#endif

#ifdef Q_OS_UNIX
bool isPrivateDir(const QString &dir, uint uid)
{
    struct stat st{};
    return ::lstat(QFile::encodeName(dir).constData(), &st) == 0 && S_ISDIR(st.st_mode)
        && st.st_uid == uid && (st.st_mode & 07777) == S_IRWXU;
}
#endif

} // namespace mervin::ipc
