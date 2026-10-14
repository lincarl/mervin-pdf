#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "app/WindowManager.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "recent/PathKey.h"
#include "security/MeasureExport.h"
#include "ui/TabPage.h"
#include "ui/MainWindow.h"
#include "ui/ViewerWidget.h"
#include "Stalls.h"

#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

using namespace mervin;

namespace {
// Reads fixture bytes so failed or stopped Save can prove the original is unchanged.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Adds an editable note used to check snapshot retention and successful Save.
bool edit(TabPage &tab, const QString &text = QStringLiteral("preserved"))
{
    return tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {}, text) >= 0;
}

// Collects Save questions and completion while supplying a chosen replacement answer.
struct Outcome {
    std::optional<TabPage::SaveCompletion> completion;
    int questions = 0;
    bool replaceChanged = true;

    // Creates checkpoint and completion callbacks that update this captured outcome.
    TabPage::SaveCallbacks callbacks()
    {
        return {[this](auto valid, auto answer) {
                    ++questions;
                    if (valid())
                        answer(replaceChanged);
                },
                [this](const auto &result) { completion = result; }};
    }
};

// Lists private recovery artifacts to detect files left after terminal cleanup.
QStringList recoveryEntries()
{
    return QDir(QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recovery")))
        .entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}
}

// Checks background Save durability, cancellation, restored edits and completion ownership.
class TstSaveEdits : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> profile_;

