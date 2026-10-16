#include "ipc/PipeName.h"

#include "config/ConfigPaths.h"
#include "platform/PlatformIntegration.h"

#include <QCryptographicHash>
#include <QDir>

#ifdef Q_OS_UNIX
#  include <QFile>
#  include <QStandardPaths>

#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace mervin::ipc {

namespace {

// A short, stable, filesystem/pipe-safe token identifying the current user.
QString userToken()
{
#ifdef Q_OS_UNIX
    // The uid, as anyone can set $USER to another user's name.
    const QByteArray user = QByteArray::number(uint(::getuid()));
#else
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("USER");
    if (name.isEmpty())
        name = QStringLiteral("default");
    const QByteArray user = name.toUtf8();
#endif

    const QByteArray digest = QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(16));
}

#ifdef Q_OS_UNIX
// Makes `dir` a folder that only this user can open, as $XDG_RUNTIME_DIR is. False
// when it is something else, such as a link or another user's folder.
bool makePrivateDir(const QString &dir)
{
    if (!QDir().mkpath(dir))
        return false;
    const QByteArray native = QFile::encodeName(dir);
    struct stat st{};
    if (::lstat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())
        return false;
    return (st.st_mode & 07777) == S_IRWXU || ::chmod(native.constData(), S_IRWXU) == 0;
}

// The per-user folder for the lock and the socket. Other users cannot open it, and
// /tmp cleaners do not reach it.
QString runtimeDir()
{
    // Qt accepts $XDG_RUNTIME_DIR only if it belongs to this user with mode 0700.
    // Otherwise Qt answers with a folder in /tmp, which is not used.
    const QString xdg = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (xdg.startsWith(QLatin1Char('/'))) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (dir == xdg)
            return dir;
    }
    QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cache.isEmpty())
        cache = QDir::homePath() + QStringLiteral("/.cache");
    const QString dir = QDir(cache).filePath(QStringLiteral("mervin-pdf/run"));
    return makePrivateDir(dir) ? dir : QString();
}
#endif

} // namespace

QString hostPipeName()
{
    QString name = QStringLiteral("MervinPDF-") + userToken();
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

InstancePaths instancePaths(const QString &name)
{
    const QString group = name.isEmpty() ? hostPipeName() : name;
#ifdef Q_OS_UNIX
    const QString dir = runtimeDir();
    if (dir.isEmpty())
        return {};
    return {QDir(dir).filePath(group + QStringLiteral(".lock")), QDir(dir).filePath(group)};
#else
    // The lock is in the per-user %TEMP%. The pipe namespace is shared by the machine,
    // so a launch checks whose process answers (see peerIsCurrentUser).
    return {QDir(QDir::tempPath()).filePath(group + QStringLiteral(".lock")), group};
#endif
}

} // namespace mervin::ipc
