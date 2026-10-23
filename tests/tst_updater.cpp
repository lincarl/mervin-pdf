#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "i18n/UiLanguage.h"
#include "mervin_version.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"
#include "update/Updater.h"
#include "update/Installer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMessageBox>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProgressDialog>
#include <QProcess>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QSettings>

#include <atomic>
#include <memory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <cstdio>

#ifndef Q_OS_WIN
#  include <unistd.h>
#endif

namespace {

using mervin::update::PackageKind;

const QString kVersion = QStringLiteral("9999.123.456");
const QByteArray kDownload = QByteArrayLiteral("a locally generated update fixture");
const QString kDownloadSha256 =
    QString::fromLatin1(QCryptographicHash::hash(kDownload, QCryptographicHash::Sha256).toHex());
QStringList installerFiles;
QList<QByteArray> installedBytes;
QStringList packageCommand; // fixture program, never a real installer
std::atomic<bool> packageCommandReady{false};
std::function<void()> beforePackageCommand;
std::function<void()> beforeInstallerReturns;
bool installerLaunchSucceeds = true;
PackageKind installedKind = PackageKind::Msi;
bool appImageReplaced = false;
std::atomic<bool> appImageCopyStarted{false};
std::atomic<bool> appImageCopyHeld{false}; // the copy, on the disk thread, waits while set
QStringList appImageReplacements;          // the files the AppImage copy was asked to install
QStringList packageInstalls;               // the files handed to the package manager
QMutex installStubsLock;                   // both stubs run on the disk thread

// The release asset of each kind for kVersion, as release.yml names it.
QString assetName(PackageKind kind)
{
    switch (kind) {
    case PackageKind::AppImage:
        return QStringLiteral("MervinPDF-9999.123.456-x86_64.AppImage");
    case PackageKind::Rpm:
        return QStringLiteral("mervin-pdf-9999.123.456.x86_64.rpm");
    case PackageKind::Deb:
        return QStringLiteral("mervin-pdf_9999.123.456_ubuntu26.04_amd64.deb");
    default:
        return QStringLiteral("MervinPDF-9999.123.456.msi");
    }
}

// The stored name of a kind's state and folder, spelled out to pin it.
QString kindName(PackageKind kind)
{
    switch (kind) {
    case PackageKind::AppImage:
        return QStringLiteral("appimage");
    case PackageKind::Deb:
        return QStringLiteral("deb");
    case PackageKind::Rpm:
        return QStringLiteral("rpm");
    case PackageKind::Msix:
        return QStringLiteral("msix");
    default:
        return QStringLiteral("msi");
    }
}

// A key of `kind`'s update state in QSettings; by default this test's copy.
QString stateKey(QStringView name, PackageKind kind = installedKind)
{
    return QStringLiteral("update/%1/%2").arg(kindName(kind), name);
}

// The shared keys versions before the per-kind state used.
const QString kLegacyLastCheck = QStringLiteral("update/lastCheckUtc");
const QString kLegacyPendingVersion = QStringLiteral("update/pendingVersion");
const QString kLegacyPendingFile = QStringLiteral("update/pendingFile");

bool setAutoUpdate(bool enabled)
{
    mervin::Settings settings = mervin::Settings::load();
    settings.autoUpdate = enabled;
    return settings.save();
}

// Delivers bytes without a network request. Record abort even after finished:
// closing QProgressDialog used to emit canceled during successful completion.
class DownloadReply : public QNetworkReply
{
public:
    explicit DownloadReply(QObject *parent, QByteArray data = kDownload,
                           NetworkError error = NoError)
        : QNetworkReply(parent), data_(std::move(data))
    {
        open(QIODevice::ReadOnly);
        if (error != NoError)
            setError(error, QStringLiteral("Network fixture failure"));
    }

    void abort() override
    {
        ++abortCount;
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
    }

    void finish()
    {
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return data_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

    int abortCount = 0;

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = qMin(maxSize, data_.size() - offset_);
        if (!count)
            return -1;
        std::memcpy(data, data_.constData() + offset_, size_t(count));
        offset_ += count;
        return count;
    }

private:
    QByteArray data_;
    qint64 offset_ = 0;
};

// Exercise the real check path without contacting GitHub or downloading a release.
class ReleaseNetworkAccessManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    int requests = 0;
    QList<QUrl> requestedUrls;
    QList<Operation> requestedOperations;
    bool hasUpdate = false;
    QByteArray noUpdateBody = QByteArrayLiteral(R"({"tag_name":"v0.0.0"})");
    QString offeredDigest = kDownloadSha256;
    QString offeredAsset = assetName(PackageKind::Msi); // the release's one asset

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requests;
        requestedUrls.append(request.url());
        requestedOperations.append(operation);
        QByteArray body = noUpdateBody;
        if (hasUpdate && requests == 1) {
            const QJsonObject asset{
                {QStringLiteral("name"), offeredAsset},
                {QStringLiteral("browser_download_url"), QStringLiteral("https://example.invalid/update")},
                {QStringLiteral("digest"), QStringLiteral("sha256:") + offeredDigest},
            };
            body = QJsonDocument(QJsonObject{
                {QStringLiteral("tag_name"), QStringLiteral("v9999.123.456")},
                {QStringLiteral("html_url"), QStringLiteral("https://example.invalid/release")},
                {QStringLiteral("assets"), QJsonArray{asset}},
            }).toJson();
        } else if (hasUpdate) {
            body = kDownload;
        }
        auto *reply = new DownloadReply(this, body, error);
        QTimer::singleShot(0, reply, &DownloadReply::finish);
        return reply;
    }
};

// Dismiss result/error dialogs so failures cannot hang the suite. Any obsolete
// install-confirmation question is recorded and rejected, never accepted.
class PromptObserver
{
public:
    PromptObserver()
    {
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible())
                    continue;
                if (box->windowTitle() == QStringLiteral("Update Ready"))
                    ++confirmations;
                messages.append(box->text());
                box->reject();
            }
        });
        timer_.start(1);
    }

    int confirmations = 0;
    QStringList messages;

private:
    QTimer timer_;
};

} // namespace

// Link substitutes only into this test executable. Record successful handoff
// without replacing an app. Full-flow cases run the real application event loop
// and verify that the handoff requests exit.
namespace mervin::update {

PackageKind installedPackageKind() { return installedKind; }
bool startWindowsInstaller(const QString &file)
{
    installerFiles.append(file);
    QFile bytes(file);
    if (bytes.open(QIODevice::ReadOnly))
        installedBytes.append(bytes.readAll());
    if (beforeInstallerReturns)
        beforeInstallerReturns();
    return installerLaunchSucceeds;
}
bool replaceAppImage(const QString &file, const QString &, QString *)
{
    {
        const QMutexLocker lock(&installStubsLock);
        appImageReplacements.append(file);
    }
    appImageCopyStarted = true;
    for (int i = 0; appImageCopyHeld.load() && i < 1000; ++i)
        QThread::msleep(10);
    return appImageReplaced;
}
// Return only a fixture command. Missing package managers never open packages.
QStringList packageInstallCommand(PackageKind, const QString &file)
{
    const QMutexLocker lock(&installStubsLock);
    packageInstalls.append(file);
    if (beforePackageCommand)
        beforePackageCommand();
    packageCommandReady = true;
    return packageCommand.isEmpty() ? QStringList{} : packageCommand + QStringList{file};
}
QStringList relaunchArguments() { return {}; }
bool relaunchAfterExit(const QString &, const QStringList &) { return false; }

} // namespace mervin::update

