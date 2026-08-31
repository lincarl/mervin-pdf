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

bool PlatformIntegration::isDefaultPdfHandler()
{
    // A strict Snap can't observe the host's MIME associations: the kde-neon
    // extension redirects $XDG_CONFIG_HOME into the sandbox and the `home`
    // interface forbids reading the real ~/.config, so xdg-mime here would query
    // a confined copy, never the desktop's mimeapps.list. Report "not default"
    // (the safe answer) rather than claim a state we can't verify.
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
    // A strict Snap cannot set the host's default PDF handler from inside the
    // sandbox: xdg-mime would write the confined $XDG_CONFIG_HOME (which the
    // desktop ignores), the `home` interface forbids touching the real
    // ~/.config/mimeapps.list, and snapd's io.snapcraft.Settings proxy only
    // accepts default-web-browser / default-url-scheme-handler - never a MIME
    // type like application/pdf. So don't silently no-op: return false and let
    // the caller point the user at the desktop's Default Applications settings
    // (the Snap-exported .desktop advertises MimeType=application/pdf, so the
    // manual route works). See docs/design.md.
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
