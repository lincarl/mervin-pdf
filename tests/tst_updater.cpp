#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "i18n/UiLanguage.h"
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
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstring>

namespace {

const QString kVersion = QStringLiteral("9999.123.456");
const QByteArray kDownload = QByteArrayLiteral("a locally generated update fixture");
const QString kAssetName = QStringLiteral("MervinPDF-9999.123.456.msi");
QStringList installerFiles;
std::function<void()> beforeInstallerReturns;
bool installerLaunchSucceeds = true;
mervin::update::PackageKind installedKind = mervin::update::PackageKind::Msi;

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
    bool hasUpdate = false;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &, QIODevice *) override
    {
        ++requests;
        QByteArray body = QByteArrayLiteral(R"({"tag_name":"v0.0.0"})");
        if (hasUpdate && requests == 1) {
            body = QByteArrayLiteral(R"({"tag_name":"v9999.123.456","assets":[{)"
                R"("name":"MervinPDF-9999.123.456.msi",)"
                R"("browser_download_url":"https://example.invalid/update.msi",)"
                R"("digest":"sha256:)")
                + QCryptographicHash::hash(kDownload, QCryptographicHash::Sha256).toHex()
                + QByteArrayLiteral(R"("}]})");
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
    if (beforeInstallerReturns)
        beforeInstallerReturns();
    return installerLaunchSucceeds;
}
bool replaceAppImage(const QString &, QString *) { return false; }
QStringList packageInstallCommand(PackageKind, const QString &) { return {}; }
QStringList relaunchArguments() { return {}; }
bool relaunchAfterExit(const QString &) { return false; }

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
    void automaticCompletionWaitsForSettings();
    void queuedAutomaticInstallCanBecomeManual();
    void startupRespectsSetting_data();
    void startupRespectsSetting();
    void msixDelegatesToStore_data();
    void msixDelegatesToStore();
    void recordStoreUrl(const QUrl &url);
    void releaseInstallsWithoutConfirmation_data();
    void releaseInstallsWithoutConfirmation();
    void disablingCancelsQueuedInstall();
    void disablingDuringDownload_data();
    void disablingDuringDownload();
    void invalidDownloadDoesNotInstall_data();
    void invalidDownloadDoesNotInstall();
    void closeVetoRetainsPendingUpdate();
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
    QString downloadedPath() const;
    bool savePending() const;
    ReleaseNetworkAccessManager *useLocalNetwork(Updater &updater);

    QTemporaryDir profile_;
    QString previousProfile_;
    QFont previousFont_;
    QString previousStyle_;
    int quitRequests_ = 0;
    QList<QUrl> storeUrls_;
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
    installedKind = update::PackageKind::Msi;
    storeUrls_.clear();
    QSettings().clear();
    installerFiles.clear();
    quitRequests_ = 0;
    beforeInstallerReturns = {};
    installerLaunchSucceeds = true;
    QVERIFY(setAutoUpdate(true));
    const QDir updates(ConfigPaths::updatesDir());
    for (const QString &file : updates.entryList(QDir::Files))
        QVERIFY(QFile::remove(updates.filePath(file)));
}