namespace mervin {

class TestUpdater : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void completedCheckUpdatesSchedule_data();
    void completedCheckUpdatesSchedule();
    void manualCompletionInstallsOverSettings_data();
    void manualCompletionInstallsOverSettings();
    void handoffQuitsBeforeSettingsLosesItsWindow();
    void automaticHandoffWaitsForADialogOpenedMeanwhile();
#ifndef Q_OS_WIN // read-only folders do not stop writes on Windows
    void unwrittenConsumeStopsTheHandoff();
#endif
    void automaticRestartWaitsForADialogOpenedDuringTheCopy();
    void quittingWaitsForTheDiskWithinItsTime();
    void automaticCompletionWaitsForSettings();
    void queuedAutomaticInstallCanBecomeManual();
    void startupRespectsSetting_data();
    void startupRespectsSetting();
    void pendingUpdateOfThisKindOnly_data();
    void pendingUpdateOfThisKindOnly();
    void replacementDuringCloseUsesStagedBytes_data();
    void replacementDuringCloseUsesStagedBytes();
    void hashlessPendingRequiresRelease_data();
    void hashlessPendingRequiresRelease();
    void stagedPackageManagerLifetime_data();
    void stagedPackageManagerLifetime();
    void missingPackageManagerKeepsRetry();
    void alteredDownloadIsNotInstalled_data();
    void alteredDownloadIsNotInstalled();
    void checkDatesArePerKind_data();
    void checkDatesArePerKind();
    void legacyDownloadsOfThisKindGo_data();
    void legacyDownloadsOfThisKindGo();
    void installRefusesAnotherKindsPackage_data();
    void installRefusesAnotherKindsPackage();
    void msixDelegatesToStore_data();
    void msixDelegatesToStore();
    void recordStoreUrl(const QUrl &url);
    void recordOpenedFile(const QUrl &url);
    void releaseInstallsWithoutConfirmation_data();
    void releaseInstallsWithoutConfirmation();
    void disablingCancelsQueuedInstall();
    void disablingDuringStaging_data();
    void disablingDuringStaging();
    void disablingDuringDownload_data();
    void disablingDuringDownload();
    void invalidDownloadDoesNotInstall_data();
    void invalidDownloadDoesNotInstall();
    void closeVetoRetainsPendingUpdate();
    void busyCloseExplainsManualInstall_data();
    void busyCloseExplainsManualInstall();
    void failedInstallerLaunchRetainsPendingUpdate();
    void installingIgnoresDuplicateCheckAndDisable();
    void cancelStillAborts_data();
    void cancelStillAborts();
    void progressFitsVersion_data();
    void progressFitsVersion();

private:
    bool eventFilter(QObject *object, QEvent *event) override;
    DownloadReply *prepareDownload(Updater &updater, bool manual,
                                   QNetworkReply::NetworkError error = QNetworkReply::NoError);
    // Where a `kind` copy keeps its download of kVersion; by default this test's copy.
    static QString downloadedPath(PackageKind kind = installedKind);
    // Records a verified download of `version` for `kind`, and a check just now.
    static bool savePending(PackageKind kind = installedKind,
                            const QString &version = kVersion);
    // Records kVersion's `kind` package as versions before the per-kind state did,
    // as a download of `version`.
    static bool saveLegacyPending(PackageKind kind, const QString &version = kVersion);
    ReleaseNetworkAccessManager *useLocalNetwork(Updater &updater);

    QTemporaryDir profile_;
    QString previousProfile_;
    QFont previousFont_;
    QString previousStyle_;
    int quitRequests_ = 0;
    QList<QUrl> storeUrls_;
    QList<QUrl> openedFiles_; // handed to the desktop, see recordOpenedFile

    // The updater touches the disk on a background thread; true once that work,
    // and what it hands back, is done.
    static bool settled()
    {
        return QTest::qWaitFor([] { return !Updater::diskBusy(); }, 10000);
    }
};

