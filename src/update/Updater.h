#pragma once

#include "update/UpdatePlan.h"

#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
QT_END_NAMESPACE

namespace mervin {

// Keeps an installed Mervin current from its GitHub Releases. With auto update
// on (Settings::autoUpdate, the default), every update::kStartsPerCheck-th start
// looks for a newer release, quietly downloads the asset for this copy's
// package kind (see update::PackageKind), verifies its SHA-256, and then asks:
// Install Now, Later (asked again on every start) or Never (auto update off and
// the download deleted). About -> Check for Updates runs the same check on
// demand with visible progress. Copies that cannot update themselves (dev
// builds, portable copies) only point the user at the release page.
//
// One instance lives for the primary process (runUi in main.cpp); reach it
// through instance(). The only URL requested directly is the release API in
// ReleaseConfig.h; download URLs come from its answer.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    static Updater *instance(); // nullptr when none exists

    // Run once after the first window is up. Offers a download an earlier
    // "Later" left pending, then counts this start and runs the automatic check
    // when it is due. The check and download are asynchronous, so startup never
    // waits on the network.
    void onStartup();

    // About -> Check for Updates: checks now whatever the start count or setting,
    // shows download progress, and reports "up to date" and errors.
    void checkNow();

    // Auto update was switched on or off (Settings or Never, through
    // WindowManager). Off stops a background download and deletes any
    // downloaded update.
    void onAutoUpdateChanged(bool on);

signals:
    // The user answered Never. main.cpp routes this to
    // WindowManager::setAutoUpdate(false) so every window's settings agree.
    void autoUpdateDisabled();

private:
    friend class TestUpdater;

    void check(bool manual);
    void onReleaseReply(QNetworkReply *reply);
    void download(const update::ReleaseAsset &asset, const QString &version);
    void showProgress();
    void onDownloadFinished();
    void askWhenIdle(bool manual = false); // manual checks can prompt over About
    void pollAsk();
    void askToInstall();
    void install(const QString &file, const QString &version);
    void installWithPackageManager(const QString &file, const QString &version);
    void restart(const QString &exe);
    void endOperation();

    const update::PackageKind kind_;
    const QString exePath_; // captured at start: a package upgrade replaces the file behind it
    QNetworkAccessManager *nam_;
    bool busy_ = false;        // a check or download is running
    bool manual_ = false;      // ...on behalf of About, so progress and outcomes show
    bool askQueued_ = false;   // an Install Now / Later / Never prompt is waiting to show
    bool askManual_ = false;   // an explicit check must report back while About is open
    QPointer<QNetworkReply> reply_;
    QFile *part_ = nullptr;    // the download in progress (<asset>.part)
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    update::ReleaseAsset asset_;
    QString downloadVersion_;
    QPointer<QProgressDialog> progress_;
};

} // namespace mervin
