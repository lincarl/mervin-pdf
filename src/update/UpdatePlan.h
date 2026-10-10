#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVersionNumber>

#include <optional>

// The network- and UI-free half of the updater: reading a GitHub release,
// choosing the asset that matches how this copy was installed, and recognising
// the Windows installs. Kept pure so tst_update_plan can pin it down without a
// network or an installed app.
namespace mervin::update {

// How this copy of Mervin was installed. It decides which release asset replaces
// it and how that asset is installed. None covers dev builds, portable copies
// and source installs, which never update themselves. Windows and the Microsoft
// Store manage Msix updates; that kind must never use GitHub release assets.
enum class PackageKind { None, Msi, Msix, AppImage, Deb, Rpm };

inline constexpr int kDaysPerCheck = 30;

// Check on startup after 30 days. Missing or future timestamps need a fresh
// check, including profiles migrating from the old start counter.
bool automaticCheckDue(const QDateTime &lastCheck, const QDateTime &now);

struct ReleaseAsset
{
    QString name;
    QString url;    // browser_download_url
    QString sha256; // lower-case hex from GitHub's "sha256:<hex>" digest; empty if absent
};

struct Release
{
    QString tag;            // "v1.64.0"
    QVersionNumber version; // parsed from the tag
    QString pageUrl;        // the release's web page
    QList<ReleaseAsset> assets;
};

// Stable versions have exactly three numeric segments. Reject suffixes so a
// prerelease cannot become an update candidate or a pending stable installer.
QVersionNumber parseVersion(QString text);

// Updates stay on the stable channel. A stable release replaces an installed
// release candidate with the same numeric version, but never a newer base version.
bool isNewerStableVersion(const QVersionNumber &candidate, QString installedVersion);

// A GitHub "latest release" API response, or nullopt for a draft, prerelease,
// or unusable version tag.
std::optional<Release> parseRelease(const QByteArray &json);

// Choose a package-kind asset or nullopt. Require an x86-64 build, SHA-256 digest and plain
// filename. For .deb alternatives, prefer QSysInfo::productVersion via osVersion.
std::optional<ReleaseAsset> assetFor(const QList<ReleaseAsset> &assets, PackageKind kind,
                                     const QString &osVersion);

// Only the MSI copy running from HKCU\Software\Mervin PDF\InstallDir updates.
// Build trees and copies of the installation folder never update themselves.
PackageKind windowsPackageKind(const QString &exeDir, const QString &registeredInstallDir);

// Classifies a Linux copy. `appImage` is $APPIMAGE when it names a file and
// `appDir` is $APPDIR; the AppImage runtime exports both, and every program
// started from inside ANY AppImage (say, its built-in terminal) inherits them,
// so only an exe inside `appDir` is that image. `buildFormat` is
// MERVIN_PACKAGE_FORMAT, which counts only for a copy running from /usr.
PackageKind linuxPackageKind(const QString &exePath, const QString &appImage,
                             const QString &appDir, QStringView buildFormat);

} // namespace mervin::update
