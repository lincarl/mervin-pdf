#include "ipc/PipeName.h"

#include "config/ConfigPaths.h"
#include "platform/PlatformIntegration.h"

#include <QCryptographicHash>
#include <QDir>

namespace mervin::ipc {

namespace {

// A short, stable, filesystem/pipe-safe token identifying the current user.
QString userToken()
{
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = QStringLiteral("default");

    const QByteArray digest =
        QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(16));
}

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

} // namespace mervin::ipc
