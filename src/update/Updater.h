#pragma once

#include "update/UpdatePlan.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <utility>

QT_BEGIN_NAMESPACE
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
QT_END_NAMESPACE

namespace mervin {

// Primary-process updater singleton. On startup, auto-update checks the release API
// when 30 days have passed, downloads the matching package, verifies SHA-256,
// installs it and restarts. Manual checks use the same installation flow and show
// progress. Unsupported/dev/portable copies link to the release. MSIX copies
// open Microsoft Store for manual checks and never contact the release API.
// Asset URLs come only from the configured release API response.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    static Updater *instance(); // nullptr when none exists

    // Last definite check or Windows installer handoff (UTC); invalid if never.
    // A failed check leaves the previous date.
    static QDateTime lastCheck();

    // Whether this copy can download and install updates itself. Portable and dev
    // copies cannot. MSIX delegates updates to Microsoft Store.
    bool canSelfUpdate() const
    {
        return kind_ != update::PackageKind::None && kind_ != update::PackageKind::Msix;
    }

    // Run once after the first window is up. When automatic updates are enabled,
    // resumes a pending installation or checks when due. The check and download
    // are asynchronous, so startup never waits on the network.
    void onStartup();

    // Settings -> Updates -> Check for Updates checks now regardless of the last check or setting,
    // downloads and installs available updates, and reports "up to date" and errors.
    // For MSIX copies it opens the Store's Downloads and updates page instead.
    void checkNow();

    // Auto update was switched on or off through WindowManager. Off cancels
    // background checks, downloads and queued installations. A manual operation
    // or an installation that has already started runs to its end.
    void onAutoUpdateChanged(bool on);

    // The application supplies its explicit quit path, which bypasses close-to-tray.
    void setCloseWindowsHandler(std::function<bool()> handler) { closeWindows_ = std::move(handler); }

signals:
    // The automatic check interval was reset at `utc`; lastCheck() now returns it.
    void checked(const QDateTime &utc);

private:
    friend class TestUpdater;

    void check(bool manual);
    void onReleaseReply(QNetworkReply *reply);
    void download(const update::ReleaseAsset &asset, const QString &version);
    void showProgress();
    void onDownloadFinished();
    void installWhenIdle(); // manual checks can finish over Settings
    void pollInstall();
    void install(const QString &file, const QString &version);
    void installWithPackageManager(const QString &file, const QString &version);
    void restart(const QString &exe);
    void endOperation();
    bool closeWindows();
    void markChecked();

    const update::PackageKind kind_;
    const QString exePath_; // captured at start: a package upgrade replaces the file behind it
    QNetworkAccessManager *nam_;
    bool busy_ = false;        // a check, download or installation is running or queued
    bool manual_ = false;      // ...on behalf of Settings, so progress and outcomes show
    bool installQueued_ = false; // a verified download is waiting for other dialogs
    bool installing_ = false;  // do not remove the package once installation starts
    QPointer<QNetworkReply> reply_;
    QFile *part_ = nullptr;    // the download in progress (<asset>.part)
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    update::ReleaseAsset asset_;
    QString downloadVersion_;
    QPointer<QProgressDialog> progress_;
    std::function<bool()> closeWindows_;
};

} // namespace mervin
