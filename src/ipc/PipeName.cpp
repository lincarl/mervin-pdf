#include "ipc/PipeName.h"

#include "config/ConfigPaths.h"
#include "platform/PlatformIntegration.h"

#include <QCryptographicHash>
#include <QDir>

#ifdef Q_OS_UNIX
#  include <QFile>
#  include <QStandardPaths>
#  include <QSysInfo>

#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace mervin::ipc {

namespace {

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

// $XDG_RUNTIME_DIR when Qt accepts it, which it does only if it belongs to this user
// with mode 0700, else empty. For anything else Qt answers with a folder in /tmp,
// which is not used.
QString runtimeDir()
{
    const QString xdg = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!xdg.startsWith(QLatin1Char('/')))
        return {};
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return dir == xdg ? dir : QString();
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

// This machine's folder in the cache directory. Several machines can share a home
// folder, and another machine's lock never goes stale here.
QString cacheDir()
{
    QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cache.isEmpty())
        cache = QDir::homePath() + QStringLiteral("/.cache");
    return QDir(cache).filePath(QStringLiteral("mervin-pdf/run-") + hostToken());
}

// Makes `dir`, which lies in this user's own home, a folder that only this user can
// open. False when it is something else, such as a link or another user's folder.
bool makePrivateDir(const QString &dir)
{
    if (!QDir().mkpath(dir))
        return false;
    const QByteArray native = QFile::encodeName(dir);
    struct stat st{};
    if (::lstat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())
        return false;
    if ((st.st_mode & 07777) != S_IRWXU && ::chmod(native.constData(), S_IRWXU) != 0)
        return false;
    return isPrivateDir(dir, uint(::geteuid()));
}

// This user's folder in the temp folder, for when the cache folder cannot hold the
// socket, as when a deep home folder makes its path too long. Anyone can create names
// in the temp folder, so a folder that is not already private to this user is refused,
// not repaired.
QString tempDir()
{
    const QString dir =
        QDir(QDir::tempPath()).filePath(QStringLiteral("mervin-pdf-%1").arg(uint(::geteuid())));
    const QByteArray native = QFile::encodeName(dir);
    // A folder this call created is this user's, whatever the umask made of its mode.
    if (::mkdir(native.constData(), S_IRWXU) == 0)
        ::chmod(native.constData(), S_IRWXU);
    return isPrivateDir(dir, uint(::geteuid())) ? dir : QString();
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
    QString dir = runtimeDir();
    if (dir.isEmpty() || !socketFits(dir, group)) {
        const QString cache = cacheDir();
        dir = socketFits(cache, group) && makePrivateDir(cache) ? cache : tempDir();
    }
    if (dir.isEmpty() || !socketFits(dir, group))
        return {};
    return {QDir(dir).filePath(group + QStringLiteral(".lock")), QDir(dir).filePath(group)};
#else
    // The lock is in the per-user %TEMP%. The pipe namespace is shared by the machine,
    // so a launch checks whose process answers (see peerIsCurrentUser).
    return {QDir(QDir::tempPath()).filePath(group + QStringLiteral(".lock")), group};
#endif
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
