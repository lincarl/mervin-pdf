#include "platform/PlatformIntegration.h"

#include <QLatin1String>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace mervin {

namespace {

// True when running inside a confined Snap. snapd exports $SNAP / $SNAP_INSTANCE_NAME
// to every app it launches; their absence means a normal (.deb/.rpm/AppImage) install.
bool isSnap()
{
    return qEnvironmentVariableIsSet("SNAP") || qEnvironmentVariableIsSet("SNAP_INSTANCE_NAME");
}

// The desktop-entry id of our handler, as xdg-mime identifies it. The package /
// AppImage install packaging/linux/mervin-pdf.desktop as "mervin-pdf.desktop",
// but snapd renames it to "<snap>_<app>.desktop" when confined.
QString desktopId()
{
    if (isSnap())
        return QStringLiteral("mervin-pdf_mervin-pdf.desktop");
    return QStringLiteral("mervin-pdf.desktop");
}

// Run a short-lived helper and return its trimmed stdout (empty on any failure).
QString runCapture(const QString &program, const QStringList &args)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(2000) || !proc.waitForFinished(3000))
        return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

} // namespace

bool PlatformIntegration::hasPackageIdentity()
{
    return false;
}

bool PlatformIntegration::isDefaultPdfHandler()
{
    // Snap confinement hides the host MIME configuration. Report false rather than trusting the
    // confined xdg-mime result.
    if (isSnap())
        return false;

    // `xdg-mime query default application/pdf` prints the .desktop id of the
    // current default handler (empty when none is set). Missing xdg-mime ->
    // empty -> "not default", which is the safe answer.
    const QString current =
        runCapture(QStringLiteral("xdg-mime"),
                   {QStringLiteral("query"), QStringLiteral("default"),
                    QStringLiteral("application/pdf")});
    return current.compare(desktopId(), Qt::CaseInsensitive) == 0;
}

bool PlatformIntegration::registerPdfHandlerAndPromptDefault()
{
    // Snap cannot modify the host MIME configuration, and its settings proxy supports only
    // browser/URL handlers. Return false so callers direct users to desktop settings; the
    // exported .desktop advertises application/pdf.
    if (isSnap())
        return false;

    // On Linux the .desktop file (installed by the .deb/.rpm, or bundled in the
    // AppImage) already advertises MimeType=application/pdf, so it shows up as an
    // "Open with" option automatically. Making it the *default* is one xdg-mime
    // call - there is no OS confirmation page like Windows has.
    QProcess proc;
    proc.start(QStringLiteral("xdg-mime"),
               {QStringLiteral("default"), desktopId(),
                QStringLiteral("application/pdf")});
    if (!proc.waitForStarted(2000) || !proc.waitForFinished(3000))
        return false;
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

} // namespace mervin