void TestUpdater::initTestCase()
{
    // PromptObserver detects the removed confirmation by its English title.
    i18n::apply(QStringLiteral("en"));
    QVERIFY(profile_.isValid());
    previousProfile_ = ConfigPaths::overrideDir();
    ConfigPaths::setOverrideDir(profile_.path());
    // All updater state is under the disposable profile on every platform,
    // including Windows where the default QSettings backend is the registry.
    QCoreApplication::setOrganizationName(QStringLiteral("MervinUpdaterTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UpdaterTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile_.path());
    QApplication::setQuitOnLastWindowClosed(false);
    previousFont_ = QApplication::font();
    previousStyle_ = qApp->styleSheet();
    qApp->installEventFilter(this);
}

bool TestUpdater::eventFilter(QObject *object, QEvent *event)
{
    if (object == qApp && event->type() == QEvent::Quit) {
        ++quitRequests_;
    }
    return QObject::eventFilter(object, event);
}

void TestUpdater::init()
{
    QVERIFY(settled()); // nothing of the last test's lands in this one
    installedKind = PackageKind::Msi;
    storeUrls_.clear();
    openedFiles_.clear();
    QSettings().clear();
    installerFiles.clear();
    installedBytes.clear();
    packageCommand.clear();
    packageCommandReady = false;
    beforePackageCommand = {};
    quitRequests_ = 0;
    beforeInstallerReturns = {};
    installerLaunchSucceeds = true;
    appImageReplaced = false;
    appImageCopyHeld = false;
    appImageCopyStarted = false;
    appImageReplacements.clear();
    packageInstalls.clear();
    QVERIFY(setAutoUpdate(true));
    // Empty the updates folder. ConfigPaths makes it only once per process.
    const QDir updates(ConfigPaths::updatesDir());
    for (const QFileInfo &entry :
         updates.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        QVERIFY(entry.isDir() ? QDir(entry.absoluteFilePath()).removeRecursively()
                              : QFile::remove(entry.absoluteFilePath()));
    }
}

void TestUpdater::cleanup()
{
    appImageCopyHeld = false; // a failed test can leave the copy waiting
    QVERIFY(settled());
    QDesktopServices::unsetUrlHandler(QStringLiteral("ms-windows-store"));
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qApp->setStyleSheet(previousStyle_);
    QApplication::setFont(previousFont_);
}

void TestUpdater::cleanupTestCase()
{
    QVERIFY(mervin::appfiles::flush(10000));
    ConfigPaths::setOverrideDir(previousProfile_);
}

void TestUpdater::completedCheckUpdatesSchedule_data()
{
    QTest::addColumn<bool>("manual");
    QTest::addColumn<bool>("networkError");
    QTest::addColumn<QByteArray>("body"); // the release API's answer, with HTTP 200
    QTest::addColumn<QString>("message"); // what a manual check says
    const QByteArray current = R"({"tag_name":"v0.0.0"})";
    // A TLS-inspecting proxy or content filter answering with its own page.
    const QByteArray unreadable = "<html><body>Blocked by your network policy</body></html>";
    const QString upToDate = QStringLiteral("You're up to date.");
    const QString failed = QStringLiteral("Couldn't check for updates right now.");
    QTest::newRow("automatic-success") << false << false << current << QString();
    QTest::newRow("manual-success") << true << false << current << upToDate;
    QTest::newRow("automatic-network-error") << false << true << current << QString();
    QTest::newRow("manual-network-error") << true << true << current << failed;
    QTest::newRow("automatic-unreadable") << false << false << unreadable << QString();
    QTest::newRow("manual-unreadable")
        << true << false << unreadable
        << failed + QStringLiteral("\n\nThe update server's answer couldn't be read.");
}

void TestUpdater::completedCheckUpdatesSchedule()
{
    QFETCH(bool, manual);
    QFETCH(bool, networkError);
    QFETCH(QByteArray, body);
    QFETCH(QString, message);
    // Only a definite answer resets the interval.
    const bool fails = networkError || !body.contains("tag_name");
    const QString key = stateKey(u"lastCheckUtc");
    const QDateTime previous = QDateTime::currentDateTimeUtc().addDays(-1);
    QSettings().setValue(key, previous);
    QSettings().setValue(QStringLiteral("update/startsSinceCheck"), 19);
    Updater updater;
    delete updater.nam_;
    auto *network = new ReleaseNetworkAccessManager(&updater);
    updater.nam_ = network;
    network->noUpdateBody = body;
    if (networkError)
        network->error = QNetworkReply::ConnectionRefusedError;
    PromptObserver observer;
    // Settings > General shows the date and follows this signal while open.
    QSignalSpy checked(&updater, &Updater::checked);
    const QDateTime started = QDateTime::currentDateTimeUtc();
    if (manual)
        updater.checkNow(); // A recent check must not block an explicit request.
    else
        updater.check(false);
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCOMPARE(network->requests, 1);
    QCOMPARE(network->requestedUrls, QList<QUrl>{QUrl(QStringLiteral(
        "https://api.github.com/repos/lincarl/mervin-pdf/releases/latest"))});
    QCOMPARE(network->requestedOperations,
             QList<QNetworkAccessManager::Operation>{QNetworkAccessManager::GetOperation});
    QSettings state;
    state.sync();
    const QDateTime lastCheck = state.value(key).toDateTime();
    QCOMPARE(Updater::lastCheck(), lastCheck);
    if (fails) {
        QCOMPARE(lastCheck, previous);
        QCOMPARE(checked.count(), 0); // a failed check keeps the old date
    } else {
        QCOMPARE(checked.count(), 1);
        QCOMPARE(checked.at(0).at(0).toDateTime(), lastCheck);
        QVERIFY(lastCheck >= started);
        QVERIFY(lastCheck <= QDateTime::currentDateTimeUtc());
        QVERIFY(!state.contains(QStringLiteral("update/startsSinceCheck")));
        QVERIFY(!update::automaticCheckDue(lastCheck, QDateTime::currentDateTimeUtc()));
    }
    if (manual) {
        QCOMPARE(observer.messages.size(), 1);
        QVERIFY2(observer.messages.first().startsWith(message), qPrintable(observer.messages.first()));
    } else {
        QVERIFY(observer.messages.isEmpty());
    }
}

QString TestUpdater::downloadedPath(PackageKind kind)
{
    return QDir(ConfigPaths::updatesDir())
        .filePath(kindName(kind) + QLatin1Char('/') + assetName(kind));
}

DownloadReply *TestUpdater::prepareDownload(Updater &updater, bool manual,
                                           QNetworkReply::NetworkError error)
{
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
    updater.busy_ = true;
    updater.manual_ = manual;
    updater.downloadVersion_ = kVersion;
    updater.asset_.name = assetName(installedKind);
    updater.asset_.sha256 = kDownloadSha256;
    if (!settled() || !QDir().mkpath(QFileInfo(downloadedPath()).absolutePath())
        || !Updater::openPartForTesting(downloadedPath() + QStringLiteral(".part")))
        return nullptr;
    updater.partFailed_ = std::make_shared<std::atomic<bool>>(false);
    auto *reply = new DownloadReply(&updater, kDownload, error);
    updater.reply_ = reply;
    QObject::connect(reply, &QNetworkReply::finished, &updater, &Updater::onDownloadFinished);
    if (manual)
        updater.showProgress();
    return reply;
}

void TestUpdater::manualCompletionInstallsOverSettings_data()
{
    QTest::addColumn<bool>("startedManually");
    QTest::newRow("manual-download") << true;
    QTest::newRow("background-download-promoted-by-check-now") << false;
}

void TestUpdater::manualCompletionInstallsOverSettings()
{
    QFETCH(bool, startedManually);
    Updater updater;
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QCOMPARE(QApplication::activeModalWidget(), &settingsDialog);

    DownloadReply *reply = prepareDownload(updater, startedManually);
    QVERIFY(reply);
    if (!startedManually)
        updater.checkNow();
    QVERIFY(updater.progress_);
    QProgressDialog *progress = updater.progress_;
    QSignalSpy canceled(progress, &QProgressDialog::canceled);
    PromptObserver observer;
    QTRY_VERIFY(progress->isVisible());
    // QWidget::activateWindow has no null-window equivalent for simulating
    // another application's screenshot overlay taking activation away.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(nullptr); // e.g. opening the screen snipper
    QT_WARNING_POP
    QCOMPARE(QApplication::activeWindow(), nullptr);
    QVERIFY(progress->isVisible());
    QCOMPARE(canceled.count(), 0);

    reply->finish();
    // Completion must not travel through the user-cancel signal or delete the
    // verified update. Manual installation proceeds even while Settings is modal.
    QCOMPARE(canceled.count(), 0);
    QCOMPARE(reply->abortCount, 0);
    QVERIFY(!progress->isVisible());
    // The installer gets the download through the pending state, which the handoff
    // then consumes, and the download is kept.
    QTRY_COMPARE_WITH_TIMEOUT(installerFiles.size(), 1, 2000);
    QVERIFY(installerFiles.first() != downloadedPath());
    QCOMPARE(installedBytes.last(), kDownload);
    QFile download(downloadedPath());
    QVERIFY(download.open(QIODevice::ReadOnly));
    QCOMPARE(download.readAll(), kDownload);
    QCOMPARE(observer.confirmations, 0);
    QVERIFY(observer.messages.isEmpty());
    QVERIFY(settingsDialog.isVisible());
    QVERIFY(!updater.installQueued_);
    QVERIFY(updater.busy_);
    QVERIFY(updater.installing_);
}

// In the app, the windows closed for the handoff are deleted once an event loop
// runs, and a Settings dialog, their child, runs its own. The handoff quits in
// the same call, so the dialog's loop ends before its window goes.
void TestUpdater::handoffQuitsBeforeSettingsLosesItsWindow()
{
    QVERIFY(setAutoUpdate(false));
    auto *window = new QMainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setAttribute(Qt::WA_DontShowOnScreen);
    window->show();
    QPointer<QMainWindow> alive(window);
    bool settingsOpen = false;
    bool deletedUnderSettings = false;
    QObject context;
    QObject::connect(window, &QObject::destroyed, &context, [&] {
        deletedUnderSettings = settingsOpen;
        QCoreApplication::quit(); // as WindowManager does when the last window goes
    });
    Updater updater;
    updater.setCloseWindowsHandler([alive] {
        if (alive)
            alive->close();
        return Updater::CloseResult::Closed;
    });
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    PromptObserver observer;
    QTimer::singleShot(0, &context, [&] {
        // On the heap, so a window deleted under it fails the test, not the process.
        QPointer<QDialog> settings = new QDialog(window);
        settings->setAttribute(Qt::WA_DontShowOnScreen);
        settings->setModal(true);
        QTimer::singleShot(0, settings, [&] { updater.checkNow(); });
        settingsOpen = true;
        settings->exec();
        settingsOpen = false;
        delete settings;
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, qApp, [] { QCoreApplication::exit(42); });
    timeout.start(5000);
    QCOMPARE(QApplication::exec(), 0);
    QCOMPARE(installerFiles.size(), 1);
    QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!alive);
    QVERIFY(!deletedUnderSettings);
    QVERIFY(observer.messages.isEmpty());
}

// The pending update is consumed before the Windows handoff. A dialog opened
// meanwhile puts it back, and the automatic installation waits for the dialog.
void TestUpdater::automaticHandoffWaitsForADialogOpenedMeanwhile()
{
    QVERIFY(savePending());
    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
    QVERIFY(settled());
    updater.busy_ = true;
    updater.installing_ = true;
    updater.install(downloadedPath(), kVersion, kDownloadSha256);
    QDialog dialog; // shown before the consume answers
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.setModal(true);
    dialog.show();
    QVERIFY(settled());
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(updater.installQueued_);
    QVERIFY(!updater.installing_);
    QCOMPARE(QSettings().value(stateKey(u"pendingFile")).toString(), downloadedPath());
    QCOMPARE(QSettings().value(stateKey(u"pendingSha256")).toString(), kDownloadSha256);
    dialog.hide();
    QTRY_COMPARE(installerFiles.size(), 1);
    QVERIFY(settled());
    QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
}

#ifndef Q_OS_WIN
// Without a written consume, a failed or cancelled MSI would relaunch this copy
// into the same installation again, so the handoff does not happen.
void TestUpdater::unwrittenConsumeStopsTheHandoff()
{
    if (::geteuid() == 0)
        QSKIP("Read-only files do not stop root");
    QVERIFY(savePending());
    QVERIFY(settled());
    const QString ini = QSettings().fileName();
    const QString folder = QFileInfo(ini).absolutePath();
    const auto folderPermissions = QFile::permissions(folder);
    const auto iniPermissions = QFile::permissions(ini);
    const auto writable = qScopeGuard([&] {
        QFile::setPermissions(folder, folderPermissions);
        QFile::setPermissions(ini, iniPermissions);
    });
    QVERIFY(QFile::setPermissions(ini, QFileDevice::ReadOwner));
    QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    Updater updater;
    int closes = 0;
    updater.setCloseWindowsHandler([&] {
        ++closes;
        return Updater::CloseResult::Closed;
    });
    PromptObserver observer;
    updater.busy_ = true;
    updater.installing_ = true;
    updater.install(downloadedPath(), kVersion, kDownloadSha256);
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCOMPARE(closes, 0);
    QVERIFY(installerFiles.isEmpty());
    QTRY_COMPARE(observer.messages.size(), 1);
    QVERIFY(observer.messages.first().contains(QStringLiteral("Couldn't start the installer")));
    QFile file(ini);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().contains("pendingFile"));
}
#endif

// The AppImage copy runs in the background while the windows stay usable. An
// automatic update does not close them over a dialog opened meanwhile.
void TestUpdater::automaticRestartWaitsForADialogOpenedDuringTheCopy()
{
    installedKind = PackageKind::AppImage;
    QVERIFY(savePending());
    appImageReplaced = true;
    appImageCopyHeld = true;
    Updater updater;
    useLocalNetwork(updater);
    int closes = 0;
    bool dialogOpenAtClose = false;
    updater.setCloseWindowsHandler([&] {
        ++closes;
        dialogOpenAtClose = QApplication::activeModalWidget() != nullptr;
        return Updater::CloseResult::Declined; // the user keeps a window, which ends the operation
    });
    updater.onStartup();
    QTRY_VERIFY(updater.installing_);
    QDialog dialog;
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.setModal(true);
    dialog.show();
    QCOMPARE(QApplication::activeModalWidget(), &dialog);
    appImageCopyHeld = false;
    QVERIFY(settled());
    QTest::qWait(1500);
    QCOMPARE(closes, 0);
    QVERIFY(updater.installing_);
    dialog.hide();
    QTRY_COMPARE(closes, 1);
    QVERIFY(!dialogOpenAtClose);
    QVERIFY(!updater.busy_);
    QCOMPARE(appImageReplacements.size(), 1);
    QVERIFY(appImageReplacements.first() != downloadedPath());
}

// Quitting waits a bounded time for the updater's disk work, such as a pending
// update put back after the installer did not start.
void TestUpdater::quittingWaitsForTheDiskWithinItsTime()
{
    installedKind = PackageKind::AppImage;
    appImageReplaced = true;
    appImageCopyHeld = true;
    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Declined; });
    updater.busy_ = true;
    updater.installing_ = true;
    QVERIFY(savePending());
    // The copy holds the disk thread.
    updater.install(downloadedPath(), kVersion, kDownloadSha256);
    QTRY_VERIFY(appImageCopyStarted.load());
    QVERIFY(!Updater::waitForDisk(50));
    appImageCopyHeld = false;
    QVERIFY(Updater::waitForDisk(5000));
    QVERIFY(settled());
    QCOMPARE(appImageReplacements.size(), 1);
}

