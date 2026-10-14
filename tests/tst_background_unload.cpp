#include "TestWaits.h"
#include "Stalls.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace mervin;

namespace {
QString copy(QTemporaryDir &files, const QString &name)
{
    const QString path = files.filePath(name);
    return QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path) ? path : QString();
}

QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool edit(TabPage &tab, const QString &text = QStringLiteral("Retained edit"))
{
    return tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, text) >= 0;
}

QStringList recoveryEntries()
{
    return QDir(QDir(ConfigPaths::configDirPath()).filePath(QStringLiteral("recovery")))
        .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}
}

class TstBackgroundUnload : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> profile_;

private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
        TabPage::setSlowDelayForTesting(0);
    }

    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
        QCoreApplication::processEvents();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        profile_.reset();
    }

    void profilePathDoesNotCreateItsDirectory()
    {
        const QString path = profile_->filePath(QStringLiteral("not-created/profile"));
        ConfigPaths::setOverrideDir(path);
        QCOMPARE(ConfigPaths::configDirPath(), path);
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(ConfigPaths::configDir(), path);
        QVERIFY(QFileInfo(path).isDir());
    }

    void activityTickReturnsDuringAnActualDocumentStall()
    {
        QTemporaryDir files;
        const QString firstPath = copy(files, QStringLiteral("inactive.pdf"));
        const QString secondPath = copy(files, QStringLiteral("active.pdf"));
        QVERIFY(!firstPath.isEmpty() && !secondPath.isEmpty());
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.show();
        QVERIFY(testing::openAndWait(window, firstPath));
        auto *first = window.findChild<TabPage *>();
        QVERIFY(first && edit(*first));
        QVERIFY(testing::openAndWait(window, secondPath));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        auto *second = qobject_cast<TabPage *>(tabs->currentWidget());
        QVERIFY(second && second != first);
        testing::Stall stall(first->viewer()->document()->accessMutex(), 20000);
        const int guiSteps = job::testing::stepsOnMainThread();
        window.updateDocumentActivity(0, 1);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(window.updateDocumentActivity(61000, 1));
        QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(first->isUnloading() && first->isBusy() && first->isLoaded());
        QVERIFY(window.hasResidentDocuments());
        QVERIFY(first->hasPendingRecoveryWrite());
        QVERIFY(edit(*second, QStringLiteral("Another tab stays editable")));
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        stall.release();
        QTRY_VERIFY_WITH_TIMEOUT(first->isSuspended(), 10000);
        QVERIFY(first->hasRecoverySnapshot());
        tabs->setCurrentWidget(first);
        QTRY_VERIFY_WITH_TIMEOUT(first->isUsable(), 10000);
        QCOMPARE(first->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(second->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(first->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
        QCOMPARE(second->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Another tab stays editable"));
    }

    void selectionBeforeReservationCancelsWithoutRecoveryFiles()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        Document *document = tab.viewer()->document();
        const QString directory = QDir(profile_->path()).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &path) {
            return step == job::Step::Writing && path == directory;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab.hasPendingRecoveryWrite());
        tab.selectForUse();
        QVERIFY(tab.isUsable());
        QVERIFY(!tab.isUnloading());
        QCOMPARE(tab.viewer()->document(), document);
        QVERIFY(tab.hasUnsavedEdits());
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void selectingTheActualTabCancelsBeforeTheQueuedResume()
    {
        QTemporaryDir files;
        const QString inactive = copy(files, QStringLiteral("inactive.pdf"));
        const QString active = copy(files, QStringLiteral("active.pdf"));
        QVERIFY(!inactive.isEmpty() && !active.isEmpty());
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.show();
        QVERIFY(testing::openAndWait(window, inactive));
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        const QPointer<TabPage> first = qobject_cast<TabPage *>(tabs->currentWidget());
        QVERIFY(first && edit(*first));
        Document *document = first->viewer()->document();
        QVERIFY(testing::openAndWait(window, active));
        const QString directory = QDir(profile_->path()).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &path) {
            return step == job::Step::Writing && path == directory;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        window.updateDocumentActivity(0, 1);
        QVERIFY(window.updateDocumentActivity(61000, 1));
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(first->isUnloading());
        tabs->setCurrentWidget(first);
        // No event processing between the actual selection and this assertion.
        QVERIFY(first->isUsable());
        QCOMPARE(first->viewer()->document(), document);
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void selectionAfterReservation_data()
    {
        QTest::addColumn<bool>("afterRelease");
        QTest::newRow("during snapshot Hold") << false;
        QTest::newRow("after Hold before manifest") << true;
    }

    void selectionAfterReservation()
    {
        QFETCH(bool, afterRelease);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        Document *document = tab.viewer()->document();
        job::testing::holdWhen([afterRelease](job::Step step, const QString &path) {
            return afterRelease ? step == job::Step::Commit && path.endsWith(QStringLiteral(".json"))
                : step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        tab.selectForUse();
        QVERIFY(tab.isBusy());
        QVERIFY(tab.isUnloading());
        QVERIFY(tab.hasPendingRecoveryWrite());
        QVERIFY(!tab.isOperationStopped());
        job::testing::release();
        // Let the worker commit, without processing its GUI adoption callback.
        QElapsedTimer elapsed;
        elapsed.start();
        while (tab.hasPendingRecoveryWrite() && elapsed.elapsed() < 10000)
            QThread::msleep(1);
        QVERIFY(!tab.hasPendingRecoveryWrite());
        QCOMPARE(TabPage::recoverablePaths().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QCOMPARE(tab.viewer()->document(), document);
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(!tab.hasRecoverySnapshot());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void stopDuringHoldKeepsTheTrappedNotice()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        tab.show();
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &path) {
            return step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        tab.stopWaiting();
        QVERIFY(tab.isBusy());
        QVERIFY(!tab.isUsable());
        auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
        QVERIFY(stop && stop->isHidden());
        auto *notice = tab.findChild<QLabel *>(QStringLiteral("waitBarText"));
        QVERIFY(notice);
        QCOMPARE(notice->text(), TabPage::tr(
            "Stopped waiting. The document stays locked until the drive answers."));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void workerManifestCommitEndsTheLocalDurabilityWindow_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("loaded Save") << true;
        QTest::newRow("edited unload") << false;
    }

    void workerManifestCommitEndsTheLocalDurabilityWindow()
    {
        QFETCH(bool, save);
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &file) {
            return step == job::Step::Commit && file.endsWith(QStringLiteral(".json"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        if (save)
            tab.startSave({});
        else
            QVERIFY(tab.suspend());
        QVERIFY(tab.hasPendingRecoveryWrite());
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(TabPage::recoverablePaths().isEmpty());
        job::testing::release();
        QElapsedTimer elapsed;
        elapsed.start();
        while (tab.hasPendingRecoveryWrite() && elapsed.elapsed() < 10000)
            QThread::msleep(1);
        QVERIFY(!tab.hasPendingRecoveryWrite());
        // Save waits at adoption and unload waits in the posted GUI callback.
        QCOMPARE(TabPage::recoverablePaths(), QStringList{path});
        QVERIFY(tab.isBusy());
        tab.stopWaiting();
        QVERIFY(tab.isUsable());
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void deletingTheTabDoesNotWaitOrKeepNewRecoveryFiles_data()
    {
        QTest::addColumn<bool>("committing");
        QTest::newRow("snapshot Hold") << false;
        QTest::newRow("manifest commit already began") << true;
    }

    void deletingTheTabDoesNotWaitOrKeepNewRecoveryFiles()
    {
        QFETCH(bool, committing);
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(*tab));
        if (committing)
            job::testing::holdCommitWhen([](const QString &path) {
                return path.endsWith(QStringLiteral(".json"));
            });
        else
            job::testing::holdWhen([](job::Step step, const QString &path) {
                return step == job::Step::Writing && QFileInfo(path).fileName().startsWith(QStringLiteral(".unload-"));
            });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab->suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QElapsedTimer elapsed;
        elapsed.start();
        delete tab;
        QVERIFY(!page);
        QVERIFY(elapsed.elapsed() < 1000);
        job::testing::release();
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    void stopDuringViewerDetachmentKeepsTheCommittedRecovery()
    {
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(tab));
        bool stopped = false;
        connect(tab.viewer(), &ViewerWidget::formModeChanged, &tab, [&](bool) {
            if (tab.isBusy() && tab.hasRecoverySnapshot() && !stopped) {
                stopped = true;
                tab.stopWaiting();
            }
        });
        QVERIFY(tab.suspend());
        QTRY_VERIFY_WITH_TIMEOUT(stopped && tab.isUsable(), 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
        QVERIFY(QFileInfo::exists(tab.recoveryPath()));
        QVERIFY(QFileInfo::exists(tab.recoveryPath() + QStringLiteral(".json")));
    }

    void deletingDuringAdoptionClosesTheDocumentAndRecovery()
    {
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(QStringLiteral(MERVIN_FIXTURE_PDF)));
        QVERIFY(edit(*tab));
        const auto lifetime = tab->viewer()->document()->lifetime();
        connect(tab, &TabPage::stateChanged, &engine, [page] {
            if (page && !page->isBusy() && page->hasRecoverySnapshot())
                delete page.data();
        });
        QVERIFY(tab->suspend());
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        {
            std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
            QVERIFY(!lifetime->document);
        }
        QVERIFY(recoveryEntries().isEmpty());
    }

    void closeStopsHousekeepingSilently_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("Don't save during snapshot") << false;
        QTest::newRow("Save after document release") << true;
    }

    void closeStopsHousekeepingSilently()
    {
        QFETCH(bool, save);
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        const QByteArray original = bytes(path);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> page = window->findChild<TabPage *>();
        QVERIFY(page && edit(*page));
        job::testing::holdWhen([](job::Step step, const QString &where) {
            return step == job::Step::Writing && QFileInfo(where).fileName().startsWith(QStringLiteral(".unload-"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(page->suspend());
        QTRY_COMPARE(job::testing::held(), 1);
        QSignalSpy userStopped(page, &TabPage::waitingStopped);
        int questions = 0;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            ++questions;
            box->done(save ? QMessageBox::Save : QMessageBox::Discard);
        });
        dialogs.start(5);
        QVERIFY(QMetaObject::invokeMethod(window, "closeTab", Q_ARG(int, 0)));
        QTRY_COMPARE_WITH_TIMEOUT(questions, 1, 10000);
        QCOMPARE(userStopped.size(), 0);
        if (save)
            QVERIFY(page && page->isBusy());
        else
            QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        manager.engine()->shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (save) {
            QVERIFY(bytes(path) != original);
            RenderEngine check;
            auto document = check.openDocument(path);
            QVERIFY(document);
            QCOMPARE(AnnotModel(*document).pageAnnots(0).size(), size_t(1));
            QCOMPARE(AnnotModel(*document).pageAnnots(0).front().contents,
                     QStringLiteral("Retained edit"));
        } else {
            QCOMPARE(bytes(path), original);
        }
    }

    void movedTabReportsProfileFailureInItsCurrentOwner()
    {
        QTemporaryDir files;
        const QString path = copy(files, QStringLiteral("edited.pdf"));
        QVERIFY(!path.isEmpty());
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        first->hide();
        second->hide();
        QVERIFY(testing::openAndWait(*first, path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QVERIFY(page && edit(*page));
        const QString blocker = profile_->filePath(QStringLiteral("not-a-folder"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("Not a directory") > 0);
        file.close();
        ConfigPaths::setOverrideDir(blocker);
        const QString directory = QDir(blocker).filePath(QStringLiteral("recovery"));
        job::testing::holdWhen([directory](job::Step step, const QString &where) {
            return step == job::Step::Writing && where == directory;
        });
        const auto restore = qScopeGuard([&] {
            job::testing::release();
            ConfigPaths::setOverrideDir(profile_->path());
        });
        first->updateDocumentActivity(0, 1);
        QVERIFY(first->updateDocumentActivity(61000, 1));
        QTRY_COMPARE(job::testing::held(), 1);
        ConfigPaths::setOverrideDir(profile_->path());
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QVERIFY(page->hasUnsavedEdits());
        QTRY_VERIFY(second->statusBar()->currentMessage().startsWith(
            QStringLiteral("edited.pdf remains loaded.")));
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(page->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Retained edit"));
    }
};

QTEST_MAIN(TstBackgroundUnload)
#include "tst_background_unload.moc"