void TestUpdater::cleanup()
{
    QDesktopServices::unsetUrlHandler(QStringLiteral("ms-windows-store"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qApp->setStyleSheet(previousStyle_);
    QApplication::setFont(previousFont_);
}

void TestUpdater::cleanupTestCase()
{
    ConfigPaths::setOverrideDir(previousProfile_);
}

void TestUpdater::completedCheckUpdatesSchedule_data()
{
    QTest::addColumn<bool>("manual");
    QTest::addColumn<bool>("fails");
    QTest::newRow("automatic-success") << false << false;
    QTest::newRow("manual-success") << true << false;
    QTest::newRow("automatic-network-error") << false << true;
    QTest::newRow("manual-network-error") << true << true;
}

void TestUpdater::completedCheckUpdatesSchedule()
{
    QFETCH(bool, manual);
    QFETCH(bool, fails);
    const QString key = QStringLiteral("update/lastCheckUtc");
    const QDateTime previous = QDateTime::currentDateTimeUtc().addDays(-1);
    QSettings().setValue(key, previous);
    QSettings().setValue(QStringLiteral("update/startsSinceCheck"), 19);
    Updater updater;
    delete updater.nam_;
    auto *network = new ReleaseNetworkAccessManager(&updater);
    updater.nam_ = network;
    if (fails)
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
    QCOMPARE(network->requests, 1);
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
}

QString TestUpdater::downloadedPath() const
{
    return QDir(ConfigPaths::updatesDir()).filePath(kAssetName);
}

DownloadReply *TestUpdater::prepareDownload(Updater &updater, bool manual,
                                           QNetworkReply::NetworkError error)
{
    updater.setCloseWindowsHandler([] { return true; });
    updater.busy_ = true;
    updater.manual_ = manual;
    updater.downloadVersion_ = kVersion;
    updater.asset_.name = kAssetName;
    updater.asset_.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(kDownload, QCryptographicHash::Sha256).toHex());
    updater.part_ = new QFile(downloadedPath() + QStringLiteral(".part"), &updater);
    if (!updater.part_->open(QIODevice::WriteOnly))
        return nullptr;
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
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingVersion")).toString(), kVersion);
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingFile")).toString(), downloadedPath());
    QFile download(downloadedPath());
    QVERIFY(download.open(QIODevice::ReadOnly));
    QCOMPARE(download.readAll(), kDownload);
    QTRY_COMPARE_WITH_TIMEOUT(installerFiles.size(), 1, 2000);
    QCOMPARE(installerFiles.first(), downloadedPath());
    QCOMPARE(observer.confirmations, 0);
    QVERIFY(observer.messages.isEmpty());
    QVERIFY(settingsDialog.isVisible());
    QVERIFY(!updater.installQueued_);
    QVERIFY(updater.busy_);
    QVERIFY(updater.installing_);
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
    updater.setCloseWindowsHandler([] { return true; });
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QVERIFY(savePending());
    PromptObserver observer;

    updater.onStartup();
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

bool TestUpdater::savePending() const
{
    QFile download(downloadedPath());
    if (!download.open(QIODevice::WriteOnly) || download.write(kDownload) != kDownload.size())
        return false;
    download.close();
    QSettings().setValue(QStringLiteral("update/pendingVersion"), kVersion);
    QSettings().setValue(QStringLiteral("update/pendingFile"), downloadedPath());
    QSettings().setValue(QStringLiteral("update/lastCheckUtc"), QDateTime::currentDateTimeUtc());
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

void TestUpdater::msixDelegatesToStore_data()
{
    QTest::addColumn<bool>("pending");
    QTest::newRow("fresh Store installation") << false;
    QTest::newRow("coexisting MSI has a pending update") << true;
}

void TestUpdater::msixDelegatesToStore()
{
    QFETCH(bool, pending);
    installedKind = update::PackageKind::Msix;
    Updater updater;
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    QDesktopServices::setUrlHandler(QStringLiteral("ms-windows-store"), this, "recordStoreUrl");
    if (pending)
        QVERIFY(savePending());
    const QDateTime previousCheck = QDateTime::currentDateTimeUtc().addDays(-31);
    QSettings().setValue(QStringLiteral("update/lastCheckUtc"), previousCheck);
    const QString previousPending = QSettings().value(QStringLiteral("update/pendingFile")).toString();
    QSignalSpy checked(&updater, &Updater::checked);

    QVERIFY(!updater.canSelfUpdate());
    updater.onStartup();
    updater.checkNow();
    updater.onAutoUpdateChanged(false);
    updater.onAutoUpdateChanged(true);
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
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingFile")).toString(), previousPending);
    if (pending) {
        QCOMPARE(QSettings().value(QStringLiteral("update/pendingVersion")).toString(), kVersion);
        QVERIFY(QFile::exists(downloadedPath()));
    }
}

void TestUpdater::startupRespectsSetting_data()
{
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("pending");
    QTest::newRow("disabled-no-pending") << false << false;
    QTest::newRow("disabled-pending") << false << true;
    QTest::newRow("enabled-no-pending") << true << false;
    QTest::newRow("enabled-pending") << true << true;
}

void TestUpdater::startupRespectsSetting()
{
    QFETCH(bool, enabled);
    QFETCH(bool, pending);
    QVERIFY(setAutoUpdate(enabled));
    if (pending)
        QVERIFY(savePending());
    Updater updater;
    updater.setCloseWindowsHandler([] { return true; });
    auto *network = useLocalNetwork(updater);
    PromptObserver observer;
    updater.onStartup();
    if (enabled && pending)
        QTRY_COMPARE(installerFiles.size(), 1);
    else if (enabled)
        QTRY_VERIFY(!updater.busy_);
    QCoreApplication::processEvents();
    QCOMPARE(network->requests, enabled && !pending ? 1 : 0);
    QCOMPARE(installerFiles.size(), enabled && pending ? 1 : 0);
    QCOMPARE(observer.confirmations, 0);
    QCOMPARE(QFile::exists(downloadedPath()), pending);
    QCOMPARE(updater.busy_, enabled && pending);
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
        QSettings().setValue(QStringLiteral("update/lastCheckUtc"),
                             QDateTime::currentDateTimeUtc().addDays(-31));
    }
    {
        Updater updater;
        updater.setCloseWindowsHandler([] { return true; });
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
        QCOMPARE(installerFiles.first(), downloadedPath());
        QVERIFY(observer.messages.isEmpty());
        QVERIFY(!update::automaticCheckDue(Updater::lastCheck(), QDateTime::currentDateTimeUtc()));
        QFile download(downloadedPath());
        QVERIFY(download.open(QIODevice::ReadOnly));
        QCOMPARE(download.readAll(), kDownload);
        QVERIFY(updater.busy_); // prevent another check until the handed-off app exits
        QVERIFY(!QSettings().contains(QStringLiteral("update/pendingFile")));
        QVERIFY(!QSettings().contains(QStringLiteral("update/pendingVersion")));
    }
    // A failed/cancelled MSI can relaunch the old binary. It must not run the
    // same installer again immediately, including a download resumed at startup.
    Updater resumed;
    auto *network = useLocalNetwork(resumed);
    resumed.onStartup();
    QCoreApplication::processEvents();
    QCOMPARE(network->requests, 0);
    QCOMPARE(installerFiles.size(), 1);
    QVERIFY(!resumed.busy_);
    QCOMPARE(QFile::exists(downloadedPath()), manual);
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
    QCoreApplication::processEvents();
    QVERIFY(updater.installQueued_);
    QVERIFY(setAutoUpdate(false));
    updater.onAutoUpdateChanged(false);
    settingsDialog.close();
    updater.pollInstall(); // also exercise a timer already posted before disable
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installQueued_);
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QSettings().contains(QStringLiteral("update/pendingFile")));
    QVERIFY(installerFiles.isEmpty());
    QCOMPARE(network->requests, 0);
    QVERIFY(observer.messages.isEmpty());
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
    QCOMPARE(installerFiles.size(), manual ? 1 : 0);
    QCOMPARE(QFile::exists(downloadedPath()), manual);
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QCOMPARE(observer.confirmations, 0);
}

