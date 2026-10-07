#include "config/ConfigPaths.h"
#include "i18n/UiLanguage.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"
#include "update/Updater.h"

#include <QAbstractButton>
#include <QApplication>
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

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &, QIODevice *) override
    {
        ++requests;
        auto *reply = new DownloadReply(this, QByteArrayLiteral(R"({"tag_name":"v0.0.0"})"), error);
        QTimer::singleShot(0, reply, &DownloadReply::finish);
        return reply;
    }
};

// QMessageBox::exec starts a nested event loop. Always dismiss its question
// with Later (reject), including unexpected error boxes, so failures cannot
// hang the suite or start an installer.
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
                if (box->windowTitle() == QStringLiteral("Update Ready")) {
                    ++count;
                    parent = box->parentWidget();
                    text = box->text();
                } else {
                    unexpected = box->text();
                }
                box->reject();
            }
        });
        timer_.start(1);
    }

    int count = 0;
    QPointer<QWidget> parent;
    QString text;
    QString unexpected;

private:
    QTimer timer_;
};

} // namespace

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
    void manualCompletionShowsPromptOverSettings_data();
    void manualCompletionShowsPromptOverSettings();
    void automaticCompletionWaitsForSettings();
    void queuedAutomaticPromptCanBecomeManual();
    void cancelStillAborts_data();
    void cancelStillAborts();
    void progressFitsVersion_data();
    void progressFitsVersion();

private:
    DownloadReply *prepareDownload(Updater &updater, bool manual);
    QString downloadedPath() const;

    QTemporaryDir profile_;
    QString previousProfile_;
    QFont previousFont_;
    QString previousStyle_;
};

void TestUpdater::initTestCase()
{
    // PromptObserver finds the update prompt by its English title.
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
}

void TestUpdater::init()
{
    QSettings().clear();
    const QDir updates(ConfigPaths::updatesDir());
    for (const QString &file : updates.entryList(QDir::Files))
        QVERIFY(QFile::remove(updates.filePath(file)));
}

void TestUpdater::cleanup()
{
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
    return QDir(ConfigPaths::updatesDir()).filePath(QStringLiteral("test-update.bin"));
}

DownloadReply *TestUpdater::prepareDownload(Updater &updater, bool manual)
{
    updater.busy_ = true;
    updater.manual_ = manual;
    updater.downloadVersion_ = kVersion;
    updater.asset_.name = QStringLiteral("test-update.bin");
    updater.asset_.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(kDownload, QCryptographicHash::Sha256).toHex());
    updater.part_ = new QFile(downloadedPath() + QStringLiteral(".part"), &updater);
    if (!updater.part_->open(QIODevice::WriteOnly))
        return nullptr;
    auto *reply = new DownloadReply(&updater);
    updater.reply_ = reply;
    QObject::connect(reply, &QNetworkReply::finished, &updater, &Updater::onDownloadFinished);
    if (manual)
        updater.showProgress();
    return reply;
}

void TestUpdater::manualCompletionShowsPromptOverSettings_data()
{
    QTest::addColumn<bool>("startedManually");
    QTest::newRow("manual-download") << true;
    QTest::newRow("background-download-promoted-by-check-now") << false;
}

void TestUpdater::manualCompletionShowsPromptOverSettings()
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
    // verified update. The Settings dialog stays open during the install question.
    QCOMPARE(canceled.count(), 0);
    QCOMPARE(reply->abortCount, 0);
    QVERIFY(!progress->isVisible());
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingVersion")).toString(), kVersion);
    QCOMPARE(QSettings().value(QStringLiteral("update/pendingFile")).toString(), downloadedPath());
    QFile download(downloadedPath());
    QVERIFY(download.open(QIODevice::ReadOnly));
    QCOMPARE(download.readAll(), kDownload);
    QTRY_COMPARE_WITH_TIMEOUT(observer.count, 1, 2000);
    QVERIFY2(observer.unexpected.isEmpty(), qPrintable(observer.unexpected));
    QCOMPARE(observer.parent.data(), &settingsDialog);
    QVERIFY(observer.text.contains(kVersion));
    QVERIFY(settingsDialog.isVisible());
    QVERIFY(!updater.askQueued_);
    QVERIFY(!updater.busy_);
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
    QCOMPARE(observer.count, 0);
    QVERIFY(updater.askQueued_);
    QVERIFY(QFile::exists(downloadedPath()));
    settingsDialog.close();
    QTRY_COMPARE_WITH_TIMEOUT(observer.count, 1, 2000);
    QVERIFY2(observer.unexpected.isEmpty(), qPrintable(observer.unexpected));
    QVERIFY(!updater.askQueued_);
}

void TestUpdater::queuedAutomaticPromptCanBecomeManual()
{
    Updater updater;
    QDialog settingsDialog;
    settingsDialog.setAttribute(Qt::WA_DontShowOnScreen);
    settingsDialog.setModal(true);
    settingsDialog.show();
    QFile download(downloadedPath());
    QVERIFY(download.open(QIODevice::WriteOnly));
    QCOMPARE(download.write(kDownload), kDownload.size());
    download.close();
    QSettings().setValue(QStringLiteral("update/pendingVersion"), kVersion);
    QSettings().setValue(QStringLiteral("update/pendingFile"), downloadedPath());
    PromptObserver observer;

    updater.askWhenIdle();
    updater.askWhenIdle(true);
    updater.askWhenIdle(); // another background request cannot undo promotion
    QTRY_COMPARE_WITH_TIMEOUT(observer.count, 1, 2000);
    QVERIFY2(observer.unexpected.isEmpty(), qPrintable(observer.unexpected));
    QCOMPARE(observer.parent.data(), &settingsDialog);
    QVERIFY(settingsDialog.isVisible());
    QVERIFY(!updater.askQueued_);
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
    QVERIFY(!updater.askQueued_);
    QVERIFY(!updater.busy_);
    QCOMPARE(observer.count, 0);
    QVERIFY2(observer.unexpected.isEmpty(), qPrintable(observer.unexpected));
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
