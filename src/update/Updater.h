#pragma once

#include "update/UpdatePlan.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QTemporaryDir;
QT_END_NAMESPACE

namespace mervin {

// Primary-process updater singleton. On startup, auto-update checks the release API
// when 30 days have passed, downloads the matching package, verifies SHA-256,
// installs it and restarts. Manual checks use the same installation flow and show
// progress. Unsupported/dev/portable copies link to the release. MSIX copies
// open Microsoft Store for manual checks and never contact the release API.
// Asset URLs come only from the configured release API response. Its state in
// QSettings, the downloads and the installation steps touch the disk on one
// background thread, in order, as the profile can be on a share that stopped
// answering; each step goes on when its disk work answers. Each package kind
// keeps its own check date, pending update and downloads, so copies of different
// kinds run by one user never install or delete each other's packages. Until a
// kind has saved its own check date, it uses the one older versions shared.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    static Updater *instance(); // nullptr when none exists

    // This package kind's last definite check or Windows installer handoff (UTC).
    // Until this kind has saved one, the date older versions shared for all kinds
    // (update/lastCheckUtc), which a Store copy, never checking itself, keeps.
    // Invalid if neither exists, or until it has been read at start (see
    // lastCheckChanged). A failed check, or an answer that cannot be read, leaves
    // the previous date.
    static QDateTime lastCheck();

    // Whether this copy can download and install updates itself. Portable and dev
    // copies cannot. MSIX delegates updates to Microsoft Store.
    bool canSelfUpdate() const
    {
        return kind_ != update::PackageKind::None && kind_ != update::PackageKind::Msix;
    }

    // Run once after the first window is up. When automatic updates are enabled,
    // resumes a pending installation or checks when due. The check and download
    // are asynchronous, so startup never waits on the network. Either way it
    // deletes this kind's downloads it no longer needs, such as one that is not
    // newer than this copy.
    void onStartup();

    // Settings -> Updates -> Check for Updates checks now regardless of the last check or setting,
    // downloads and installs available updates, and reports "up to date" and errors.
    // For MSIX copies it opens the Store's Downloads and updates page instead.
    void checkNow();

    // Auto update was switched on or off through WindowManager. Off cancels
    // background checks, downloads and automatic installations that have not
    // started, including one that went back to wait for a dialog. A manual
    // operation, or an installation step already running, runs to its end.
    void onAutoUpdateChanged(bool on);

    // How the windows answered the request to close them for an installation.
    enum class CloseResult {
        Closed,   // the windows are closing, and the updater quits next
        Declined, // the user chose Cancel in a save prompt
        Busy,     // an operation, a chosen save or another dialog keeps a window open
    };
    Q_ENUM(CloseResult)

    // The application supplies its explicit quit path, which bypasses close-to-tray.
    void setCloseWindowsHandler(std::function<CloseResult()> handler)
    {
        closeWindows_ = std::move(handler);
    }

    // What a manual installation says when Mervin is too busy to close for it.
    static QString closeRefusedText();

    // Waits up to `ms` milliseconds for the disk work asked for so far, such as a
    // pending update put back as the app quits. For the end of the process; works
    // after the updater is gone. True when nothing is left.
    static bool waitForDisk(int ms);

signals:
    // The automatic check interval was reset at `utc`; lastCheck() now returns it.
    void checked(const QDateTime &utc);
    // lastCheck() changed: read from the profile at start, or reset by a check.
    void lastCheckChanged(const QDateTime &utc);

private:
    friend class TestUpdater;

    // Runs `work` on the disk thread after what was asked before, then `done` with
    // its result here, unless the updater is gone or, `forOperation`, the operation
    // that asked has ended meanwhile (see operation_).
    template <class Work, class Done>
    void onDisk(Work work, Done done, bool forOperation = true);
    // For tests: disk work, or its completion, has not finished.
    static bool diskBusy();
    // For tests, with the disk thread idle. Opens `path` as the download in progress.
    static bool openPartForTesting(const QString &path);

    void check(bool manual);
    void onReleaseReply(QNetworkReply *reply);
    void download(const update::ReleaseAsset &asset, const QString &version);
    void showProgress();
    void onDownloadFinished();
    void installWhenIdle(); // manual checks can finish over Settings
    void pollInstall();
    bool shouldWaitForDialogs() const;
    // Copies and verifies `file` into private staging before installation.
    // Requires the expected SHA-256 obtained from the HTTPS release API.
    void install(const QString &file, const QString &version, const QString &sha256);
    void installStaged(const QString &file, const QString &version, const QString &sha256);
    void offerManualDownload(const QString &version, const QString &pageUrl);
    void clearStaging();
    void handOffToInstaller(const QString &file, const QString &version, const QString &sha256);
    void installWithPackageManager(const QString &file, const QString &version);
    void runPackageManager(const QStringList &command, const QString &file, const QString &version);
    void restart(const QString &exe);
    void endOperation();
    CloseResult closeWindows();
    // After closeWindows() did not close, a manual operation that found Mervin
    // busy says so. A declined save prompt needs no message.
    void reportCloseRefused(CloseResult result) const;
    void markChecked();
    void startDownload(); // the network part of download(), once the .part file is open

    const update::PackageKind kind_;
    const QString exePath_; // captured at start: a package upgrade replaces the file behind it
    QNetworkAccessManager *nam_;
    bool busy_ = false;        // a check, download or installation is running or queued
    bool manual_ = false;      // ...on behalf of Settings, so progress and outcomes show
    bool installQueued_ = false; // a verified download is waiting for other dialogs
    bool installing_ = false;  // do not remove the package once installation starts
    QPointer<QNetworkReply> reply_;
    // While a download runs: whether a write to its .part file failed. The file
    // itself belongs to the disk thread.
    std::shared_ptr<std::atomic<bool>> partFailed_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    update::ReleaseAsset asset_;
    QString downloadVersion_;
    QString releasePageUrl_;
    // Only this independent, verified copy reaches an installer. The original
    // cache file remains pending for retries. Cleared on the disk thread.
    std::shared_ptr<QTemporaryDir> staging_;
    QString stagedFile_;
    QPointer<QProgressDialog> progress_;
    std::function<CloseResult()> closeWindows_;
    QDateTime lastCheck_;
    bool lastCheckKnown_ = false; // read at start, or set by a check
    quint64 operation_ = 0;       // bumped by endOperation(), see onDisk()
};

} // namespace mervin