void TestUpdater::invalidDownloadDoesNotInstall_data()
{
    QTest::addColumn<bool>("badHash");
    QTest::newRow("hash-mismatch") << true;
    QTest::newRow("network-failure") << false;
}

void TestUpdater::invalidDownloadDoesNotInstall()
{
    QFETCH(bool, badHash);
    Updater updater;
    DownloadReply *reply = prepareDownload(updater, false,
        badHash ? QNetworkReply::NoError : QNetworkReply::ConnectionRefusedError);
    QVERIFY(reply);
    if (badHash)
        updater.asset_.sha256 = QString(64, QLatin1Char('0'));
    PromptObserver observer;
    reply->finish();
    QCoreApplication::processEvents();
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installQueued_);
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QVERIFY(!QSettings().contains(QStringLiteral("update/pendingFile")));
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
        return false; // the user cancels an unsaved-document prompt
    });
    PromptObserver observer;
    reply->finish();
    QTRY_COMPARE(closeAttempts, 1);
    QVERIFY(installerFiles.isEmpty());
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installing_);
    QVERIFY(QFile::exists(downloadedPath()));
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingVersion")).toString(), kVersion);
    QVERIFY(observer.messages.isEmpty());

    // An explicit retry can use the same download without another transfer.
    auto *network = useLocalNetwork(updater);
    network->hasUpdate = true;
    updater.setCloseWindowsHandler([] { return true; });
    updater.checkNow();
    QTRY_COMPARE(installerFiles.size(), 1);
    QCOMPARE(network->requests, 1);
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
    QVERIFY(!updater.busy_);
    QVERIFY(!updater.installing_);
    QVERIFY(QFile::exists(downloadedPath()));
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingVersion")).toString(), kVersion);
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
    QVERIFY(!QFile::exists(downloadedPath()));
    QVERIFY(!QFile::exists(downloadedPath() + QStringLiteral(".part")));
    QVERIFY(!QSettings().contains(QStringLiteral("update/pendingFile")));
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

QTEST_MAIN(mervin::TestUpdater)
#include "tst_updater.moc"
