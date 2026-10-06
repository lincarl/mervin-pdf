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
// when 30 days have passed, downloads the matching package and verifies SHA-256.
// Install Now installs; Later prompts next start; Never disables updates and deletes
// the download. Manual checks show
// progress; unsupported/dev/portable copies link to the release. Asset URLs come only from the
// configured release API response.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    static Updater *instance(); // nullptr when none exists

    // When the last check got a definite answer (UTC); invalid if never. A failed
    // check leaves the previous date.
    static QDateTime lastCheck();

    // Whether this copy can download and install updates itself. Portable and dev
    // copies cannot: they never check on their own, and a manual check only
    // links to the release page.
    bool canSelfUpdate() const { return kind_ != update::PackageKind::None; }

    // Run once after the first window is up. Offers a download an earlier
    // "Later" left pending, then runs the automatic check
    // when it is due. The check and download are asynchronous, so startup never
    // waits on the network.
    void onStartup();

    // Settings -> General -> Check for Updates checks now regardless of the last check or setting,
    // shows download progress, and reports "up to date" and errors.
    void checkNow();

    // Auto update was switched on or off (Settings or Never, through
    // WindowManager). Off stops a background download and deletes any
    // downloaded update.
    void onAutoUpdateChanged(bool on);

    // The application supplies its explicit quit path, which bypasses close-to-tray.
    void setCloseWindowsHandler(std::function<bool()> handler) { closeWindows_ = std::move(handler); }

signals:
    // The user answered Never. main.cpp routes this to
    // WindowManager::setAutoUpdate(false) so every window's settings agree.
    void autoUpdateDisabled();

    // A check got a definite answer at `utc`; lastCheck() now returns it.
    void checked(const QDateTime &utc);

private:
    friend class TestUpdater;

    void check(bool manual);
    void onReleaseReply(QNetworkReply *reply);
    void download(const update::ReleaseAsset &asset, const QString &version);
    void showProgress();
    void onDownloadFinished();
    void askWhenIdle(bool manual = false); // manual checks can prompt over Settings
    void pollAsk();
    void askToInstall();
    void install(const QString &file, const QString &version);
    void installWithPackageManager(const QString &file, const QString &version);
    void restart(const QString &exe);
    void endOperation();
    bool closeWindows();
    void markChecked();

    const update::PackageKind kind_;
    const QString exePath_; // captured at start: a package upgrade replaces the file behind it
    QNetworkAccessManager *nam_;
    bool busy_ = false;        // a check or download is running
    bool manual_ = false;      // ...on behalf of Settings, so progress and outcomes show
    bool askQueued_ = false;   // an Install Now / Later / Never prompt is waiting to show
    bool askManual_ = false;   // an explicit check must report back while Settings is open
    QPointer<QNetworkReply> reply_;
    QFile *part_ = nullptr;    // the download in progress (<asset>.part)
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    update::ReleaseAsset asset_;
    QString downloadVersion_;
    QPointer<QProgressDialog> progress_;
    std::function<bool()> closeWindows_;
};

} // namespace mervin