void TestUpdater::automaticCompletionWaitsForSettings()
{
    Updater updater;
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QCOMPARE(QApplication::activeModalWidget(), &settingsDialog);
    DownloadReply *reply = prepareDownload(updater, false);
    QVERIFY(reply);
    PromptObserver observer;
    reply->finish();

    // Let the initial queued poll run while Settings is still modal, then allow
    // its real retry timer to continue once Settings closes.
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(updater.installQueued_);
    QVERIFY(updater.busy_);
    QVERIFY(QFile::exists(downloadedPath()));
    settingsDialog.close();
    QTRY_COMPARE_WITH_TIMEOUT(installerFiles.size(), 1, 2000);
    QCOMPARE(observer.confirmations, 0);
    QVERIFY(!updater.installQueued_);
    QVERIFY(updater.busy_);
}

void TestUpdater::queuedAutomaticInstallCanBecomeManual()
{
    Updater updater;
    auto *network = useLocalNetwork(updater);
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QVERIFY(savePending());
    PromptObserver observer;

    updater.onStartup();
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QVERIFY(updater.installQueued_);
    QVERIFY(installerFiles.isEmpty());
    updater.checkNow();
    updater.checkNow(); // repeated clicks must not start another installation
    QTRY_COMPARE_WITH_TIMEOUT(installerFiles.size(), 1, 2000);
    QCOMPARE(network->requests, 0);
    QCOMPARE(observer.confirmations, 0);
    QVERIFY(observer.messages.isEmpty());
    QVERIFY(settingsDialog.isVisible());
    QVERIFY(!updater.installQueued_);
    QVERIFY(updater.busy_);
    QVERIFY(updater.installing_);
}

namespace {

bool writeDownload(const QString &path)
{
    QFile download(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && download.open(QIODevice::WriteOnly)
        && download.write(kDownload) == kDownload.size();
}

} // namespace

bool TestUpdater::savePending(PackageKind kind, const QString &version)
{
    if (!writeDownload(downloadedPath(kind)))
        return false;
    QSettings st;
    st.setValue(stateKey(u"pendingVersion", kind), version);
    st.setValue(stateKey(u"pendingFile", kind), downloadedPath(kind));
    st.setValue(stateKey(u"pendingSha256", kind), kDownloadSha256);
    st.setValue(stateKey(u"lastCheckUtc", kind), QDateTime::currentDateTimeUtc());
    return true;
}

bool TestUpdater::saveLegacyPending(PackageKind kind, const QString &version)
{
    // Older versions kept their downloads in the updates folder itself.
    const QString file = QDir(ConfigPaths::updatesDir()).filePath(assetName(kind));
    if (!writeDownload(file))
        return false;
    QSettings st;
    st.setValue(kLegacyPendingVersion, version);
    st.setValue(kLegacyPendingFile, file);
    return true;
}

ReleaseNetworkAccessManager *TestUpdater::useLocalNetwork(Updater &updater)
{
    delete updater.nam_;
    auto *network = new ReleaseNetworkAccessManager(&updater);
    updater.nam_ = network;
    return network;
}

void TestUpdater::recordStoreUrl(const QUrl &url)
{
    storeUrls_.append(url);
}

// Stands in for the desktop's software installer, so no test opens a real file.
void TestUpdater::recordOpenedFile(const QUrl &url)
{
    openedFiles_.append(url);
}

void TestUpdater::msixDelegatesToStore_data()
{
    QTest::addColumn<bool>("pending");
    QTest::newRow("fresh Store installation") << false;
    QTest::newRow("coexisting MSI has a pending update") << true;
}

void TestUpdater::msixDelegatesToStore()
{
    QFETCH(bool, pending);
    installedKind = PackageKind::Msix;
    if (pending)
        QVERIFY(savePending(PackageKind::Msi));
    const QDateTime previousCheck = QDateTime::currentDateTimeUtc().addDays(-31);
    // Never checking itself, the Store copy shows the date an older MSI copy saved.
    QSettings().setValue(kLegacyLastCheck, previousCheck);
    const QString previousPending =
        QSettings().value(stateKey(u"pendingFile", PackageKind::Msi)).toString();
    Updater updater; // reads the last check, on its disk thread
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    QDesktopServices::setUrlHandler(QStringLiteral("ms-windows-store"), this, "recordStoreUrl");
    QSignalSpy checked(&updater, &Updater::checked);

    QVERIFY(!updater.canSelfUpdate());
    updater.onStartup();
    updater.checkNow();
    updater.onAutoUpdateChanged(false);
    updater.onAutoUpdateChanged(true);
    QVERIFY(settled());
    QCoreApplication::processEvents();

    QCOMPARE(network->requests, 0);
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(quitRequests_, 0);
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installQueued_);
    QVERIFY(!updater.installing_);
    QCOMPARE(storeUrls_, QList<QUrl>{QUrl(QStringLiteral("ms-windows-store://downloadsandupdates"))});
    QCOMPARE(checked.count(), 0);
    QCOMPARE(Updater::lastCheck(), previousCheck);
    QCOMPARE(QSettings().value(stateKey(u"pendingFile", PackageKind::Msi)).toString(),
             previousPending);
    if (pending) {
        QCOMPARE(QSettings().value(stateKey(u"pendingVersion", PackageKind::Msi)).toString(),
                 kVersion);
        QVERIFY(QFile::exists(downloadedPath(PackageKind::Msi)));
    }
}

void TestUpdater::startupRespectsSetting_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QString>("pendingVersion"); // empty when nothing was downloaded
    const QString installed = QStringLiteral(MERVIN_VERSION_STRING); // this copy's own
    QTest::newRow("disabled-no-pending") << false << QString();
    QTest::newRow("disabled-pending") << false << kVersion;
    // A finished update leaves nothing behind, with or without automatic updates.
    QTest::newRow("disabled-installed") << false << installed;
    QTest::newRow("enabled-no-pending") << true << QString();
    QTest::newRow("enabled-pending") << true << kVersion;
    QTest::newRow("enabled-installed") << true << installed;
}

void TestUpdater::startupRespectsSetting()
{
    QFETCH(bool, enabled);
    QFETCH(QString, pendingVersion);
    const bool pending = pendingVersion == kVersion;
    QVERIFY(setAutoUpdate(enabled));
    if (!pendingVersion.isEmpty())
        QVERIFY(savePending(installedKind, pendingVersion));
    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
    auto *network = useLocalNetwork(updater);
    PromptObserver observer;
    updater.onStartup();
    if (enabled && pending)
        QTRY_COMPARE(installerFiles.size(), 1);
    else if (enabled)
        QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCoreApplication::processEvents();
    // A saved download comes with a check just now.
    QCOMPARE(network->requests, enabled && pendingVersion.isEmpty() ? 1 : 0);
    QCOMPARE(installerFiles.size(), enabled && pending ? 1 : 0);
    QCOMPARE(observer.confirmations, 0);
    QCOMPARE(QFile::exists(downloadedPath()), pending);
    QCOMPARE(QSettings().contains(stateKey(u"pendingFile")), pending && !enabled);
    QCOMPARE(updater.busy_, enabled && pending);
}

