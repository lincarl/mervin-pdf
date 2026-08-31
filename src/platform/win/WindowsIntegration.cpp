#include "platform/PlatformIntegration.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QSettings>
#include <QString>
#include <QUrl>

namespace mervin {

namespace {

constexpr auto kProgId = "MervinPDF.Document";

QString exePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

} // namespace

bool PlatformIntegration::registerPdfHandlerAndPromptDefault()
{
    const QString exe = exePath();
    const QString openCmd = QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1"));

    // ProgID: how to open a Mervin-associated document.
    {
        QSettings progid(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1").arg(QLatin1String(kProgId)),
            QSettings::NativeFormat);
        progid.setValue(QStringLiteral("."), QStringLiteral("PDF Document"));
        progid.setValue(QStringLiteral("shell/open/command/."), openCmd);
        progid.setValue(QStringLiteral("DefaultIcon/."), QStringLiteral("\"%1\",0").arg(exe));
        if (progid.status() != QSettings::NoError)
            return false;
    }

    // Offer the ProgID as an option for .pdf (does not steal the default).
    {
        QSettings assoc(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.pdf\\OpenWithProgids"),
            QSettings::NativeFormat);
        assoc.setValue(QLatin1String(kProgId), QString());
    }

    // Application capabilities + RegisteredApplications, so Mervin shows up in
    // Settings -> Default Apps as a .pdf handler.
    {
        QSettings caps(QStringLiteral("HKEY_CURRENT_USER\\Software\\MervinPDF\\Capabilities"),
                       QSettings::NativeFormat);
        caps.setValue(QStringLiteral("ApplicationName"), QStringLiteral("Mervin PDF"));
        caps.setValue(QStringLiteral("ApplicationDescription"),
                      QStringLiteral("Lightweight PDF reader"));
        // Icon shown in Settings -> Default Apps and the "open with" picker;
        // index 0 is the icon embedded in the exe (see resources/icons).
        caps.setValue(QStringLiteral("ApplicationIcon"), QStringLiteral("\"%1\",0").arg(exe));
        caps.setValue(QStringLiteral("FileAssociations/.pdf"), QLatin1String(kProgId));
    }
    {
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\RegisteredApplications"),
                      QSettings::NativeFormat);
        reg.setValue(QStringLiteral("MervinPDF"),
                     QStringLiteral("Software\\MervinPDF\\Capabilities"));
    }

    // Let the user confirm in the OS UI (silent association is not permitted).
    // Deep-link straight to Mervin's own Default Apps page (Win11 21H2+ with the
    // 2023-04 cumulative update) so the .pdf association is right there with a
    // one-click "Set default"; older builds ignore the query and show the list.
    // registeredAppUser matches the per-user name written above under HKCU
    // \Software\RegisteredApplications.
    const QString deepLink = QStringLiteral("ms-settings:defaultapps?registeredAppUser=%1")
                                 .arg(QString::fromUtf8(
                                     QUrl::toPercentEncoding(QStringLiteral("MervinPDF"))));
    QDesktopServices::openUrl(QUrl(deepLink));
    return true;
}

bool PlatformIntegration::isDefaultPdfHandler()
{
    // Windows records the user's chosen default for an extension under
    // FileExts\<ext>\UserChoice as the ProgId value (set via Settings -> Default
    // Apps or the "Open with" dialog). If it names our ProgID, Mervin is the
    // current default. Missing key / different ProgID -> not the default.
    QSettings userChoice(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\Explorer\\FileExts\\.pdf\\UserChoice"),
        QSettings::NativeFormat);
    return userChoice.value(QStringLiteral("ProgId"))
               .toString()
               .compare(QLatin1String(kProgId), Qt::CaseInsensitive)
           == 0;
}

} // namespace mervin
