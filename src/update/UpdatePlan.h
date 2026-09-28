#pragma once

#include <QByteArray>
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
// and source installs, which never update themselves.
enum class PackageKind { None, NsisSetup, Msi, AppImage, Deb, Rpm };

// The automatic check runs on every kStartsPerCheck-th start.
inline constexpr int kStartsPerCheck = 20;

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

// "v1.9.0" and "1.9.0" parse to the same version. Only numeric segments count,
// so pre-release suffixes are ignored (the "latest" endpoint never serves them).
QVersionNumber parseVersion(QString text);

// A GitHub "latest release" API response, or nullopt when it has no usable
// version tag.
std::optional<Release> parseRelease(const QByteArray &json);

// The asset that updates a `kind` install, or nullopt when the release has none
// (or kind is None). Releases only carry x86-64 builds. Only assets with a
// SHA-256 digest count, since nothing else can be verified before it runs, and
// only plain file names, since the name becomes a path in the updates folder.
// `osVersion` is QSysInfo::productVersion(): a .deb built for that Ubuntu
// release is preferred when a release carries several.
std::optional<ReleaseAsset> assetFor(const QList<ReleaseAsset> &assets, PackageKind kind,
                                     const QString &osVersion);

// Classifies a Windows copy. Both installers record their folder under
// HKCU\Software\Mervin PDF\InstallDir (the MSI with a trailing backslash); only
// that registered copy updates, and only NSIS leaves uninstall.exe beside the exe.
PackageKind windowsPackageKind(const QString &exeDir, const QString &registeredInstallDir,
                               bool hasUninstaller);

// Classifies a Linux copy. `appImage` is $APPIMAGE when it names a file and
// `appDir` is $APPDIR; the AppImage runtime exports both, and every program
// started from inside ANY AppImage (say, its built-in terminal) inherits them,
// so only an exe inside `appDir` is that image. `buildFormat` is
// MERVIN_PACKAGE_FORMAT, which counts only for a copy running from /usr.
PackageKind linuxPackageKind(const QString &exePath, const QString &appImage,
                             const QString &appDir, QStringView buildFormat);

} // namespace mervin::update