void TestUpdater::pendingUpdateOfThisKindOnly_data()
{
    QTest::addColumn<PackageKind>("copyKind");
    QTest::addColumn<PackageKind>("recordKind");
    QTest::addColumn<bool>("legacy"); // saved by a version before the per-kind state
    QTest::addColumn<bool>("manual");
    const PackageKind appImage = PackageKind::AppImage;
    const PackageKind deb = PackageKind::Deb;
    QTest::newRow("own, start") << appImage << appImage << false << false;
    QTest::newRow("own from an older version, start") << appImage << appImage << true << false;
    QTest::newRow("deb, start") << appImage << deb << false << false;
    QTest::newRow("deb from an older version, start") << appImage << deb << true << false;
    QTest::newRow("deb, Check for Updates") << appImage << deb << false << true;
    QTest::newRow("deb from an older version, Check for Updates") << appImage << deb << true << true;
    // The reverse, where the .deb install finds the AppImage's download.
    QTest::newRow("deb copy, own, start") << deb << deb << false << false;
    QTest::newRow("deb copy, AppImage, start") << deb << appImage << false << false;
    QTest::newRow("deb copy, AppImage from an older version, start")
        << deb << appImage << true << false;
    QTest::newRow("deb copy, AppImage from an older version, Check for Updates")
        << deb << appImage << true << true;
}

// An AppImage beside a .deb install shares the user's settings and updates folder.
// Each copy installs only its own kind's download and leaves the other's alone.
void TestUpdater::pendingUpdateOfThisKindOnly()
{
    QFETCH(PackageKind, copyKind);
    QFETCH(PackageKind, recordKind);
    QFETCH(bool, legacy);
    QFETCH(bool, manual);
    installedKind = copyKind;
    appImageReplaced = true;
    QVERIFY(legacy ? saveLegacyPending(recordKind) : savePending(recordKind));
    QSettings().setValue(stateKey(u"lastCheckUtc"), QDateTime::currentDateTimeUtc());
    const QString recordFile = legacy ? QSettings().value(kLegacyPendingFile).toString()
                                      : downloadedPath(recordKind);
    const QString versionKey = legacy ? kLegacyPendingVersion : stateKey(u"pendingVersion", recordKind);
    const QString fileKey = legacy ? kLegacyPendingFile : stateKey(u"pendingFile", recordKind);

    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Declined; }); // the restart ends the operation
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    network->offeredAsset = assetName(copyKind);
    QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordOpenedFile");
    PromptObserver observer;
    if (manual)
        updater.checkNow();
    else
        updater.onStartup();
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCoreApplication::processEvents();

    const QStringList installed = appImageReplacements + packageInstalls;
    if (recordKind == copyKind) {
        // Legacy metadata is discarded and reacquired from the release API.
        QCOMPARE(installed.size(), 1);
        QVERIFY(installed.first() != recordFile);
        QCOMPARE(network->requests, legacy ? 2 : 0);
    } else if (manual) {
        // A check downloads this copy's own package instead of reusing the other's.
        QCOMPARE(network->requests, 2);
        QCOMPARE(installed.size(), 1);
        QVERIFY(installed.first() != downloadedPath());
    } else {
        QVERIFY2(installed.isEmpty(), qPrintable(installed.join(u' ')));
        QCOMPARE(network->requests, 0);
    }
    // A desktop file launch cannot own installer completion or staging cleanup.
    QVERIFY(openedFiles_.isEmpty());
    if (recordKind != copyKind) {
        QSettings st;
        QCOMPARE(st.value(versionKey).toString(), kVersion);
        QCOMPARE(st.value(fileKey).toString(), recordFile);
        QVERIFY(QFile::exists(recordFile));
    }
    if (manual && copyKind == PackageKind::Deb)
        QCOMPARE(observer.messages, QStringList{QStringLiteral("Mervin PDF %1 is available.").arg(kVersion)});
    else
        QVERIFY(observer.messages.isEmpty());
}

// The cached source can be replaced while a save prompt runs. Installation must
// use the bytes copied and verified before entering that nested event loop.
void TestUpdater::replacementDuringCloseUsesStagedBytes_data()
{
    QTest::addColumn<bool>("replacePath");
    QTest::addColumn<bool>("decline");
    QTest::newRow("replace path, install") << true << false;
    QTest::newRow("overwrite bytes, install") << false << false;
    QTest::newRow("replace path, cancel") << true << true;
    QTest::newRow("overwrite bytes, cancel") << false << true;
}

void TestUpdater::replacementDuringCloseUsesStagedBytes()
{
    QFETCH(bool, replacePath);
    QFETCH(bool, decline);
    QVERIFY(savePending());
    QString staged;
    QString directory;
    bool changed = false;
    bool privatePermissions = true;
    Updater updater;
    updater.setCloseWindowsHandler([&] {
        staged = updater.stagedFile_;
        directory = QFileInfo(staged).absolutePath();
#ifndef Q_OS_WIN
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        privatePermissions = !(QFile::permissions(directory) & others)
            && !(QFile::permissions(staged) & others);
#endif
        QDialog prompt;
        QTimer::singleShot(0, &prompt, [&] {
            if (replacePath && !QFile::remove(downloadedPath()))
                return prompt.reject();
            QFile source(downloadedPath());
            changed = source.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && source.write("replaced during save prompt") > 0;
            prompt.accept();
        });
        prompt.exec();
        return decline ? Updater::CloseResult::Declined : Updater::CloseResult::Closed;
    });
    updater.onStartup();
    QTRY_VERIFY(changed);
    QVERIFY(settled());
    QVERIFY(privatePermissions);
    QVERIFY(staged != downloadedPath());
    if (decline) {
        QVERIFY(installerFiles.isEmpty());
        QVERIFY(!QFileInfo::exists(directory));
        QCOMPARE(QSettings().value(stateKey(u"pendingFile")).toString(), downloadedPath());
        QCOMPARE(QSettings().value(stateKey(u"pendingSha256")).toString(), kDownloadSha256);
    } else {
        QCOMPARE(installerFiles, QStringList{staged});
        QCOMPARE(installedBytes, QList<QByteArray>{kDownload});
        // The detached helper owns this copy now. Destruction of Updater must
        // not remove it before msiexec has opened it.
        QVERIFY(QFileInfo::exists(staged));
    }
}

void TestUpdater::hashlessPendingRequiresRelease_data()
{
    QTest::addColumn<bool>("legacy");
    QTest::addColumn<bool>("networkFailure");
    QTest::addColumn<QString>("digest");
    QTest::newRow("legacy refreshed") << true << false << kDownloadSha256;
    QTest::newRow("per-kind refreshed") << false << false << kDownloadSha256;
    QTest::newRow("legacy offline") << true << true << kDownloadSha256;
    QTest::newRow("per-kind offline") << false << true << kDownloadSha256;
    QTest::newRow("release missing digest") << true << false << QString();
    QTest::newRow("release malformed digest") << false << false << QStringLiteral("ab12");
}

void TestUpdater::hashlessPendingRequiresRelease()
{
    QFETCH(bool, legacy);
    QFETCH(bool, networkFailure);
    QFETCH(QString, digest);
    QVERIFY(legacy ? saveLegacyPending(installedKind) : savePending());
    QSettings().remove(stateKey(u"pendingSha256"));
    QSettings().setValue(stateKey(u"lastCheckUtc"), QDateTime::currentDateTimeUtc());
    Updater updater;
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    network->offeredDigest = digest;
    if (networkFailure)
        network->error = QNetworkReply::ConnectionRefusedError;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Declined; });
    updater.onStartup();
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    const bool refreshed = !networkFailure && update::isSha256(digest);
    QCOMPARE(network->requests, refreshed ? 2 : 1);
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(QSettings().contains(stateKey(u"pendingFile")), refreshed);
    if (refreshed)
        QCOMPARE(QSettings().value(stateKey(u"pendingSha256")).toString(), kDownloadSha256);
    QVERIFY(!QSettings().contains(kLegacyPendingFile));
    if (networkFailure)
        QVERIFY(QSettings().value(stateKey(u"checkDue")).toBool());
}