private slots:
    // Creates an isolated profile and resets Save stall hooks for each case.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        QVERIFY(settings.save());
        TabPage::setSlowDelayForTesting(0);
    }

    // Releases stalls and settles closes and profile writes before resetting overrides.
    void cleanup()
    {
        job::testing::release();
        job::testing::failWhen({});
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
        profile_.reset();
    }

    // Checks durable recovery precedes original close and retains full viewer state.
    void loadedSaveRestoresFullStateAndCommitsRecoveryBeforeClosing()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        auto state = tab.viewer()->captureResumeState();
        state.tool = ViewerWidget::ToolMode::Measure;
        state.measureEnabled = true;
        state.measureKind = MeasureKind::Polyline;
        state.inProgressPage = 0;
        state.inProgress = {{20, 20}, {40, 40}};
        state.view.scale = 1.75;
        state.view.zoomMode = QStringLiteral("custom");
        tab.viewer()->restoreResumeState(state);
        const auto expected = tab.viewer()->captureResumeState();
        const auto lifetime = tab.viewer()->document()->lifetime();
        std::atomic<bool> manifestSeen{false}, closeSeen{false};
        job::testing::failWhen([&](job::Step step, const QString &destination) {
            if (step == job::Step::Commit && destination.endsWith(QStringLiteral(".json"))) {
                std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
                manifestSeen = lifetime->document != nullptr;
            }
            if (step == job::Step::Commit && destination == path) {
                std::lock_guard<std::timed_mutex> lock(lifetime->mutex);
                const auto entries = recoveryEntries();
                closeSeen = lifetime->document == nullptr
                    && std::any_of(entries.cbegin(), entries.cend(), [](const auto &name) {
                        return name.endsWith(QStringLiteral(".pdf.json"));
                    });
            }
            return false;
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        Outcome outcome;
        QElapsedTimer action;
        action.start();
        tab.startSave(outcome.callbacks());
        QVERIFY(action.elapsed() < 1000);
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QVERIFY(manifestSeen);
        QVERIFY(closeSeen);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(tab.isUsable());
        QVERIFY(!tab.hasUnsavedEdits());
        const auto restored = tab.viewer()->captureResumeState();
        QCOMPARE(restored.inProgress, expected.inProgress);
        QCOMPARE(restored.inProgressPage, expected.inProgressPage);
        QCOMPARE(restored.measureKind, expected.measureKind);
        QCOMPARE(restored.tool, expected.tool);
        QCOMPARE(restored.view.scale, expected.view.scale);
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        QCOMPARE(QDir(files.path()).entryList({QStringLiteral(".mervin-save-*")}, QDir::Files | QDir::Hidden).size(), 0);
    }

    // Selects the document gate or its access mutex for a real worker stall.
    void actualDocumentStall_data()
    {
        QTest::addColumn<bool>("gate");
        QTest::newRow("gate") << true;
        QTest::newRow("access") << false;
    }

    // Checks the real Save command returns while document access is held elsewhere.
    void saveCommandRunsOffGuiWithActualAccessStall()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        window.resize(1000, 800);
        window.show();
        QVERIFY(window.openFile(path));
        auto *tab = window.findChild<TabPage *>();
        QVERIFY(tab);
        QTRY_VERIFY(tab->isUsable());
        QVERIFY(edit(*tab));
        const auto lifetime = tab->viewer()->document()->lifetime();
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Reading && file == path;
        });
        std::unique_ptr<testing::Stall<std::mutex>> access;
        connect(tab, &TabPage::stateChanged, &window, [&] {
            if (tab->isBusy() && !access)
                access = std::make_unique<testing::Stall<std::mutex>>(
                    tab->viewer()->document()->accessMutex(), 10000);
        });
        const auto release = qScopeGuard([&] {
            job::testing::release();
            if (access)
                access->release();
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        QElapsedTimer command;
        command.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "saveMeasurements"));
        QVERIFY(command.elapsed() < 1000);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        QVERIFY(access);
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        auto *still = tab->findChild<QWidget *>(QStringLiteral("operationStill"));
        QVERIFY(still && !still->isHidden());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(lifetime->holds.load() > 0, 10000);
        tab->stopWaiting();
        QVERIFY(tab->isBusy());
        access->release();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(tab->hasUnsavedEdits());
    }

    // Selects canceled and accepted changed-source questions after an owner move.
    void changedQuestionSurvivesItsQueuedOwnersDestruction_data()
    {
        QTest::addColumn<bool>("replace");
        QTest::newRow("replace") << true;
        QTest::newRow("cancel") << false;
    }

    // Checks a queued replacement question follows the tab past old-window destruction.
    void changedQuestionSurvivesItsQueuedOwnersDestruction()
    {
        QFETCH(bool, replace);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(first->openFile(path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External edit\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        QDialog guard(second);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(QMetaObject::invokeMethod(first, "saveMeasurements"));
        QTRY_VERIFY(MainWindow::testing::hasQueuedTabChanges(*first));
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        int questions = 0;
        QPointer<QWidget> shownIn;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("File changed"))
                return;
            ++questions;
            shownIn = box->parentWidget();
            box->done(replace ? QMessageBox::Yes : QMessageBox::Cancel);
        });
        dialogs.start(5);
        guard.close();
        QTRY_VERIFY_WITH_TIMEOUT(questions == 1 && !page->isBusy(), 10000);
        QCOMPARE(shownIn.data(), second);
        QCOMPARE(page->hasUnsavedEdits(), !replace);
        if (!replace)
            QCOMPARE(bytes(path), external);
        else
            QVERIFY(bytes(path) != external);
    }

    // Selects successful and failed Save reports after an owner move.
    void saveReportSurvivesItsQueuedOwnersDestruction_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::newRow("Saved status") << false;
        QTest::newRow("Save error") << true;
    }

    // Checks Save status is delivered only by the surviving current window.
    void saveReportSurvivesItsQueuedOwnersDestruction()
    {
        QFETCH(bool, failed);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        QVERIFY(first->openFile(path));
        const QPointer<TabPage> page = first->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        if (failed)
            job::testing::failWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        QDialog guard(second);
        guard.setWindowModality(Qt::WindowModal);
        guard.show();
        QVERIFY(QMetaObject::invokeMethod(first, "saveMeasurements"));
        QTRY_VERIFY_WITH_TIMEOUT(!page->isBusy()
            && MainWindow::testing::hasQueuedTabChanges(*first), 10000);
        second->adoptTab(first->releaseTab(0));
        delete first.data();
        QVERIFY(!first);
        int reports = 0;
        QPointer<QWidget> shownIn;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("Save"))
                return;
            ++reports;
            shownIn = box->parentWidget();
            box->done(QMessageBox::Ok);
        });
        dialogs.start(5);
        guard.close();
        if (failed) {
            QTRY_COMPARE_WITH_TIMEOUT(reports, 1, 10000);
            QCOMPARE(shownIn.data(), second);
            QVERIFY(page->hasUnsavedEdits());
        } else {
            const QString saved = QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(path));
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                for (auto *label : second->findChildren<QLabel *>())
                    if (label->text() == saved)
                        return true;
                return false;
            }(), 10000);
            QVERIFY(!page->hasUnsavedEdits());
            QCOMPARE(reports, 0);
        }
    }

    // Checks the warning distinguishes successful replacement from failed reopening.
    void reopenFailureReportsTheCommittedOriginal()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(path));
        const QPointer<TabPage> page = window->findChild<TabPage *>();
        QTRY_VERIFY(page && page->isUsable());
        QVERIFY(edit(*page));
        std::atomic<bool> reopenFailed{false};
        job::testing::failWhen([path, &reopenFailed, reads = 0]
                              (job::Step step, const QString &file) mutable {
            if (step == job::Step::Reading && file == path && ++reads >= 3) {
                reopenFailed = true;
                return true;
            }
            return false;
        });
        job::testing::holdWhen([&reopenFailed](job::Step step, const QString &file) {
            return reopenFailed && step == job::Step::Reading
                && file.contains(QStringLiteral("/recovery/"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QString warning;
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box || box->windowTitle() != QStringLiteral("Save"))
                return;
            warning = box->text();
            box->done(QMessageBox::Ok);
        });
        dialogs.start(5);
        QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
        QTRY_VERIFY_WITH_TIMEOUT(!warning.isEmpty(), 10000);
        QVERIFY(bytes(path) != original);
        QVERIFY(page->isLoading());
        QVERIFY(page->hasRecoverySnapshot());
        QVERIFY2(warning.startsWith(QStringLiteral(
                     "The file was saved, but could not be reopened.")), qPrintable(warning));
        QVERIFY(!warning.contains(QStringLiteral("remains open")));
        QVERIFY(warning.contains(QDir::toNativeSeparators(page->recoveryPath())));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QVERIFY(page->hasUnsavedEdits());
    }

    // Checks worker dispatch, independent editing and completion through controlled stalls.
    void actualDocumentStall()
    {
        QFETCH(bool, gate);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        tab.resize(1000, 800);
        tab.show();
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        const auto lifetime = tab.viewer()->document()->lifetime();
        Document *document = tab.viewer()->document();
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Reading && file == path;
        });
        const int guiSteps = job::testing::stepsOnMainThread();
        Outcome outcome;
        QElapsedTimer action;
        action.start();
        tab.startSave(outcome.callbacks());
        QVERIFY(action.elapsed() < 1000);
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        auto *still = tab.findChild<QWidget *>(QStringLiteral("operationStill"));
        QVERIFY(still && !still->isHidden());
        std::unique_ptr<testing::Stall<std::timed_mutex>> gateStall;
        std::unique_ptr<testing::Stall<std::mutex>> accessStall;
        if (gate)
            gateStall = std::make_unique<testing::Stall<std::timed_mutex>>(lifetime->mutex, 10000);
        else
            accessStall = std::make_unique<testing::Stall<std::mutex>>(document->accessMutex(), 10000);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(lifetime->holds.load() > 0, 10000);
        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
        heartbeat.start(10);
        QTRY_VERIFY_WITH_TIMEOUT(heartbeats >= 5, 1000);
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        action.restart();
        tab.stopWaiting();
        QVERIFY(action.elapsed() < 1000);
        QVERIFY(outcome.completion.has_value());
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Stopped);
        if (!gate) {
            QVERIFY(tab.isBusy());
            auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
            QVERIFY(stop && stop->isHidden());
        }
        if (gateStall)
            gateStall->release();
        if (accessStall)
            accessStall->release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(bytes(path), original);
        QVERIFY(!tab.hasRecoverySnapshot());
    }

    // Checks Stop retains snapshot lock and edits until the held worker releases access.
    void stopDuringSnapshotKeepsTheTrappedNoticeUntilRelease()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        job::testing::holdWhen([](job::Step step, const QString &folder) {
            return step == job::Step::Writing && folder.contains(QStringLiteral("/.save-"));
        });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        tab.stopWaiting();
        QVERIFY(tab.isBusy());
        QVERIFY(outcome.completion.has_value());
        auto *text = tab.findChild<QLabel *>(QStringLiteral("waitBarText"));
        auto *stop = tab.findChild<QPushButton *>(QStringLiteral("waitBarStop"));
        QVERIFY(text && text->text().contains(QStringLiteral("document stays locked")));
        QVERIFY(stop && stop->isHidden());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Selects stops before and during guarded original replacement.
    void stoppedReplacementRecoversAndRetryWaits_data()
    {
        QTest::addColumn<bool>("commitStarted");
        QTest::newRow("before commit") << false;
        QTest::newRow("commit started") << true;
    }

    // Checks recovery remains editable and retry respects the earlier replacement worker.
    void stoppedReplacementRecoversAndRetryWaits()
    {
        QFETCH(bool, commitStarted);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        if (commitStarted)
            job::testing::holdCommitWhen([path](const QString &file) { return file == path; });
        else
            job::testing::holdWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        Outcome stopped;
        tab.startSave(stopped.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        const QString firstCopy = tab.recoveryPath();
        tab.stopWaiting();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(stopped.completion->result, TabPage::SaveResult::Stopped);
        QCOMPARE(bytes(path), original);
        QVERIFY(edit(tab, QStringLiteral("newer")));
        Outcome retry;
        tab.startSave(retry.callbacks());
        QTest::qWait(100);
        QVERIFY(tab.isBusy());
        QVERIFY(!retry.completion.has_value());
        QVERIFY(QFileInfo::exists(firstCopy));
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(retry.completion.has_value(), 10000);
        QCOMPARE(retry.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(retry.questions, commitStarted ? 1 : 0);
        QVERIFY(!tab.hasUnsavedEdits());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), size_t(2));
        QCOMPARE(notes.back().contents, QStringLiteral("newer"));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks unloaded Save copies retained recovery and asks about external source changes.
    void unloadedSaveDoesNotOpenPdfAndChangedSourceRequiresAnswer()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(tab.suspend());
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        const int opens = RenderEngine::testing::opens();
        Outcome cancel;
        cancel.replaceChanged = false;
        tab.startSave(cancel.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(cancel.completion.has_value(), 10000);
        QCOMPARE(cancel.completion->result, TabPage::SaveResult::Canceled);
        QCOMPARE(cancel.questions, 1);
        QCOMPARE(bytes(path), external);
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(!tab.isLoaded());
        Outcome replace;
        tab.startSave(replace.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(replace.completion.has_value(), 10000);
        QCOMPARE(replace.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(replace.questions, 1);
        QCOMPARE(RenderEngine::testing::opens(), opens);
        QVERIFY(!tab.isLoaded());
        QVERIFY(!tab.hasUnsavedEdits());
        auto saved = engine.openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

    // Selects automatic resume and Retry during running or queued Save completion.
    void unloadedSaveKeepsItsJobWhenLoadIsRequested_data()
    {
        QTest::addColumn<bool>("retry");
        QTest::addColumn<bool>("stored");
        QTest::newRow("resume during Save") << false << false;
        QTest::newRow("Retry during Save") << true << false;
        QTest::newRow("resume before Save delivery") << false << true;
        QTest::newRow("Retry before Save delivery") << true << true;
    }

    // Checks load requests preserve an unloaded Save and work after its delivery.
    void unloadedSaveKeepsItsJobWhenLoadIsRequested()
    {
        QFETCH(bool, retry);
        QFETCH(bool, stored);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(tab.suspend());
        // Stop standing render workers so settle can wait for Save without GUI delivery.
        engine.shutdown();
        QVERIFY(background::settle(10000));
        const QString copy = tab.recoveryPath();
        QVERIFY(!copy.isEmpty());
        const int opens = RenderEngine::testing::opens();
        QSignalSpy resumed(&tab, &TabPage::resumeFinished);
        QSignalSpy finished(&tab, &TabPage::operationFinished);
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Commit && file == path;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        if (stored) {
            job::testing::release();
            QVERIFY(background::settle(10000));
        }
        QVERIFY(tab.isBusy());
        QVERIFY(!outcome.completion.has_value());
        const auto generation = tab.saveGeneration();
        if (retry)
            tab.retryLoad();
        else
            tab.resumeAsync();
        QVERIFY(!tab.isLoading());
        QVERIFY(tab.isBusy());
        QCOMPARE(tab.saveGeneration(), generation);
        QCOMPARE(tab.recoveryPath(), copy);
        QCOMPARE(resumed.size(), 0);
        QCOMPARE(RenderEngine::testing::opens(), opens);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(resumed.size(), 0);
        QVERIFY(!tab.isBusy());
        QVERIFY(!tab.isLoading());
        QVERIFY(!tab.isLoaded());
        QVERIFY(!tab.hasUnsavedEdits());
        QVERIFY(bytes(path) != original);
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (retry)
            tab.retryLoad();
        else
            tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(resumed.front().front().toBool());
        QVERIFY(tab.isUsable());
        const auto notes = tab.viewer()->annotModel()->pageAnnots(0);
        QCOMPARE(notes.size(), size_t(1));
        QCOMPARE(notes.front().contents, QStringLiteral("preserved"));
    }

    // Selects manifest commit, original replacement and reopen failures.
    void failedSaveKeepsEdits_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("manifest commit") << 0;
        QTest::newRow("original commit") << 1;
        QTest::newRow("reopen original") << 2;
    }

    // Checks every failure preserves an editable document or durable recovery.
    void failedSaveKeepsEdits()
    {
        QFETCH(int, failure);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        job::testing::failWhen([failure, path, reads = 0](job::Step step, const QString &file) mutable {
            if (failure == 0)
                return step == job::Step::Commit && file.endsWith(QStringLiteral(".json"));
            if (failure == 1)
                return step == job::Step::Commit && file == path;
            if (step == job::Step::Reading && file == path)
                return ++reads >= 3;
            return false;
        });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Failed);
        QCOMPARE(outcome.completion->originalSaved, failure == 2);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(tab.hasRecoverySnapshot(), failure != 0);
        if (failure != 2)
            QCOMPARE(bytes(path), original);
        if (failure != 0)
            QVERIFY(TabPage::recoverablePaths().contains(path));
        else
            QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks owner deletion abandons a pending approval without stranding its worker.
    void deletingTabAtChangedQuestionWakesItsWorker()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        QVERIFY(tab->open(path));
        QVERIFY(edit(*tab));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        const QByteArray external = bytes(path);
        const QPointer<TabPage> page(tab);
        bool completed = false;
        TabPage::SaveCallbacks callbacks;
        callbacks.changed = [page](auto, auto) { delete page.data(); };
        callbacks.finished = [&](const auto &) { completed = true; };
        tab->startSave(std::move(callbacks));
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(!completed);
        QCOMPARE(bytes(path), external);
        QVERIFY(recoveryEntries().isEmpty());
    }

    // Checks direct Stop during viewer detachment preserves the already adopted snapshot.
    void stopFromViewerSignalDuringAdoptionKeepsRecovery()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        bool stopped = false;
        connect(tab.viewer(), &ViewerWidget::formModeChanged, &tab, [&](bool) {
            if (tab.isBusy() && tab.hasRecoverySnapshot() && !stopped) {
                stopped = true;
                tab.stopWaiting();
            }
        });
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Stopped);
        QVERIFY(stopped);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(tab.hasRecoverySnapshot());
        QVERIFY(tab.hasUnsavedEdits());
        QCOMPARE(tab.viewer()->annotModel()->pageAnnots(0).size(), size_t(1));
        QCOMPARE(bytes(path), original);
        QVERIFY(TabPage::recoverablePaths().contains(path));
        QVERIFY(QFileInfo::exists(tab.recoveryPath()));
    }

    // Selects deletion while successful or failed Save restores the tab.
    void deletionDuringDelivery_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::addColumn<bool>("afterRecovery");
        QTest::newRow("successful Save unfreezes") << false << false;
        QTest::newRow("failed Save unfreezes") << true << false;
        QTest::newRow("failed Save recovery finishes") << true << true;
    }

    // Checks callback-driven deletion closes documents and removes owned recovery safely.
    void deletionDuringDelivery()
    {
        QFETCH(bool, failed);
        QFETCH(bool, afterRecovery);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        const QByteArray original = bytes(path);
        RenderEngine engine;
        auto *tab = new TabPage(&engine);
        const QPointer<TabPage> page(tab);
        QVERIFY(tab->open(path));
        QVERIFY(edit(*tab));
        if (failed)
            job::testing::failWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
        bool began = false;
        connect(tab, &TabPage::stateChanged, &engine, [&, page] {
            if (!page)
                return;
            began = began || page->isBusy();
            if (began && !page->isBusy() && (!afterRecovery || page->isUsable()))
                delete page.data();
        });
        Outcome outcome;
        tab->startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(recoveryEntries().isEmpty());
        if (failed)
            QCOMPARE(bytes(path), original);
        else
            QVERIFY(bytes(path) != original);
        QCOMPARE(outcome.completion.has_value(), afterRecovery);
    }

    // Selects earlier stopped recovery readers and competing Save outcomes.
    void unloadedSaveRetainsAnAbandonedLoadInput_data()
    {
        QTest::addColumn<bool>("stopFirst");
        QTest::newRow("Save completes while load waits") << false;
        QTest::newRow("stopped Save then retry") << true;
    }

    // Checks retained recovery remains present until every abandoned reader closes.
    void unloadedSaveRetainsAnAbandonedLoadInput()
    {
        QFETCH(bool, stopFirst);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(path));
        QVERIFY(edit(tab));
        QVERIFY(tab.suspend());
        const QString copy = tab.recoveryPath();
        job::testing::holdWhen([copy, path, stopFirst, first = true](job::Step step, const QString &file) mutable {
            const bool initialLoad = step == job::Step::Reading && file == copy && first;
            if (initialLoad)
                first = false;
            return initialLoad
                || (stopFirst && step == job::Step::Commit && file == path);
        });
        tab.resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        Outcome first;
        tab.startSave(first.callbacks());
        if (stopFirst) {
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 2, 10000);
            tab.stopWaiting();
            QVERIFY(first.completion.has_value());
            QCOMPARE(first.completion->result, TabPage::SaveResult::Stopped);
            QVERIFY(tab.hasUnsavedEdits());
            Outcome retry;
            tab.startSave(retry.callbacks());
            QTest::qWait(100);
            QVERIFY(!retry.completion.has_value());
            QVERIFY(QFileInfo::exists(copy));
            job::testing::release();
            QTRY_VERIFY_WITH_TIMEOUT(retry.completion.has_value(), 10000);
            QCOMPARE(retry.completion->result, TabPage::SaveResult::Saved);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(first.completion.has_value(), 10000);
            QCOMPARE(first.completion->result, TabPage::SaveResult::Saved);
            QVERIFY(QFileInfo::exists(copy));
            job::testing::release();
        }
        QVERIFY(!tab.hasUnsavedEdits());
        QVERIFY(!tab.isLoaded());
        engine.shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QVERIFY(!QFileInfo::exists(copy));
        QVERIFY(recoveryEntries().isEmpty());
    }

