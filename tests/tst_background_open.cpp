#include "TestWaits.h"
#include "Stalls.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "recent/RecentStore.h"
#include "recent/ViewStateStore.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/QpdfService.h"
#include "session/SessionStore.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

using namespace mervin;

namespace {
TabPage *current(MainWindow &window)
{
    return qobject_cast<TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
}

void showWindow(MainWindow &window)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1000, 800);
    window.show();
    QCoreApplication::processEvents();
}

void holdRead(const QString &path)
{
    job::testing::holdWhen([path](job::Step step, const QString &reported) {
        return step == job::Step::Reading && reported == path;
    });
}

QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

// Checks placeholder opens, queued load results and their ownership across windows.
class TstBackgroundOpen : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.closeToTray = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
    }

    void cleanup()
    {
        job::testing::release();
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    void heldOpenReturnsAndFocusesItsPlaceholder()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        holdRead(path);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(window.openFile(path));
        QVERIFY(elapsed.elapsed() < 1000);
        auto *tab = current(window);
        QVERIFY(tab && tab->freshOpen() && tab->isLoading());
        QCOMPARE(tab->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(), QStringLiteral("Loading"));
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(job::testing::stepsOnMainThread(), 0);
        QVERIFY(window.openFile(files.filePath(QStringLiteral("./plan.pdf"))));
        QCOMPARE(window.tabCount(), 1);
        QCOMPARE(current(window), tab);
        TabPage::setSlowDelayForTesting(0);
        QTRY_VERIFY(tab->findChild<QPushButton *>(QStringLiteral("stopDocumentLoad"))->isVisible());
        job::testing::release();
        QTRY_VERIFY(tab->isLoaded());
        QVERIFY(!tab->freshOpen());
    }

    void lazyRetryWaitsForTheOutputReservation()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("output.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        tab.initializeSuspended(path);
        job::State writing;
        const job::Scope scope(writing);
        QVERIFY(job::reserveDestinations({normalizePathKey(path)}));
        std::atomic<int> reads{0};
        job::testing::failWhen([&](job::Step step, const QString &file) {
            if (step == job::Step::Reading && file == path)
                ++reads;
            return false;
        });
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        tab.resumeAsync();
        QVERIFY(!tab.isLoading());
        QCOMPARE(tab.findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                 TabPage::pathBeingWrittenError());
        QSignalSpy retryFinished(&tab, &TabPage::resumeFinished);
        tab.retryLoad();
        QTRY_COMPARE(retryFinished.size(), 1);
        QVERIFY(!retryFinished.first().first().toBool());
        QVERIFY(!tab.isLoading() && !tab.isLoaded());
        QCOMPARE(reads.load(), 0);
        QCOMPARE(tab.findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                 TabPage::pathBeingWrittenError());
        writing.stop();
        tab.retryLoad();
        QTRY_VERIFY(tab.isLoaded());
        QVERIFY(reads.load() > 0);
    }

    void storedFreshLoadDoesNotReportAnUnresponsiveDrive()
    {
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QDialog guard(window);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        TabPage::setSlowDelayForTesting(0);
        holdRead(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(window->openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *page = current(*window);
        auto *stop = page->findChild<QPushButton *>(QStringLiteral("stopDocumentLoad"));
        QSignalSpy ready(page, &TabPage::freshLoadReady);
        QTRY_COMPARE(job::testing::held(), 1);
        QTRY_VERIFY(stop->isVisible());
        job::testing::release();
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(TabPage::testing::storedDocument(*page));
        QTest::qWait(750);
        QVERIFY(page->isLoading() && !page->isLoaded());
        QCOMPARE(page->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                 QStringLiteral("Loading"));
        QVERIFY(stop->isHidden());
        guard.close();
        QTRY_VERIFY(page->isLoaded());
    }

#ifdef Q_OS_UNIX
    void aliasAdmissionSurvivesQueuedAndInstalledResults()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("output.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QVERIFY(QFile::link(path, alias));
        job::State writing;
        const job::Scope scope(writing);
        {
            WindowManager manager;
            MainWindow *window = manager.createWindow();
            showWindow(*window);
            QDialog guard(window);
            guard.setWindowModality(Qt::WindowModal);
            guard.show();
            QVERIFY(window->openFile(alias));
            auto *page = current(*window);
            QTRY_VERIFY(TabPage::testing::storedDocument(*page));
            QVERIFY(!job::reserveDestinations({normalizePathKey(path)}));
            guard.close();
            QTRY_VERIFY(page->isLoaded());
            QCOMPARE(page->canonicalPath(), path);
            QVERIFY(!job::reserveDestinations({normalizePathKey(path)}));
        }
        QVERIFY(job::reserveDestinations({normalizePathKey(path)}));
    }

    void fifoOpenReturnsBeforeTheDriveAnswers()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("held.pdf"));
        QVERIFY(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
        testing::PipeWriter writer(path, 5000);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(window.openFile(path));
        QVERIFY(elapsed.elapsed() < 1000);
        auto *tab = current(window);
        QVERIFY(tab && tab->isLoading());
        QTest::qWait(100);
        QVERIFY(tab->isLoading());
        tab->stopWaiting();
        QTRY_COMPARE(window.tabCount(), 0);
        writer.release();
        QTest::qWait(100);
    }

    void linkedFolderDeduplicatesBeforeInstallation()
    {
        QTemporaryDir files;
        QVERIFY(QDir(files.path()).mkdir(QStringLiteral("real")));
        const QString path = files.filePath(QStringLiteral("real/plan.pdf"));
        const QString link = files.filePath(QStringLiteral("alias"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QVERIFY(QFile::link(files.filePath(QStringLiteral("real")), link));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        auto *original = current(*window);
        const QString alias = QDir(link).filePath(QStringLiteral("plan.pdf"));
        QVERIFY(window->openFile(alias));
        auto *newcomer = current(*window);
        QVERIFY(newcomer != original);
        QSignalSpy installed(newcomer, &TabPage::resumeFinished);
        QTRY_COMPARE(window->tabCount(), 1);
        QCOMPARE(current(*window), original);
        QCOMPARE(installed.size(), 0);
    }

    void simultaneousAliasesKeepOneDocument()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString firstAlias = files.filePath(QStringLiteral("first.pdf"));
        const QString secondAlias = files.filePath(QStringLiteral("second.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QVERIFY(QFile::link(path, firstAlias));
        QVERIFY(QFile::link(path, secondAlias));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QDialog guard(window);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(window->openFile(firstAlias));
        QPointer<TabPage> first = current(*window);
        QVERIFY(window->openFile(secondAlias));
        QPointer<TabPage> second = current(*window);
        QVERIFY(first != second);
        QTRY_VERIFY(TabPage::testing::storedDocument(*first));
        QTRY_VERIFY(TabPage::testing::storedDocument(*second));
        guard.close();
        QTRY_COMPARE(window->tabCount(), 1);
        QTRY_VERIFY(current(*window)->isLoaded());
        QCOMPARE(window->tabPaths(), QStringList{path});
        QVERIFY(bool(first) != bool(second));
    }

    void independentRecoveriesAreNotDiscardedByCanonicalDeduplication()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("original.pdf"));
        const QString firstAlias = files.filePath(QStringLiteral("first.pdf"));
        const QString secondAlias = files.filePath(QStringLiteral("second.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(QFile::link(source, firstAlias));
        QVERIFY(QFile::link(source, secondAlias));
        struct Recovery {
            QString path;
            QByteArray snapshot;
            QByteArray manifest;
        };
        QList<Recovery> recoveries;
        RenderEngine engine;
        for (const QString &alias : {firstAlias, secondAlias}) {
            TabPage edited(&engine);
            QVERIFY(edited.open(alias));
            QVERIFY(edited.viewer()->annotModel()->addTextNote(
                0, {40, 40}, Qt::yellow, {}, QFileInfo(alias).fileName()) >= 0);
            QVERIFY(edited.suspend());
            recoveries.append({edited.recoveryPath(), bytes(edited.recoveryPath()),
                               bytes(edited.recoveryPath() + QStringLiteral(".json"))});
        }
        QVERIFY(appfiles::flush(10000));
        for (const auto &recovery : recoveries) {
            QVERIFY(write(recovery.path, recovery.snapshot));
            QVERIFY(write(recovery.path + QStringLiteral(".json"), recovery.manifest));
        }
        QCOMPARE(TabPage::recoverablePaths().count(source), 2);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, firstAlias));
        QPointer<TabPage> first = current(*window);
        QVERIFY(testing::openAndWait(*window, secondAlias));
        auto *second = current(*window);
        QVERIFY(first && first != second);
        QCOMPARE(window->tabCount(), 2);
        for (auto *page : {first.data(), second}) {
            QCOMPARE(page->canonicalPath(), source);
            QVERIFY(page->hasUnsavedEdits());
            QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        }
        QCOMPARE(first->viewer()->annotModel()->pageAnnots(0).front().contents, QStringLiteral("first.pdf"));
        QCOMPARE(second->viewer()->annotModel()->pageAnnots(0).front().contents, QStringLiteral("second.pdf"));
    }

    void aLateAliasDoesNotStealTheSelectedTab()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString other = files.filePath(QStringLiteral("other.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        for (const QString &file : {path, other})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), file));
        QVERIFY(QFile::link(path, alias));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        QVERIFY(testing::openAndWait(*window, other));
        auto *selected = current(*window);
        holdRead(alias);
        QVERIFY(window->openFile(alias));
        QTRY_COMPARE(job::testing::held(), 1);
        window->findChild<QTabWidget *>()->setCurrentIndex(1);
        QCOMPARE(current(*window), selected);
        job::testing::release();
        QTRY_COMPARE(window->tabCount(), 2);
        QCOMPARE(current(*window), selected);
    }
#endif

#ifdef Q_OS_WIN
    void caseDifferencesFocusThePendingTab()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("Plan.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        holdRead(path);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        QVERIFY(window.openFile(path));
        auto *tab = current(window);
        QVERIFY(window.openFile(path.toUpper()));
        QCOMPARE(window.tabCount(), 1);
        QCOMPARE(current(window), tab);
        tab->stopWaiting();
        QTRY_COMPARE(window.tabCount(), 0);
    }
#endif

    void stopLeavesNoNewHistoryOrLateDocument()
    {
        QTemporaryDir files;
        const QString kept = files.filePath(QStringLiteral("kept.pdf"));
        const QString pending = files.filePath(QStringLiteral("pending.pdf"));
        for (const QString &file : {kept, pending})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), file));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, kept));
        QTRY_VERIFY(!manager.fileProbe()->isChecking({kept, {}}));
        manager.saveViewState(kept, current(*window)->capturedViewState());
        QVERIFY(appfiles::flush(10000));
        const QString recent = RecentStore::defaultFile();
        const QString view = ViewStateStore::defaultFile();
        const QByteArray recentBefore = bytes(recent);
        const QByteArray viewBefore = bytes(view);
        QVERIFY(!recentBefore.isEmpty());
        QVERIFY(!viewBefore.isEmpty());
        const int runningBefore = background::running();
        holdRead(pending);
        QVERIFY(window->openFile(pending));
        QPointer<TabPage> tab = current(*window);
        QVERIFY(window->openFile(pending)); // focusing a placeholder must not record an open
        QTRY_COMPARE(job::testing::held(), 1);
        tab->stopWaiting();
        QTRY_VERIFY(!tab);
        QCOMPARE(window->tabPaths(), QStringList{kept});
        QVERIFY(appfiles::flush(10000));
        QCOMPARE(bytes(recent), recentBefore);
        QCOMPARE(bytes(view), viewBefore);
        SessionStore session;
        QVERIFY(session.load());
        QCOMPARE(session.paths(), QStringList{kept});
        std::optional<WindowManager::Reopen> reopened;
        manager.reopenClosedTab(window, [&](WindowManager::Reopen result) { reopened = result; });
        QTRY_VERIFY(reopened.has_value());
        QCOMPARE(*reopened, WindowManager::Reopen::NothingToReopen);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= runningBefore, 10000);
        QVERIFY(!window->hasLoadingDocuments());
        QCOMPARE(window->tabPaths(), QStringList{kept});
    }

    void stopDiscardsAStoredFreshResultThroughTheQueue()
    {
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QDialog guard(&window);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(window.openFile(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QPointer<TabPage> page = current(window);
        QTRY_VERIFY(TabPage::testing::storedDocument(*page));
        const auto lifetime = TabPage::testing::storedDocument(*page);
        QSignalSpy installed(page, &TabPage::resumeFinished);
        page->stopWaiting();
        QCOMPARE(window.tabCount(), 1); // the dialog still owns its tab positions
        guard.close();
        QTRY_VERIFY(!page);
        QCOMPARE(window.tabCount(), 0);
        QCOMPARE(installed.size(), 0);
        QTRY_VERIFY([&] {
            std::lock_guard lock(lifetime->mutex);
            return !lifetime->document;
        }());
    }

    void recoveredFreshStopKeepsItsOriginalAndRetry_data()
    {
        QTest::addColumn<bool>("stored");
        QTest::newRow("read-held") << false;
        QTest::newRow("already-stored") << true;
    }

    void recoveredFreshStopKeepsItsOriginalAndRetry()
    {
        QFETCH(bool, stored);
        QTemporaryDir files;
        const QString original = files.filePath(QStringLiteral("original.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), original));
        RenderEngine engine;
        QString snapshot;
        QByteArray savedBytes;
        QByteArray manifest;
        {
            TabPage edited(&engine);
            QVERIFY(edited.open(original));
            edited.viewer()->setScale(1.7);
            edited.viewer()->setRotation(90);
            QVERIFY(edited.viewer()->annotModel()->addTextNote(
                0, {40, 40}, Qt::yellow, {}, QStringLiteral("Recovered edit")) >= 0);
            QVERIFY(edited.suspend());
            snapshot = edited.recoveryPath();
            savedBytes = bytes(snapshot);
            manifest = bytes(snapshot + QStringLiteral(".json"));
        }
        QVERIFY(appfiles::flush(10000));
        QVERIFY(write(snapshot, savedBytes));
        QVERIFY(write(snapshot + QStringLiteral(".json"), manifest));
        QCOMPARE(TabPage::recoverablePaths(), QStringList{original});
        QVERIFY(QFile::remove(original));
        if (!stored)
            holdRead(snapshot);
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QDialog guard(&window);
        if (stored) {
            guard.setWindowModality(Qt::WindowModal);
            guard.show();
        }
        QVERIFY(window.openFile(original));
        auto *page = current(window);
        QVERIFY(page->hasRecoverySnapshot());
        QCOMPARE(page->recoveryPath(), snapshot);
        if (stored)
            QTRY_VERIFY(TabPage::testing::storedDocument(*page));
        else
            QTRY_COMPARE(job::testing::held(), 1);
        page->stopWaiting();
        guard.close();
        QCoreApplication::processEvents();
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(page->isSuspended());
        QVERIFY(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad"))->isVisible());
        QCOMPARE(page->path(), original);
        QCOMPARE(page->canonicalPath(), original);
        QCOMPARE(bytes(snapshot), savedBytes);
        QCOMPARE(bytes(snapshot + QStringLiteral(".json")), manifest);
        job::testing::release();
        QTest::mouseClick(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(page && page->isLoaded());
        QCOMPARE(page->path(), original);
        QCOMPARE(page->canonicalPath(), original);
        QVERIFY(page->hasUnsavedEdits());
        QCOMPARE(page->capturedViewState().scale, 1.7);
        QCOMPARE(page->capturedViewState().rotation, 90);
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).front().contents, QStringLiteral("Recovered edit"));
    }

    void missingFreshFileKeepsRetryAndCanRecover()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("missing.pdf"));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(path));
        auto *tab = current(*window);
        QTRY_VERIFY(!tab->isLoading());
        QVERIFY(tab->freshOpen());
        QCOMPARE(tab->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(), QStringLiteral("The file was not found."));
        manager.closeRemovedTabs();
        QVERIFY(window->closeRemovedTabs(path));
        QCoreApplication::processEvents();
        QCOMPARE(window->tabCount(), 1);
        const quint64 failedGeneration = tab->loadGeneration();
        tab->resumeAsync(); // automatic selection and adoption use this route
        QVERIFY(!tab->isLoading());
        QCOMPARE(tab->loadGeneration(), failedGeneration);
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QTest::mouseClick(tab->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(tab->isLoaded());
        QCOMPARE(current(*window), tab);
        QVERIFY(!tab->freshOpen());
    }

    void damagedFreshFileOffersRetry()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("damaged.pdf"));
        QVERIFY(write(path, QByteArrayLiteral("%PDF-1.7\nnot a valid document\n")));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(window.openFile(path));
        auto *page = current(window);
        QTRY_VERIFY(!page->isLoading());
        QCOMPARE(page->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(), QStringLiteral("The file is damaged."));
        QVERIFY(page->freshOpen());
        QVERIFY(write(path, bytes(QStringLiteral(MERVIN_FIXTURE_PDF))));
        QTest::mouseClick(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(page->isLoaded());
    }

    void reservedFreshOpenFollowsItsFailurePolicy_data()
    {
        QTest::addColumn<bool>("canonicalOnly");
        QTest::addColumn<bool>("closeOnFailure");
        QTest::newRow("initial ordinary") << false << false;
        QTest::newRow("initial Duplicate") << false << true;
#ifdef Q_OS_UNIX
        QTest::newRow("resolved alias ordinary") << true << false;
        QTest::newRow("resolved alias Duplicate") << true << true;
#endif
    }

    void reservedFreshOpenFollowsItsFailurePolicy()
    {
        QFETCH(bool, canonicalOnly);
        QFETCH(bool, closeOnFailure);
        const int mainThreadSteps = job::testing::stepsOnMainThread();
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("output.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        if (canonicalOnly)
            QVERIFY(QFile::link(source, alias));
        WindowManager manager;
        QPointer<MainWindow> window = manager.createWindow();
        QDialog guard(window);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        job::State writing;
        const job::Scope scope(writing);
        if (!canonicalOnly)
            QVERIFY(job::reserveDestinations({normalizePathKey(source)}));
        else
            holdRead(alias);
        std::atomic<int> reads{0};
        job::testing::failWhen([&](job::Step step, const QString &file) {
            if (step == job::Step::Reading && file == alias)
                ++reads;
            return false;
        });
        const auto clearHook = qScopeGuard([] { job::testing::failWhen({}); });
        QVERIFY(window->openFile(canonicalOnly ? alias : source, closeOnFailure,
                                 -1, true, {}, false, std::nullopt, closeOnFailure));
        QPointer<TabPage> page = current(*window);
        QVERIFY(page);
        QSignalSpy installed(page, &TabPage::resumeFinished);
        if (canonicalOnly) {
            QTRY_COMPARE(job::testing::held(), 1);
            QVERIFY(job::reserveDestinations({normalizePathKey(source)}));
            job::testing::release();
        } else {
            QVERIFY(!page->isLoading());
            QVERIFY(!TabPage::testing::storedDocument(*page));
        }
        guard.close();
        guard.setParent(nullptr); // its owner may close after the queued failure
        if (closeOnFailure) {
            QTRY_VERIFY(!window && !page);
        } else {
            QTRY_VERIFY(page && !page->isLoading());
            QVERIFY(page->freshOpen() && !page->isLoaded());
            QCOMPARE(page->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                     TabPage::pathBeingWrittenError());
            QVERIFY(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad"))->isVisible());
        }
        for (const auto &attempt : installed)
            QVERIFY(!attempt.front().toBool());
        if (canonicalOnly)
            QCOMPARE(reads.load(), 1); // only the report before canonical resolution
        writing.stop();
        if (!closeOnFailure) {
            QTest::mouseClick(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
            QTRY_VERIFY(page && page->isLoaded());
            QCOMPARE(page->canonicalPath(), source);
        }
        QCOMPARE(job::testing::stepsOnMainThread(), mainThreadSteps);
    }

#ifdef Q_OS_UNIX
    void encryptedAliasChecksReservationBeforeAskingForItsPassword()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted-output.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                                 QStringLiteral("correct"), QStringLiteral("owner"),
                                 QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        QVERIFY(QFile::link(source, alias));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QDialog guard(window);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        holdRead(alias);
        QVERIFY(window->openFile(alias));
        QPointer<TabPage> page = current(*window);
        QSignalSpy ready(page, &TabPage::freshLoadReady);
        QTRY_COMPARE(job::testing::held(), 1);
        job::State writing;
        const job::Scope scope(writing);
        QVERIFY(job::reserveDestinations({normalizePathKey(source)}));
        job::testing::release();
        QTRY_COMPARE(ready.size(), 1);
        int unexpectedPrompts = 0;
        QTimer rejectUnexpected;
        rejectUnexpected.setInterval(5);
        connect(&rejectUnexpected, &QTimer::timeout, this, [&] {
            if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                ++unexpectedPrompts;
                prompt->reject();
            }
        });
        rejectUnexpected.start();
        guard.close();
        QTRY_VERIFY(page && !page->isLoading());
        QTest::qWait(100);
        QCOMPARE(unexpectedPrompts, 0);
        QCOMPARE(page->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(),
                 TabPage::pathBeingWrittenError());
        QVERIFY(page->freshOpen() && !page->isLoaded());
        writing.stop();
        page->setPassword(QStringLiteral("correct"));
        QTest::mouseClick(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(page && page->isLoaded());
        QCOMPARE(page->canonicalPath(), source);
    }
#endif

    void passwordPromptHandlesAcceptanceRejectionAndCancel_data()
    {
        QTest::addColumn<bool>("wrongFirst");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("accepted") << false << false;
        QTest::newRow("wrong then accepted") << true << false;
        QTest::newRow("canceled") << false << true;
    }

    void passwordPromptHandlesAcceptanceRejectionAndCancel()
    {
        QFETCH(bool, wrongFirst);
        QFETCH(bool, cancel);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("encrypted.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                 QStringLiteral("correct"), QStringLiteral("owner"),
                                 QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QList<std::optional<QString>> answers;
        if (wrongFirst)
            answers.append(QStringLiteral("wrong"));
        answers.append(cancel ? std::optional<QString>{} : std::optional<QString>{QStringLiteral("correct")});
        QStringList reasons;
        QStringList labels;
        QList<Qt::WindowModality> modalities;
        QTimer respond;
        respond.setInterval(5);
        connect(&respond, &QTimer::timeout, this, [&] {
            auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!prompt)
                return;
            modalities.append(prompt->windowModality());
            labels.append(prompt->labelText());
            reasons.append(current(window)->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text());
            if (answers.isEmpty() || !answers.front()) {
                if (!answers.isEmpty())
                    answers.removeFirst();
                prompt->reject();
            } else {
                prompt->setTextValue(*answers.takeFirst());
                prompt->accept();
            }
        });
        respond.start();
        QVERIFY(window.openFile(path));
        auto *page = current(window);
        if (cancel) {
            QTRY_VERIFY(answers.isEmpty());
            QVERIFY(page->freshOpen());
            QVERIFY(page->isSuspended());
            QVERIFY(page->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad"))->isVisible());
            QCOMPARE(window.tabCount(), 1);
        } else {
            QTRY_VERIFY(page->isLoaded());
            QCOMPARE(page->password(), QStringLiteral("correct"));
            QVERIFY(answers.isEmpty());
        }
        respond.stop();
        QCOMPARE(reasons.size(), wrongFirst ? 2 : 1);
        for (const auto modality : modalities)
            QCOMPARE(modality, Qt::WindowModal);
        for (const QString &label : labels)
            QCOMPARE(label, QStringLiteral("Enter the password for encrypted.pdf."));
        if (wrongFirst)
            QCOMPARE(reasons.last(), QStringLiteral("The password was not accepted."));
    }

    // Supplies password acceptance, cancellation and direct owner destruction during Quit.
    void quittingWithAnActivePasswordPrompt_data()
    {
        QTest::addColumn<int>("disposition");
        QTest::newRow("answer password before queued Quit") << 0;
        QTest::newRow("cancel password before queued Quit") << 1;
        QTest::newRow("direct owner destruction") << 2;
    }

    // Checks that Quit waits for a password prompt and direct destruction safely releases it.
    void quittingWithAnActivePasswordPrompt()
    {
        QFETCH(int, disposition);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("encrypted.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                 QStringLiteral("correct"), QStringLiteral("owner"),
                                 QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        WindowManager manager;
        QPointer<MainWindow> owner = manager.createWindow();
        QPointer<MainWindow> other = manager.createWindow();
        bool quitRequested = false;
        bool closingBeforeApproval = false;
        QPointer<QInputDialog> activePrompt;
        QTimer quit;
        quit.setInterval(5);
        connect(&quit, &QTimer::timeout, this, [&] {
            auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!prompt || prompt->parentWidget() != owner)
                return;
            activePrompt = prompt;
            quit.stop();
            quitRequested = true;
            if (disposition == 2) {
                delete owner.data();
                delete other.data();
            } else {
                manager.closeAllForQuit();
                QVERIFY(owner && other && activePrompt);
                // A pending Quit includes new loads and windows. Only final
                // approval marks the manager as closing.
                closingBeforeApproval = manager.isClosing();
                if (disposition == 0) {
                    prompt->setTextValue(QStringLiteral("correct"));
                    prompt->accept();
                } else {
                    prompt->reject();
                }
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        });
        quit.start();
        QVERIFY(owner->openFile(path));
        QTRY_VERIFY(quitRequested);
        QVERIFY(!closingBeforeApproval);
        QTRY_VERIFY(!owner && !other && !activePrompt);
        QVERIFY(!QApplication::activeModalWidget());
    }

    void canceledDuplicatePasswordClosesItsEmptyWindow()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("encrypted.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                 QStringLiteral("correct"), QStringLiteral("owner"),
                                 QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        WindowManager manager;
        MainWindow *kept = manager.createWindow();
        QTimer reject;
        reject.setInterval(5);
        connect(&reject, &QTimer::timeout, this, [&] {
            if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                prompt->reject();
        });
        reject.start();
        manager.duplicateToNewWindow(path, {}, {}, {});
        QPointer<MainWindow> duplicate;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<MainWindow *>(widget); window && window != kept)
                duplicate = window;
        QVERIFY(duplicate);
        QTRY_VERIFY(!duplicate);
        reject.stop();
        QVERIFY(kept->isVisible());
    }

    void movedPlaceholderFinishesInItsCurrentWindow_data()
    {
        QTest::addColumn<bool>("stored");
        QTest::newRow("reading") << false;
        QTest::newRow("completion queued") << true;
    }

    void movedPlaceholderFinishesInItsCurrentWindow()
    {
        QFETCH(bool, stored);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString other = files.filePath(QStringLiteral("other.pdf"));
        for (const QString &file : {path, other})
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), file));
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(testing::openAndWait(*first, other));
        QDialog guard(first);
        if (stored) {
            guard.setWindowModality(Qt::WindowModal);
            guard.show();
        } else {
            holdRead(path);
        }
        QVERIFY(first->openFile(path));
        QPointer<TabPage> page = current(*first);
        QSignalSpy ready(page, &TabPage::freshLoadReady);
        if (stored) {
            QTRY_VERIFY(TabPage::testing::storedDocument(*page));
            QTRY_COMPARE(ready.size(), 1);
        } else {
            QTRY_COMPARE(job::testing::held(), 1);
        }
        second->adoptTab(first->releaseTab(1));
        guard.close();
        job::testing::release();
        QTRY_VERIFY(page && page->isLoaded());
        QCOMPARE(page->window(), second);
        QCOMPARE(second->currentTabPath(), path);
        QCOMPARE(first->tabPaths(), QStringList{other});
    }

    void lastTabMoveKeepsQueuedLoadActions_data()
    {
        QTest::addColumn<int>("action");
        QTest::addColumn<bool>("unmoved");
        QTest::newRow("ready") << 0 << false;
        QTest::newRow("password") << 1 << false;
        QTest::newRow("failed duplicate") << 2 << false;
        QTest::newRow("stopped fresh open") << 3 << false;
        QTest::newRow("unmoved ready") << 0 << true;
        QTest::newRow("unmoved password") << 1 << true;
        QTest::newRow("unmoved failed duplicate") << 2 << true;
        QTest::newRow("unmoved stopped fresh open") << 3 << true;
    }

    // Checks that a moved fresh tab retains queued load actions after its former owner is destroyed.
    void lastTabMoveKeepsQueuedLoadActions()
    {
        QFETCH(int, action);
        QFETCH(bool, unmoved);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("plan.pdf"));
        const QString kept = files.filePath(QStringLiteral("kept.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), kept));
        if (action == 1) {
            QpdfService service;
            QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), path, {},
                                     QStringLiteral("correct"), QStringLiteral("owner"),
                                     QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        } else if (action != 2) {
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        }
        WindowManager manager;
        QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        showWindow(*first);
        showWindow(*second);
        QVERIFY(testing::openAndWait(*second, kept));
        // Adoption must not start another read before the queued action transfers.
        // Restore the target before delivering a password prompt.
        second->setWindowState(Qt::WindowMinimized);
        // Keep each result in the original owner's queue until its last tab moves.
        // A parentless guard survives the source window's normal destruction.
        QDialog guard;
        guard.setWindowModality(Qt::ApplicationModal);
        if (action == 0 || action == 3)
            guard.show();
        if (action == 3)
            holdRead(path);
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(first->openFile(path, action == 2, -1, true, {}, false,
                                std::nullopt, action == 2));
        QPointer<TabPage> page = current(*first);
        QVERIFY(page);
        QSignalSpy ready(page, &TabPage::freshLoadReady);
        QSignalSpy finished(page, &TabPage::resumeFinished);
        if (action == 1 || action == 2)
            connect(page, &TabPage::resumeFinished, &guard, [&] { guard.show(); },
                    Qt::SingleShotConnection);
        if (action == 0) {
            QTRY_COMPARE(ready.size(), 1);
            QVERIFY(TabPage::testing::storedDocument(*page));
        } else if (action == 3) {
            QTRY_COMPARE(job::testing::held(), 1);
            page->stopWaiting();
        } else {
            QTRY_COMPARE(finished.size(), 1);
        }
        QCOMPARE(first->tabCount(), 1);
        if (unmoved) {
            bool latePrompt = false;
            QTimer rejectLate;
            connect(&rejectLate, &QTimer::timeout, this, [&] {
                if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                    latePrompt = true;
                    prompt->reject();
                }
            });
            rejectLate.start(5);
            delete first.data();
            guard.close();
            QTest::qWait(100);
            QVERIFY(!first && !page);
            QVERIFY(!latePrompt);
            QCOMPARE(second->tabPaths(), QStringList{kept});
            return;
        }
        const quint64 generation = page->loadGeneration();
        manager.mergeTab(first, 0, second, -1);
        // CloseFlow waits for this modal guard. Destroy the former owner directly
        // to keep the queue-discard boundary covered after that integration.
        delete first.data();
        QTRY_VERIFY(!first);
        QVERIFY(page && page->window() == second);
        QCOMPARE(page->loadGeneration(), generation);
        second->setWindowState(Qt::WindowNoState);
        int prompts = 0;
        QPointer<QInputDialog> answered;
        QTimer answer;
        answer.setInterval(5);
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!prompt || answered == prompt)
                return;
            answered = prompt;
            ++prompts;
            QCOMPARE(prompt->parentWidget(), second);
            QCOMPARE(prompt->windowModality(), Qt::WindowModal);
            prompt->setTextValue(QStringLiteral("correct"));
            prompt->accept();
        });
        answer.start();
        guard.close();
        if (action <= 1) {
            QTRY_VERIFY_WITH_TIMEOUT(page && page->isLoaded(), 1000);
            QCOMPARE(second->tabCount(), 2);
            QCOMPARE(prompts, action == 1 ? 1 : 0);
            if (action == 1)
                QCOMPARE(page->password(), QStringLiteral("correct"));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!page, 1000);
            QCOMPARE(second->tabPaths(), QStringList{kept});
        }
        answer.stop();
        job::testing::release();
    }

    void failedDuplicatePreservesAnArrivingTab()
    {
        QTemporaryDir files;
        const QString missing = files.filePath(QStringLiteral("missing.pdf"));
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        QVERIFY(testing::openAndWait(*first, QStringLiteral(MERVIN_FIXTURE_PDF)));
        holdRead(missing);
        manager.duplicateToNewWindow(missing, {}, {}, {});
        QPointer<MainWindow> duplicate;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<MainWindow *>(widget); window && window != first)
                duplicate = window;
        QVERIFY(duplicate);
        QVERIFY(testing::openAndWait(*duplicate, QStringLiteral(MERVIN_FIXTURE_PDF), true));
        job::testing::release();
        QTRY_VERIFY(duplicate && duplicate->tabCount() == 1);
        QVERIFY(current(*duplicate)->isLoaded());
    }

    void duplicatePreservesTheSourceView_data()
    {
        QTest::addColumn<bool>("unloaded");
        QTest::newRow("loaded") << false;
        QTest::newRow("unloaded") << true;
    }

    void duplicatePreservesTheSourceView()
    {
        QFETCH(bool, unloaded);
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        QVERIFY(testing::openAndWait(*first, QStringLiteral(MERVIN_FIXTURE_PDF)));
        auto *original = current(*first);
        original->viewer()->setScale(1.7);
        original->viewer()->setRotation(90);
        original->viewer()->restorePageScrollFraction(2, 0.2, 0.4);
        const ViewState saved = original->capturedViewState();
        if (unloaded) {
            QVERIFY(original->suspend());
            QVERIFY(original->isSuspended());
        }
        manager.duplicateToNewWindow(original->path(), {}, original->capturedViewState(), {});
        MainWindow *duplicate = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<MainWindow *>(widget); window && window != first)
                duplicate = window;
        QVERIFY(duplicate);
        QTRY_VERIFY(current(*duplicate)->isLoaded());
        const ViewState copied = current(*duplicate)->capturedViewState();
        QCOMPARE(copied.scale, saved.scale);
        QCOMPARE(copied.rotation, saved.rotation);
        QCOMPARE(copied.page, saved.page);
        QVERIFY(qAbs(copied.offsetX - saved.offsetX) < 0.002);
        QVERIFY(qAbs(copied.offsetY - saved.offsetY) < 0.002);
        QCOMPARE(original->capturedViewState().scale, saved.scale);
    }

    void backgroundPasswordFailureWaitsForExplicitRetry()
    {
        QTemporaryDir files;
        const QString encrypted = files.filePath(QStringLiteral("encrypted.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), encrypted, {},
                                 QStringLiteral("correct"), QStringLiteral("owner"),
                                 QpdfService::Algorithm::AES256, {}), QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        int unexpectedPrompts = 0;
        QTimer rejectUnexpected;
        rejectUnexpected.setInterval(5);
        connect(&rejectUnexpected, &QTimer::timeout, this, [&] {
            if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                ++unexpectedPrompts;
                prompt->reject();
            }
        });
        rejectUnexpected.start();
        QVERIFY(window.openFile(encrypted, false, -1, false));
        auto *tab = qobject_cast<TabPage *>(window.findChild<QTabWidget *>()->widget(1));
        QTRY_VERIFY(!tab->isLoading());
        QVERIFY(tab->freshOpen());
        QCOMPARE(unexpectedPrompts, 0);
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(tab->findChild<QLabel *>(QStringLiteral("documentLoadStatus"))->text(), QStringLiteral("This document is password-protected."));
        // The approved background-open behavior keeps Retry when selected later.
        QTest::qWait(100); // let the background prompt request leave the owner queue
        const quint64 failedGeneration = tab->loadGeneration();
        window.findChild<QTabWidget *>()->setCurrentWidget(tab);
        QTest::qWait(100);
        QCOMPARE(unexpectedPrompts, 0);
        QCOMPARE(tab->loadGeneration(), failedGeneration);
        QVERIFY(tab->isSuspended());
        QVERIFY(tab->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad"))->isVisible());
        rejectUnexpected.stop();
        int retryPrompts = 0;
        QTimer acceptRetry;
        acceptRetry.setInterval(5);
        connect(&acceptRetry, &QTimer::timeout, this, [&] {
            auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!prompt || prompt->property("testSubmitted").toBool())
                return;
            prompt->setProperty("testSubmitted", true);
            ++retryPrompts;
            QCOMPARE(prompt->windowModality(), Qt::WindowModal);
            prompt->setTextValue(QStringLiteral("correct"));
            prompt->accept();
        });
        acceptRetry.start();
        QTest::mouseClick(tab->findChild<QPushButton *>(QStringLiteral("retryDocumentLoad")), Qt::LeftButton);
        QTRY_VERIFY(tab->isLoaded());
        QCOMPARE(retryPrompts, 1);
        QCOMPARE(tab->password(), QStringLiteral("correct"));
    }