// A harmless child program stands in for pkexec and reads the real staged file.
// Its completion is the boundary for cleanup, including cancellation and errors.
void TestUpdater::stagedPackageManagerLifetime_data()
{
    QTest::addColumn<PackageKind>("kind");
    QTest::addColumn<int>("exitCode");
    QTest::addColumn<bool>("launchFails");
    for (PackageKind kind : {PackageKind::Deb, PackageKind::Rpm}) {
        const char *name = kind == PackageKind::Deb ? "deb" : "rpm";
        QTest::addRow("%s success", name) << kind << 0 << false;
        QTest::addRow("%s cancellation", name) << kind << 126 << false;
        QTest::addRow("%s failure", name) << kind << 1 << false;
        QTest::addRow("%s failed launch", name) << kind << 0 << true;
    }
}

void TestUpdater::stagedPackageManagerLifetime()
{
    QFETCH(PackageKind, kind);
    QFETCH(int, exitCode);
    QFETCH(bool, launchFails);
    installedKind = kind;
    QVERIFY(savePending());
    const QString read = profile_.filePath(QStringLiteral("child-read"));
    const QString proceed = profile_.filePath(QStringLiteral("child-proceed"));
    QFile::remove(read);
    QFile::remove(proceed);
    packageCommand = launchFails
        ? QStringList{profile_.filePath(QStringLiteral("missing-installer"))}
        : QStringList{QCoreApplication::applicationFilePath(), QStringLiteral("--installer-fixture"),
                      read, proceed, QString::number(exitCode)};
    bool changed = false;
    beforePackageCommand = [&] {
        QFile original(downloadedPath());
        changed = original.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && original.write("changed after staging") > 0;
    };
    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Declined; });
    PromptObserver observer;
    updater.onStartup();
    QTRY_VERIFY(packageCommandReady.load());
    QVERIFY(changed);
    const QString staged = packageInstalls.first();
    const QString directory = QFileInfo(staged).absolutePath();
    QVERIFY(staged != downloadedPath());
    if (!launchFails) {
        QTRY_VERIFY(QFileInfo::exists(read));
        QFile copied(read);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), kDownload);
        QVERIFY(QFileInfo::exists(staged));
        QVERIFY(updater.installing_);
        QFile release(proceed);
        QVERIFY(release.open(QIODevice::WriteOnly));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!updater.busy_, 10000);
    QVERIFY(settled());
    QVERIFY(!QFileInfo::exists(directory));
    QCOMPARE(QSettings().value(stateKey(u"pendingFile")).toString(), downloadedPath());
    QCOMPARE(QSettings().value(stateKey(u"pendingSha256")).toString(), kDownloadSha256);
    QCOMPARE(observer.messages.isEmpty(), !launchFails && (exitCode == 0 || exitCode == 126));
}

void TestUpdater::missingPackageManagerKeepsRetry()
{
    installedKind = PackageKind::Deb;
    QVERIFY(savePending());
    Updater updater;
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    network->offeredAsset = assetName(installedKind);
    QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordOpenedFile");
    PromptObserver observer;
    updater.checkNow();
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCOMPARE(network->requests, 1);
    QCOMPARE(packageInstalls.size(), 1);
    QVERIFY(!QFileInfo::exists(QFileInfo(packageInstalls.first()).absolutePath()));
    QVERIFY(openedFiles_.isEmpty());
    QCOMPARE(QSettings().value(stateKey(u"pendingFile")).toString(), downloadedPath());
    QCOMPARE(QSettings().value(stateKey(u"pendingSha256")).toString(), kDownloadSha256);
    QCOMPARE(observer.messages, QStringList{QStringLiteral("Mervin PDF %1 is available.").arg(kVersion)});
}

void TestUpdater::alteredDownloadIsNotInstalled_data()
{
    QTest::addColumn<bool>("manual");
    QTest::newRow("automatic") << false;
    QTest::newRow("manual") << true;
}

// The download is hashed again before it installs. One changed on disk since it
// was verified is discarded, a manual check says so, and the next start checks.
void TestUpdater::alteredDownloadIsNotInstalled()
{
    QFETCH(bool, manual);
    QVERIFY(setAutoUpdate(!manual));
    QVERIFY(savePending());
    {
        QFile download(downloadedPath());
        QVERIFY(download.open(QIODevice::WriteOnly));
        QVERIFY(download.write("changed after it was verified") > 0);
    }
    {
        Updater updater;
        updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
        auto *network = useLocalNetwork(updater);
        network->hasUpdate = true;
        PromptObserver observer;
        if (manual)
            updater.checkNow(); // the same version is out, so it reuses the download
        else
            updater.onStartup();
        QTRY_VERIFY(!updater.busy_);
        QVERIFY(settled());
        QCoreApplication::processEvents();
        QVERIFY(installerFiles.isEmpty());
        QCOMPARE(network->requests, manual ? 1 : 0);
        QVERIFY(!QFile::exists(downloadedPath()));
        QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
        if (manual) {
            QCOMPARE(observer.messages,
                     QStringList{QStringLiteral(
                         "The downloaded update failed its integrity check and was discarded.")});
        } else {
            QVERIFY(observer.messages.isEmpty());
        }
    }

    // The check that found the discarded download does not count, so the next
    // start checks again, while Settings still shows when this copy last checked.
    const QDateTime checked = QSettings().value(stateKey(u"lastCheckUtc")).toDateTime();
    QVERIFY(!update::automaticCheckDue(checked, QDateTime::currentDateTimeUtc()));
    QVERIFY(setAutoUpdate(true));
    Updater next;
    auto *network = useLocalNetwork(next);
    QSignalSpy checks(&next, &Updater::checked);
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QCOMPARE(Updater::lastCheck(), checked);
    next.onStartup();
    QTRY_VERIFY(!next.busy_);
    QVERIFY(settled());
    QCOMPARE(network->requests, 1);
    QCOMPARE(checks.count(), 1);
    QVERIFY(!QSettings().contains(stateKey(u"checkDue"))); // that check reset the interval
}

void TestUpdater::checkDatesArePerKind_data()
{
    QTest::addColumn<int>("sharedAgeDays"); // age of the date older versions shared
    QTest::addColumn<bool>("appImageChecks");
    QTest::newRow("due by the shared date") << 40 << true;
    QTest::newRow("not due by the shared date") << 1 << false;
}

// The .deb copy's check does not put off the AppImage copy's own update. Until a
// kind has its own date, the one older versions shared still counts.
void TestUpdater::checkDatesArePerKind()
{
    QFETCH(int, sharedAgeDays);
    QFETCH(bool, appImageChecks);
    const QDateTime shared = QDateTime::currentDateTimeUtc().addDays(-sharedAgeDays);
    QSettings().setValue(kLegacyLastCheck, shared);
    const QDateTime started = QDateTime::currentDateTimeUtc();
    {
        installedKind = PackageKind::Deb;
        Updater deb;
        auto *network = useLocalNetwork(deb);
        PromptObserver observer;
        deb.checkNow();
        QTRY_VERIFY(!deb.busy_);
        QVERIFY(settled());
        QCOMPARE(network->requests, 1);
        QVERIFY(QSettings().value(stateKey(u"lastCheckUtc", PackageKind::Deb)).toDateTime()
                >= started);
    }
    // Older copies still read and write the shared date.
    QCOMPARE(QSettings().value(kLegacyLastCheck).toDateTime(), shared);

    installedKind = PackageKind::AppImage;
    Updater appImage;
    auto *network = useLocalNetwork(appImage);
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QCOMPARE(Updater::lastCheck(), shared);
    appImage.onStartup();
    QTRY_VERIFY(!appImage.busy_);
    QVERIFY(settled());
    QCOMPARE(network->requests, appImageChecks ? 1 : 0);
    QCOMPARE(QSettings().contains(stateKey(u"lastCheckUtc")), appImageChecks);
}

void TestUpdater::legacyDownloadsOfThisKindGo_data()
{
    QTest::addColumn<bool>("enabled"); // automatic updates
    QTest::newRow("automatic updates on") << true;
    QTest::newRow("automatic updates off") << false;
}