#ifdef Q_OS_UNIX
    // Checks a changed alias retains its new key before the worker scope releases reservations.
    void retargetedAliasKeepsItsNewCanonicalAdmission()
    {
        QTemporaryDir files;
        const QString first = files.filePath(QStringLiteral("first.pdf"));
        const QString second = files.filePath(QStringLiteral("second.pdf"));
        const QString alias = files.filePath(QStringLiteral("alias.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), first));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), second));
        QVERIFY(QFile::link(first, alias));
        RenderEngine engine;
        TabPage tab(&engine);
        QVERIFY(tab.open(alias));
        QVERIFY(edit(tab));
        const QByteArray initial = bytes(first);
        QVERIFY(QFile::remove(alias));
        QVERIFY(QFile::link(second, alias));
        QFile changed(second);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% Different target\n") > 0);
        changed.close();
        Outcome outcome;
        tab.startSave(outcome.callbacks());
        QTRY_VERIFY_WITH_TIMEOUT(outcome.completion.has_value(), 10000);
        QCOMPARE(outcome.completion->result, TabPage::SaveResult::Saved);
        QCOMPARE(outcome.questions, 1);
        QCOMPARE(tab.canonicalPath(), second);
        QCOMPARE(bytes(first), initial);
        job::State otherWriter;
        const job::Scope scope(otherWriter);
        QVERIFY(!job::reserveDestinations({fileKey(QFileInfo(second))}));
        QVERIFY(job::reserveDestinations({fileKey(QFileInfo(first))}));
        auto saved = engine.openDocument(second);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }
#endif
};

QTEST_MAIN(TstSaveEdits)
#include "tst_save_edits.moc"