#ifdef Q_OS_UNIX
    void heldCommandLineFileDoesNotDelaySessionRestore()
    {
        QTemporaryDir files;
        const QString heldPath = files.filePath(QStringLiteral("held.pdf"));
        const QString restoredPath = files.filePath(QStringLiteral("restored.pdf"));
        const QString childProfile = files.filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(childProfile));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), restoredPath));
        QVERIFY(::mkfifo(QFile::encodeName(heldPath).constData(), 0600) == 0);
        QVERIFY(write(QDir(childProfile).filePath(QStringLiteral("config.toml")),
                      QByteArrayLiteral("auto_update = false\nrestore_session = true\nclose_to_tray = false\nui_language = \"en\"\n")));
        const QString sessionPath = QDir(childProfile).filePath(QStringLiteral("session.json"));
        QJsonObject session{{QStringLiteral("open"), QJsonArray{restoredPath}},
                            {QStringLiteral("active"), restoredPath}};
        QVERIFY(write(sessionPath, QJsonDocument(session).toJson()));
        testing::PipeWriter writer(heldPath, 30000);
        QProcess process;
        process.start(QStringLiteral(MERVIN_APP_EXE),
                      {QStringLiteral("--profile"), childProfile,
                       QStringLiteral("--quit-after-startup"), heldPath});
        const auto stopChild = qScopeGuard([&] {
            writer.release();
            if (!process.waitForFinished(10000)) {
                process.kill();
                process.waitForFinished(5000);
            }
        });
        QVERIFY(process.waitForStarted(10000));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto open = QJsonDocument::fromJson(bytes(sessionPath)).object()
                                  .value(QStringLiteral("open")).toArray();
            return open.contains(heldPath) && open.contains(restoredPath);
        }(), 10000);
        QCOMPARE(process.state(), QProcess::Running); // readiness still waits on the requested load
        writer.release();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    }
#endif

    void stagedBatchCompletesWithoutWaitingForARead()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("held.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        holdRead(path);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        bool done = false;
        manager.openStaged({{path, -1, true, false, false}}, {}, [&] { done = true; }, window);
        QTRY_VERIFY(done);
        QVERIFY(window->hasLoadingDocuments());
        QCOMPARE(window->tabPaths(), QStringList{path});
        bool restored = false;
        manager.openStaged({{QStringLiteral(MERVIN_FIXTURE_PDF), -1, false, true, false}}, {},
                           [&] { restored = true; }, window);
        QTRY_VERIFY(restored);
        QCOMPARE(window->tabCount(), 2);
        current(*window)->stopWaiting();
        QTRY_COMPARE(window->tabCount(), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> profile_;
};

QTEST_MAIN(TstBackgroundOpen)
#include "tst_background_open.moc"