// An older version kept its record in the shared keys and its downloads in the
// updates folder itself. Once that update is installed, this kind's file, its
// unfinished download and the record go at the next start. Another kind's
// package in the same folder stays.
void TestUpdater::legacyDownloadsOfThisKindGo()
{
    QFETCH(bool, enabled);
    installedKind = PackageKind::AppImage;
    QVERIFY(setAutoUpdate(enabled));
    QVERIFY(saveLegacyPending(PackageKind::AppImage, QStringLiteral(MERVIN_VERSION_STRING)));
    QSettings().setValue(stateKey(u"lastCheckUtc"), QDateTime::currentDateTimeUtc());
    const QDir updates(ConfigPaths::updatesDir());
    const QString own = updates.filePath(assetName(PackageKind::AppImage));
    const QString part = updates.filePath(
        QStringLiteral("MervinPDF-9999.123.457-x86_64.AppImage.part")); // an interrupted download
    const QString other = updates.filePath(assetName(PackageKind::Deb));
    {
        QFile file(part);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write("partial") > 0);
    }
    {
        QFile file(other);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write(kDownload) == kDownload.size());
    }

    Updater updater;
    auto *network = useLocalNetwork(updater);
    PromptObserver observer;
    updater.onStartup();
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());

    QVERIFY(!QFile::exists(own));
    QVERIFY(!QFile::exists(part));
    QVERIFY(QFile::exists(other));
    QVERIFY(!QSettings().contains(kLegacyPendingVersion));
    QVERIFY(!QSettings().contains(kLegacyPendingFile));
    QCOMPARE(network->requests, 0);
    QVERIFY(appImageReplacements.isEmpty());
    QVERIFY(observer.messages.isEmpty());
}

void TestUpdater::installRefusesAnotherKindsPackage_data()
{
    QTest::addColumn<PackageKind>("copyKind");
    QTest::addColumn<PackageKind>("fileKind");
    QTest::newRow("AppImage copy, .deb") << PackageKind::AppImage << PackageKind::Deb;
    QTest::newRow(".deb copy, AppImage") << PackageKind::Deb << PackageKind::AppImage;
}

// The last guard. Whatever file reaches install(), another kind's package is not
// installed, opened or reported.
void TestUpdater::installRefusesAnotherKindsPackage()
{
    QFETCH(PackageKind, copyKind);
    QFETCH(PackageKind, fileKind);
    installedKind = copyKind;
    appImageReplaced = true;
    QVERIFY(savePending(fileKind));
    Updater updater;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Declined; });
    QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordOpenedFile");
    PromptObserver observer;
    QVERIFY(settled());
    updater.busy_ = true;
    updater.installing_ = true;
    updater.install(downloadedPath(fileKind), kVersion, kDownloadSha256);
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installing_);
    QVERIFY(appImageReplacements.isEmpty());
    QVERIFY(packageInstalls.isEmpty());
    QVERIFY(openedFiles_.isEmpty());
    QVERIFY(observer.messages.isEmpty());
    QVERIFY(QFile::exists(downloadedPath(fileKind)));
}

void TestUpdater::releaseInstallsWithoutConfirmation_data()
{
    QTest::addColumn<bool>("manual");
    QTest::addColumn<bool>("pending");
    QTest::newRow("automatic-download") << false << false;
    QTest::newRow("automatic-resumes-pending") << false << true;
    QTest::newRow("manual-download-auto-disabled") << true << false;
    QTest::newRow("manual-reuses-pending-auto-disabled") << true << true;
}

void TestUpdater::releaseInstallsWithoutConfirmation()
{
    QFETCH(bool, manual);
    QFETCH(bool, pending);
    QVERIFY(setAutoUpdate(!manual));
    if (pending) {
        QVERIFY(savePending());
        QSettings().setValue(stateKey(u"lastCheckUtc"), QDateTime::currentDateTimeUtc().addDays(-31));
    }
    {
        Updater updater;
        updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
        auto *network = useLocalNetwork(updater);
        network->hasUpdate = true;
        PromptObserver observer;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, qApp, [] { QCoreApplication::exit(42); });
        timeout.start(2000);
        if (manual)
            updater.checkNow();
        else
            updater.onStartup();
        QCOMPARE(QApplication::exec(), 0); // the updater must quit after handing off
        QCOMPARE(quitRequests_, 1);
        QCOMPARE(installerFiles.size(), 1);
        QCOMPARE(network->requests, pending ? (manual ? 1 : 0) : 2);
        QVERIFY(installerFiles.first() != downloadedPath());
        QCOMPARE(installedBytes.last(), kDownload);
        QVERIFY(observer.messages.isEmpty());
        QVERIFY(!update::automaticCheckDue(Updater::lastCheck(), QDateTime::currentDateTimeUtc()));
        QFile download(downloadedPath());
        QVERIFY(download.open(QIODevice::ReadOnly));
        QCOMPARE(download.readAll(), kDownload);
        QVERIFY(updater.busy_); // prevent another check until the handed-off app exits
        QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
        QVERIFY(!QSettings().contains(stateKey(u"pendingVersion")));
    }
    // A failed/cancelled MSI can relaunch the old binary. It must not run the
    // same installer again immediately, including a download resumed at startup.
    Updater resumed;
    auto *network = useLocalNetwork(resumed);
    resumed.onStartup();
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QCOMPARE(network->requests, 0);
    QCOMPARE(installerFiles.size(), 1);
    QVERIFY(!resumed.busy_);
    // The handed-off package goes at the next start, also without automatic updates.
    QVERIFY(!QFile::exists(downloadedPath()));
}

void TestUpdater::disablingCancelsQueuedInstall()
{
    Updater updater;
    auto *network = useLocalNetwork(updater);
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QVERIFY(savePending());
    PromptObserver observer;
    updater.onStartup();
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QVERIFY(updater.installQueued_);
    QVERIFY(setAutoUpdate(false));
    updater.onAutoUpdateChanged(false);
    settingsDialog.close();
    updater.pollInstall(); // also exercise a timer already posted before disable
    QVERIFY(settled());
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installQueued_);
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(network->requests, 0);
    QVERIFY(observer.messages.isEmpty());
}

void TestUpdater::disablingDuringStaging_data()
{
    QTest::addColumn<bool>("manual");
    QTest::newRow("automatic discarded") << false;
    QTest::newRow("manual retained") << true;
}

void TestUpdater::disablingDuringStaging()
{
    QFETCH(bool, manual);
    QVERIFY(savePending());
    Updater updater;
    int closes = 0;
    updater.setCloseWindowsHandler([&] {
        ++closes;
        return Updater::CloseResult::Declined;
    });
    updater.busy_ = true;
    updater.manual_ = manual;
    updater.install(downloadedPath(), kVersion, kDownloadSha256);
    QVERIFY(setAutoUpdate(false));
    updater.onAutoUpdateChanged(false);
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCOMPARE(closes, manual ? 1 : 0);
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(QFileInfo::exists(downloadedPath()), manual);
    QCOMPARE(QSettings().contains(stateKey(u"pendingFile")), manual);
    const QDir downloads(QFileInfo(downloadedPath()).absolutePath());
    QVERIFY(downloads.entryList({QStringLiteral(".install-*")},
                                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestUpdater::disablingDuringDownload_data()
{
    QTest::addColumn<bool>("manual");
    QTest::newRow("background-cancelled") << false;
    QTest::newRow("manual-continues") << true;
}

void TestUpdater::disablingDuringDownload()
{
    QFETCH(bool, manual);
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, manual);
    QVERIFY(reply);
    PromptObserver observer;
    QVERIFY(setAutoUpdate(false));
    updater.onAutoUpdateChanged(false);
    QCOMPARE(reply->abortCount, manual ? 0 : 1);
    reply->finish();
    if (manual)
        QTRY_COMPARE(installerFiles.size(), 1);
    else
        QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QCOMPARE(installerFiles.size(), manual ? 1 : 0);
    QCOMPARE(QFile::exists(downloadedPath()), manual);
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QCOMPARE(observer.confirmations, 0);
}

void TestUpdater::invalidDownloadDoesNotInstall_data()
{
    QTest::addColumn<QString>("digest");
    QTest::addColumn<bool>("networkFailure");
    QTest::newRow("hash-mismatch") << QString(64, QLatin1Char('0')) << false;
    QTest::newRow("missing-digest") << QString() << false;
    QTest::newRow("malformed-digest") << QStringLiteral("ab12") << false;
    QTest::newRow("network-failure") << kDownloadSha256 << true;
}

void TestUpdater::invalidDownloadDoesNotInstall()
{
    QFETCH(QString, digest);
    QFETCH(bool, networkFailure);
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, false,
        networkFailure ? QNetworkReply::ConnectionRefusedError : QNetworkReply::NoError);
    QVERIFY(reply);
    updater.asset_.sha256 = digest;
    PromptObserver observer;
    reply->finish();
    QVERIFY(settled());
    QCoreApplication::processEvents();
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installQueued_);
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
    QVERIFY(observer.messages.isEmpty());
}

void TestUpdater::closeVetoRetainsPendingUpdate()
{
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, false);
    QVERIFY(reply);
    int closeAttempts = 0;
    updater.setCloseWindowsHandler([&] {
        ++closeAttempts;
        return Updater::CloseResult::Declined; // the user cancels an unsaved-document prompt
    });
    PromptObserver observer;
    reply->finish();
    QTRY_COMPARE(closeAttempts, 1);
    QVERIFY(settled()); // the consumed update is put back
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installing_);
    QVERIFY(QFile::exists(downloadedPath()));
    QCOMPARE(QSettings().value(stateKey(u"pendingVersion")).toString(), kVersion);
    QVERIFY(observer.messages.isEmpty());

    // An explicit retry can use the same download without another transfer.
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    updater.setCloseWindowsHandler([] { return Updater::CloseResult::Closed; });
    updater.checkNow();
    QTRY_COMPARE(installerFiles.size(), 1);
    QCOMPARE(network->requests, 1);
}

void TestUpdater::busyCloseExplainsManualInstall_data()
{
    QTest::addColumn<PackageKind>("kind");
    QTest::addColumn<bool>("manual");
    QTest::addColumn<Updater::CloseResult>("answer");
    QTest::addColumn<bool>("explained");
    for (const PackageKind kind : {PackageKind::Msi, PackageKind::AppImage}) {
        const char *name = kind == PackageKind::Msi ? "MSI handoff" : "AppImage restart";
        QTest::addRow("%s, manual, busy", name) << kind << true << Updater::CloseResult::Busy << true;
        QTest::addRow("%s, manual, declined", name)
            << kind << true << Updater::CloseResult::Declined << false;
        QTest::addRow("%s, automatic, busy", name)
            << kind << false << Updater::CloseResult::Busy << false;
    }
}

// A manual installation that finds Mervin too busy to close says so, as nothing
// else would happen. Cancel in a save prompt and automatic updates stay quiet.
void TestUpdater::busyCloseExplainsManualInstall()
{
    QFETCH(PackageKind, kind);
    QFETCH(bool, manual);
    QFETCH(Updater::CloseResult, answer);
    QFETCH(bool, explained);
    installedKind = kind;
    appImageReplaced = true;
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, manual);
    QVERIFY(reply);
    int closeAttempts = 0;
    updater.setCloseWindowsHandler([&] {
        ++closeAttempts;
        return answer;
    });
    PromptObserver observer;
    reply->finish();
    QTRY_COMPARE(closeAttempts, 1);
    QTRY_VERIFY(!updater.busy_);
    QVERIFY(settled());
    QVERIFY(!updater.installing_);
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(quitRequests_, 0);
    // The download stays for the next attempt.
    QVERIFY(QFile::exists(downloadedPath()));
    QCOMPARE(QSettings().value(stateKey(u"pendingVersion")).toString(), kVersion);
    QCOMPARE(appImageReplacements.size(), kind == PackageKind::AppImage ? 1 : 0);
    QCOMPARE(observer.messages,
             explained ? QStringList{Updater::closeRefusedText()} : QStringList{});
}

void TestUpdater::failedInstallerLaunchRetainsPendingUpdate()
{
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, false);
    QVERIFY(reply);
    installerLaunchSucceeds = false;
    PromptObserver observer;
    reply->finish();
    QTRY_COMPARE(installerFiles.size(), 1);
    QVERIFY(settled()); // the consumed update is put back
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installing_);
    QVERIFY(QFile::exists(downloadedPath()));
    QCOMPARE(QSettings().value(stateKey(u"pendingVersion")).toString(), kVersion);
    QCOMPARE(observer.messages.size(), 1);
    QVERIFY(observer.messages.first().contains(QStringLiteral("Couldn't start the installer")));
    QCOMPARE(observer.confirmations, 0);
}

void TestUpdater::installingIgnoresDuplicateCheckAndDisable()
{
    Updater updater;
    auto *network = useLocalNetwork(updater);
    DownloadReply *reply = prepareDownload(updater, false);
    QVERIFY(reply);
    bool stayedBusy = false;
    bool keptDownload = false;
    beforeInstallerReturns = [&] {
        updater.onAutoUpdateChanged(false);
        updater.checkNow();
        stayedBusy = updater.busy_ && updater.installing_;
        keptDownload = QFile::exists(downloadedPath());
    };
    PromptObserver observer;
    reply->finish();
    QTRY_COMPARE(installerFiles.size(), 1);
    QVERIFY(stayedBusy);
    QVERIFY(keptDownload);
    QCOMPARE(network->requests, 0);
    QVERIFY(updater.busy_);
    QVERIFY(updater.installing_);
    QCOMPARE(observer.confirmations, 0);
}

void TestUpdater::cancelStillAborts_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("cancel-button") << 0;
    QTest::newRow("escape") << 1;
    QTest::newRow("window-close") << 2;
}

void TestUpdater::cancelStillAborts()
{
    QFETCH(int, action);
    Updater updater;
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    DownloadReply *reply = prepareDownload(updater, true);
    QVERIFY(reply);
    QProgressDialog *progress = updater.progress_;
    QVERIFY(progress);
    QSignalSpy canceled(progress, &QProgressDialog::canceled);
    PromptObserver observer;
    QTRY_VERIFY(progress->isVisible());
    progress->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), progress);

    if (action == 0) {
        auto *button = progress->findChild<QAbstractButton *>();
        QVERIFY(button);
        button->click();
    } else if (action == 1) {
        QTest::keyClick(progress, Qt::Key_Escape);
    } else {
        progress->close();
    }
    QCOMPARE(canceled.count(), 1);
    QCOMPARE(reply->abortCount, 1);
    reply->finish();
    QCOMPARE(reply->abortCount, 1);
    QVERIFY(settled());
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QVERIFY(!QSettings().contains(stateKey(u"pendingFile")));
    QVERIFY(!updater.installQueued_);
    QVERIFY(!updater.busy_);
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(observer.messages.isEmpty());
}

void TestUpdater::progressFitsVersion_data()
{
    QTest::addColumn<qreal>("fontScale");
    QTest::newRow("normal-font") << qreal(1.0);
    QTest::newRow("large-font") << qreal(1.5);
}

void TestUpdater::progressFitsVersion()
{
    QFETCH(qreal, fontScale);
    QFont font = previousFont_;
    font.setPointSizeF(font.pointSizeF() * fontScale);
    QApplication::setFont(font);
    const QPalette palette = theme::darkPalette(QColor(QStringLiteral("#4f8cff")));
    qApp->setStyleSheet(Theme::buildStyleSheet(palette, QStringLiteral("#4f8cff")));

    Updater updater;
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    DownloadReply *reply = prepareDownload(updater, true);
    QVERIFY(reply);
    QProgressDialog *progress = updater.progress_;
    QVERIFY(progress);
    QTRY_VERIFY(progress->isVisible());
    auto *label = progress->findChild<QLabel *>();
    QVERIFY(label);
    QVERIFY(label->text().contains(kVersion));
    QVERIFY(progress->width() >= 420);
    QVERIFY(label->width() >= label->sizeHint().width());
    QVERIFY2(label->contentsRect().width() >= label->fontMetrics().horizontalAdvance(label->text()),
             "The complete download/version label must fit without clipping.");
    progress->close();
    reply->finish();
}

} // namespace mervin

int main(int argc, char **argv)
{
    if (argc == 6 && QByteArray(argv[1]) == "--installer-fixture") {
        QCoreApplication app(argc, argv);
        const QStringList args = app.arguments();
        QFile staged(args.at(5));
        QFile result(args.at(2));
        if (!staged.open(QIODevice::ReadOnly) || !result.open(QIODevice::WriteOnly))
            return 2;
        const QByteArray bytes = staged.readAll();
        if (result.write(bytes) != bytes.size() || !result.flush())
            return 3;
        result.close();
        for (int i = 0; i < 500 && !QFileInfo::exists(args.at(3)); ++i)
            QThread::msleep(10);
        if (!QFileInfo::exists(args.at(3)))
            return 4;
        const int code = args.at(4).toInt();
        if (code != 0 && code != 126)
            std::fputs("Fixture installer failure\n", stderr);
        return code;
    }
    QApplication app(argc, argv);
    mervin::TestUpdater test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_updater.moc"
