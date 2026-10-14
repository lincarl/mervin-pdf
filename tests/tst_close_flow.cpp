#include "app/Relaunch.h"
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
#include "Stalls.h"
#include "TestWaits.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QTimer>
#include <QtTest>

#include <functional>
#include <memory>
#include <atomic>

#ifdef Q_OS_UNIX
#  include <sys/stat.h>
#endif

using namespace mervin;

namespace {

// Finds the selected document tab in a test window.
TabPage *currentTab(MainWindow *window)
{
    auto *tabs = window->findChild<QTabWidget *>();
    return tabs ? qobject_cast<TabPage *>(tabs->currentWidget()) : nullptr;
}


// Starts a stoppable file operation held at its first read.
void holdOperation(TabPage *tab, const QString &path)
{
    job::testing::holdWhen([path](job::Step step, const QString &where) {
        return step == job::Step::Reading && where == path;
    });
    tab->startOperation({QFileInfo(path).absolutePath(), [path] { job::reading(path); }, {}});
}

// Sends the close-tab command through MainWindow's slot.
void closeTab(MainWindow *window, int index = 0)
{
    QVERIFY(QMetaObject::invokeMethod(window, "closeTab", Q_ARG(int, index)));
}

// Reports whether the tab explicitly shows its operation wait bar.
bool barShown(TabPage *tab)
{
    auto *bar = tab ? tab->findChild<QWidget *>(QStringLiteral("waitBar")) : nullptr;
    return bar && !bar->isHidden();
}

} // namespace

// Checks asynchronous close decisions, ownership changes and updater admission.
class TstCloseFlow : public QObject
{
    Q_OBJECT

private slots:
    // Creates an isolated profile and starts automatic answers for expected close questions.
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
        answers_.clear();
        questions_.clear();
        unexpected_.clear();
        beforeAnswer_ = {};
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            questions_.append(box->text());
            if (beforeAnswer_)
                beforeAnswer_(box);
            if (answers_.isEmpty()) {
                unexpected_.append(box->text());
                box->reject();
                return;
            }
            auto *button = box->button(answers_.takeFirst());
            if (!button) {
                unexpected_.append(box->text());
                box->reject();
                return;
            }
            button->click();
        });
        dialogs_.start(5);
    }

    // Releases held work and checks cleanup and dialog answers before removing the profile.
    void cleanup()
    {
        job::testing::release();
        QVERIFY(background::settle(10000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        dialogs_.stop();
        disconnect(&dialogs_, nullptr, this, nullptr);
        ConfigPaths::setOverrideDir({});
        profile_.reset();
        QVERIFY2(unexpected_.isEmpty(), qPrintable(unexpected_.join(u'\n')));
        QVERIFY(answers_.isEmpty());
    }

    // Selects successful, failed and stopped Save results while Quit waits.
    void quitTakesTheExplicitResultOfAnExistingSave_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("Saved closes") << 0;
        QTest::newRow("Failed cancels Quit") << 1;
        QTest::newRow("Stopped cancels Quit") << 2;
    }

    // Checks Quit uses the explicit earlier Save outcome without a duplicate write.
    void quitTakesTheExplicitResultOfAnExistingSave()
    {
        QFETCH(int, outcome);
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray before = original.readAll();
        original.close();
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Save selected before Quit") >= 0);
        std::atomic<int> commits{0};
        job::testing::holdWhen([path, &commits](job::Step step, const QString &file) {
            if (step != job::Step::Commit || file != path)
                return false;
            ++commits;
            return true;
        });
        answers_ << QMessageBox::Save;
        closeTab(window);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab->isBusy() && tab->isClosing());
        manager.quitAll();
        QTest::qWait(50);
        QVERIFY(window && tab && tab->isClosing());
        if (outcome == 1) {
            job::testing::failWhen([path, first = true](job::Step step, const QString &file) mutable {
                if (step != job::Step::Commit || file != path || !first)
                    return false;
                first = false;
                return true;
            });
            answers_ << QMessageBox::Ok;
        } else if (outcome == 2) {
            tab->stopWaiting();
        }
        job::testing::release();
        if (outcome == 0) {
            QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
            auto saved = manager.engine()->openDocument(path);
            QVERIFY(saved);
            QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                     QStringLiteral("Save selected before Quit"));
            QCOMPARE(questions_.size(), 1);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(tab && !tab->isClosing() && tab->isUsable(), 10000);
            QVERIFY(window && tab->hasUnsavedEdits());
            QTRY_VERIFY(answers_.isEmpty());
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(), before);
            QCOMPARE(questions_.size(), outcome == 1 ? 2 : 1);
        }
        QCOMPARE(commits.load(), 1);
    }

    // Checks a new edit during Save completion prevents reuse of the earlier close answer.
    void anEditAtSaveCompletionRequiresAnotherDecision()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Saved edit") >= 0);
        bool busySeen = false, edited = false;
        connect(tab, &TabPage::stateChanged, this, [&, tab] {
            if (!tab)
                return;
            busySeen = busySeen || tab->isBusy();
            if (busySeen && tab->isUsable() && !tab->hasUnsavedEdits() && !edited) {
                edited = true;
                addNote(tab, "Edit after Save");
            }
        });
        answers_ << QMessageBox::Save << QMessageBox::Cancel;
        closeTab(window);
        QTRY_VERIFY(edited && questions_.size() == 2 && answers_.isEmpty());
        QVERIFY(tab && !tab->isClosing() && tab->hasUnsavedEdits());
        QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).size(), size_t(2));
        auto saved = manager.engine()->openDocument(path);
        QVERIFY(saved);
        const auto notes = AnnotModel(*saved).pageAnnots(0);
        QCOMPARE(notes.size(), size_t(1));
        QCOMPARE(notes.front().contents, QStringLiteral("Saved edit"));
    }

    // Selects moved Save outcomes and later requests in the original close scope.
    void completedSaveAfterMovingOutDoesNotBlockClose_data()
    {
        QTest::addColumn<bool>("returnToSource");
        QTest::addColumn<int>("outcome");
        QTest::newRow("Saved returns to closing window") << true << 0;
        QTest::newRow("Quit includes saved moved tab") << false << 0;
        QTest::newRow("Failed preserves remaining window close") << false << 1;
        QTest::newRow("Stopped preserves remaining window close") << false << 2;
    }

    // Checks a moved tab's completed Save cannot leave its former close flow waiting.
    void completedSaveAfterMovingOutDoesNotBlockClose()
    {
        QFETCH(bool, returnToSource);
        QFETCH(int, outcome);
        QTemporaryDir files;
        const QString path = copy(files, "saved.pdf");
        const QString other = copy(files, "held.pdf");
        const auto pending = std::make_shared<std::mutex>();
        testing::Stall<std::mutex> otherWork(*pending, 30000);
        WindowManager manager;
        const QPointer<MainWindow> source = manager.createWindow();
        const QPointer<MainWindow> target = manager.createWindow();
        const auto release = qScopeGuard([&] {
            otherWork.release();
            job::testing::release();
        });
        QVERIFY(testing::openAndWait(*source, path));
        const QPointer<TabPage> tab = currentTab(source);
        QVERIFY(addNote(tab, "Saved after moving") >= 0);
        QVERIFY(testing::openAndWait(*source, other));
        const QPointer<TabPage> otherTab = currentTab(source);
        const auto otherStarted = std::make_shared<std::atomic<bool>>(false);
        otherTab->startOperation({files.path(), [other, pending, otherStarted] {
            if (job::reading(other)) {
                *otherStarted = true;
                const std::lock_guard<std::mutex> lock(*pending);
            }
        }, {}});
        QTRY_VERIFY(otherStarted->load());
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Commit && file == path;
        });
        answers_ << QMessageBox::Save;
        QVERIFY(!source->close());
        QTRY_COMPARE(job::testing::held(), 1);
        source->whenTabsCanChange([&manager, source, target] {
            manager.mergeTab(source, 0, target, 0);
        });
        QTRY_VERIFY(tab && tab->window() == target && !tab->isClosing());
        QVERIFY(source && otherTab->isBusy() && otherTab->isClosing());
        if (outcome == 1) {
            job::testing::failWhen([path](job::Step step, const QString &file) {
                return step == job::Step::Commit && file == path;
            });
            answers_ << QMessageBox::Ok;
        } else if (outcome == 2) {
            tab->stopWaiting();
        }
        job::testing::release();
        QTRY_VERIFY(tab->isUsable() && tab->hasUnsavedEdits() == (outcome != 0));
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(source && otherTab->isClosing());
        if (outcome != 0) {
            otherWork.release();
            QTRY_VERIFY_WITH_TIMEOUT(!source, 1500);
            QVERIFY(target && tab && tab->hasUnsavedEdits());
            QCOMPARE(questions_.size(), outcome == 1 ? 2 : 1);
            return;
        }
        if (returnToSource) {
            target->whenTabsCanChange([&manager, source, target] {
                manager.mergeTab(target, 0, source, 0);
            });
            QTRY_VERIFY(tab && tab->window() == source);
        } else {
            manager.quitAll();
        }
        otherWork.release();
        QTRY_VERIFY_WITH_TIMEOUT(!source && !target, 1500);
        QCOMPARE(questions_.size(), 1);
        auto saved = manager.engine()->openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                 QStringLiteral("Saved after moving"));
    }

    // Checks Quit during pre-Save attention starts the inherited Save exactly once.
    void quitDuringSaveAttentionStartsTheChosenSave()
    {
        QTemporaryDir files;
        const QString path = copy(files, "saved.pdf");
        const QString other = copy(files, "other.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Save after attention") >= 0);
        QVERIFY(testing::openAndWait(*window, other));
        const QPointer<TabPage> otherTab = currentTab(window);
        auto *tabs = window->findChild<QTabWidget *>();
        bool quitStarted = false;
        const auto commits = std::make_shared<std::atomic<int>>(0);
        job::testing::holdWhen([path, commits](job::Step step, const QString &file) {
            if (step == job::Step::Commit && file == path)
                ++*commits;
            return false;
        });
        QMetaObject::Connection attentionObserver;
        const auto clearObservers = qScopeGuard([&] {
            beforeAnswer_ = {};
            disconnect(attentionObserver);
        });
        beforeAnswer_ = [&](QMessageBox *box) {
            connect(box, &QDialog::finished, this, [&, tab, otherTab] {
                tabs->setCurrentWidget(otherTab);
                attentionObserver = connect(tabs, &QTabWidget::currentChanged, this, [&, tab](int) {
                    if (!quitStarted && tab && currentTab(window) == tab && !tab->isBusy()) {
                        quitStarted = true;
                        manager.quitAll();
                    }
                });
            });
        };
        answers_ << QMessageBox::Save;
        closeTab(window);
        QTRY_VERIFY(quitStarted);
        QTRY_VERIFY_WITH_TIMEOUT(!window, 1500);
        QCOMPARE(questions_.size(), 1);
        QCOMPARE(commits->load(), 1);
        auto saved = manager.engine()->openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                 QStringLiteral("Save after attention"));
    }

    // Closing one held tab must not make another clean tab wait behind it.
    void closeWaitsAndIndependentTabCloses()
    {
        QTemporaryDir files;
        const QString first = copy(files, "first.pdf");
        const QString second = copy(files, "second.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, first));
        const QPointer<TabPage> tab = currentTab(window);
        holdOperation(tab, first);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(testing::openAndWait(*window, second));
        QElapsedTimer timer;
        timer.start();
        closeTab(window);
        QVERIFY(timer.elapsed() < 1000);
        QTRY_VERIFY(barShown(tab));
        QVERIFY(timer.elapsed() < 1000);
        QCOMPARE(window->tabCount(), 2);
        QVERIFY(tab->isBusy());
        closeTab(window, 1);
        QTRY_COMPARE(window->tabCount(), 1);
        QVERIFY(tab);
        job::testing::release();
        QTRY_VERIFY(!tab);
        QCOMPARE(window->tabCount(), 0);
    }

    // Checks that Stop resumes the close question while the abandoned worker remains held.
    void stopOperationContinuesDirtyQuestion()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Unsaved") >= 0);
        holdOperation(tab, path);
        QTRY_COMPARE(job::testing::held(), 1);
        answers_ << QMessageBox::Discard;
        closeTab(window);
        QTRY_VERIFY(barShown(tab));
        QVERIFY(questions_.isEmpty());
        tab->stopWaiting();
        QTRY_VERIFY(!tab);
        QCOMPARE(questions_, QStringList{QStringLiteral("Save changes to plan.pdf?")});
        QCOMPARE(job::testing::held(), 1);
    }

    // Supplies window-close and Quit variants for retaining an abandoned fresh tab.
    void canceledCloseKeepsAbandonedFreshTab_data()
    {
        QTest::addColumn<bool>("quit");
        QTest::newRow("Window close keeps fresh tab") << false;
        QTest::newRow("Quit keeps fresh tab") << true;
    }

    // Checks that Cancel keeps both an edited tab and an abandoned opening placeholder.
    void canceledCloseKeepsAbandonedFreshTab()
    {
        QFETCH(bool, quit);
        QTemporaryDir files;
        const QString edited = copy(files, "edited.pdf");
        const QString fresh = copy(files, "fresh.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, edited));
        const QPointer<TabPage> editedTab = currentTab(window);
        QVERIFY(addNote(editedTab, "Keep edits") >= 0);
        job::testing::holdWhen([fresh](job::Step step, const QString &where) {
            return step == job::Step::Reading && where == fresh;
        });
        // Resolve the fresh tab first, then Cancel the later dirty decision.
        QVERIFY(window->openFile(fresh, false, 0, false));
        auto *tabs = window->findChild<QTabWidget *>();
        const QPointer<TabPage> freshTab = qobject_cast<TabPage *>(tabs->widget(0));
        QVERIFY(freshTab && freshTab->freshOpen());
        QTRY_COMPARE(job::testing::held(), 1);
        answers_ << QMessageBox::Cancel;
        if (quit)
            manager.quitAll();
        else
            QVERIFY(!window->close());
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(editedTab && !editedTab->isClosing());
        QVERIFY(window && freshTab && !freshTab->isLoading());
        QCOMPARE(window->tabCount(), 2);
        QVERIFY(freshTab->freshOpen());
        QCOMPARE(job::testing::held(), 1);
        job::testing::release();
        freshTab->retryLoad();
        QTRY_VERIFY_WITH_TIMEOUT(freshTab && freshTab->isLoaded(), 10000);
        QVERIFY(!freshTab->freshOpen());
        QCOMPARE(questions_.size(), 1);
    }

    // Supplies completion and Stop variants for Save waiting on a recovery load.
    void recoverySaveWait_data()
    {
        QTest::addColumn<bool>("stop");
        QTest::newRow("release saves and closes") << false;
        QTest::newRow("Stop cancels close") << true;
    }

    // Checks close-time Save waits for retained recovery instead of blocking the GUI.
    void recoverySaveWait()
    {
        QFETCH(bool, stop);
        QTemporaryDir files;
        const QString path = copy(files, "recovered.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Recovered edit") >= 0);
        QVERIFY(mervin::testing::suspendAndWait(*tab));
        const QString recovery = tab->recoveryPath();
        QVERIFY(!recovery.isEmpty());
        job::testing::holdWhen([recovery, path, first = true](job::Step step, const QString &where) mutable {
            const bool load = step == job::Step::Reading && where == recovery && first;
            if (load)
                first = false;
            return load || (step == job::Step::Commit && where == path);
        });
        tab->resumeAsync();
        QTRY_COMPARE(job::testing::held(), 1);
        answers_ << QMessageBox::Save;
        QElapsedTimer timer;
        timer.start();
        closeTab(window);
        QVERIFY(timer.elapsed() < 1000);
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_COMPARE(job::testing::held(), 2);
        QVERIFY(tab && tab->isBusy() && !tab->isLoading());
        auto *button = tab->findChild<QPushButton *>(QStringLiteral("waitBarStop"));
        QVERIFY(button && !button->isHidden());
        if (stop) {
            button->click();
            QTRY_VERIFY(!tab->isClosing());
            QVERIFY(!tab->isLoading());
            QVERIFY(tab->hasUnsavedEdits());
            QCOMPARE(tab->recoveryPath(), recovery);
            QVERIFY(QFileInfo::exists(recovery));
        }
        job::testing::release();
        if (stop) {
            tab->retryLoad();
            QTRY_VERIFY_WITH_TIMEOUT(tab->isLoaded(), 10000);
            QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).front().contents,
                     QStringLiteral("Recovered edit"));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!tab, 10000);
            auto saved = manager.engine()->openDocument(path);
            QVERIFY(saved);
            QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                     QStringLiteral("Recovered edit"));
        }
    }

    // Checks guarded ownership when abandoning a recovery load destroys its standalone window.
    void abandoningRecoveryLoadCanDestroyStandaloneOwner()
    {
        QTemporaryDir files;
        const QString path = copy(files, "recovered.pdf");
        RenderEngine engine;
        const QPointer<MainWindow> window = new MainWindow(&engine, nullptr);
        window->show();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Kept") >= 0);
        QVERIFY(mervin::testing::suspendAndWait(*tab));
        const QString recovery = tab->recoveryPath();
        job::testing::holdWhen([recovery](job::Step step, const QString &where) {
            return step == job::Step::Reading && where == recovery;
        });
        tab->resumeAsync();
        QTRY_COMPARE(job::testing::held(), 1);
        connect(tab, &TabPage::stateChanged, this, [window, tab] {
            if (tab && !tab->isLoading() && !tab->isLoaded())
                delete window.data();
        });
        closeTab(window);
        QTRY_VERIFY(!window && !tab);
    }

    // Supplies tab destruction and Quit observers for the recovery retry signal.
    void recoverySaveRetryObserver_data()
    {
        QTest::addColumn<bool>("deleteTab");
        QTest::newRow("Save observer deletes tab") << true;
        QTest::newRow("Quit preserves chosen Save") << false;
    }

    // Checks that recovery retry tolerates destruction or a broader Quit during state delivery.
    void recoverySaveRetryObserver()
    {
        QFETCH(bool, deleteTab);
        QTemporaryDir files;
        const QString path = copy(files, "recovered.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Retry edit") >= 0);
        QVERIFY(mervin::testing::suspendAndWait(*tab));
        const QString recovery = tab->recoveryPath();
        job::testing::holdWhen([recovery, path, first = true](job::Step step, const QString &where) mutable {
            const bool load = step == job::Step::Reading && where == recovery && first;
            if (load)
                first = false;
            return load || (step == job::Step::Commit && where == path);
        });
        tab->resumeAsync();
        QTRY_COMPARE(job::testing::held(), 1);
        bool observed = false;
        beforeAnswer_ = [&](QMessageBox *) {
            connect(tab, &TabPage::stateChanged, this, [&, tab] {
                if (!observed && tab && tab->isBusy()) {
                    observed = true;
                    if (deleteTab)
                        delete tab.data();
                    else
                        manager.quitAll();
                }
            });
        };
        answers_ << QMessageBox::Save;
        closeTab(window);
        QTRY_VERIFY(observed);
        if (deleteTab) {
            QTRY_VERIFY(!tab);
            QCOMPARE(window->tabCount(), 0);
        } else {
            QTRY_COMPARE(job::testing::held(), 2);
            QVERIFY(window && tab && tab->isBusy() && tab->isClosing());
        }
        job::testing::release();
        if (!deleteTab) {
            QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
            auto saved = manager.engine()->openDocument(path);
            QVERIFY(saved);
            QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                     QStringLiteral("Retry edit"));
        }
        QCOMPARE(questions_.size(), 1);
    }

#ifdef Q_OS_UNIX
    // A real kernel-blocked recovery open exercises the same continuation as
    // the portable held-step tests. Stop must preserve the edited snapshot.
    void recoverySaveStopWithNamedPipe()
    {
        QTemporaryDir files;
        const QString path = copy(files, "recovered.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Pipe recovery") >= 0);
        QVERIFY(mervin::testing::suspendAndWait(*tab));
        const QString recovery = tab->recoveryPath();
        QFile snapshot(recovery);
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QByteArray bytes = snapshot.readAll();
        snapshot.close();
        QVERIFY(QFile::remove(recovery));
        QCOMPARE(::mkfifo(QFile::encodeName(recovery).constData(), 0600), 0);
        testing::PipeWriter writer(recovery);
        QVERIFY(appfiles::flush(10000));
        QVERIFY(Document::waitForCloses(10000));
        // A live engine's render workers stay active. Wait for this load to end
        // against that baseline, rather than requiring every worker to exit.
        const int tasks = background::running();
        const auto release = qScopeGuard([&] {
            writer.release();
            (void)QTest::qWaitFor([tasks] { return background::running() <= tasks; }, 10000);
        });
        const auto reading = std::make_shared<std::atomic<bool>>(false);
        job::testing::holdWhen([reading, recovery](job::Step step, const QString &where) {
            if (step == job::Step::Reading && where == recovery)
                reading->store(true);
            return false;
        });
        tab->resumeAsync();
        QTRY_VERIFY(reading->load());
        answers_ << QMessageBox::Save;
        QElapsedTimer timer;
        timer.start();
        closeTab(window);
        QTRY_VERIFY(answers_.isEmpty());
        QVERIFY(timer.elapsed() < 1000);
        QTRY_VERIFY(tab->isBusy() && !tab->isLoading());
        tab->stopWaiting();
        QTRY_VERIFY(!tab->isClosing());
        QVERIFY(tab->hasUnsavedEdits());
        QCOMPARE(tab->recoveryPath(), recovery);
        writer.release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
        QVERIFY(QFile::remove(recovery));
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        QCOMPARE(snapshot.write(bytes), qint64(bytes.size()));
        snapshot.close();
        tab->retryLoad();
        QTRY_VERIFY_WITH_TIMEOUT(tab->isLoaded(), 10000);
        QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).front().contents,
                 QStringLiteral("Pipe recovery"));
    }
#endif

    // Checks that a later Cancel preserves all windows and earlier unsaved edits.
    void canceledQuitPreservesEarlierDecisions()
    {
        QTemporaryDir files;
        const QString first = copy(files, "first.pdf");
        const QString held = copy(files, "held.pdf");
        const QString later = copy(files, "later.pdf");
        WindowManager manager;
        const QPointer<MainWindow> firstWindow = manager.createWindow();
        const QPointer<MainWindow> secondWindow = manager.createWindow();
        QVERIFY(testing::openAndWait(*firstWindow, first));
        const QPointer<TabPage> firstTab = currentTab(firstWindow);
        QVERIFY(addNote(firstTab, "First edit") >= 0);
        QVERIFY(testing::openAndWait(*secondWindow, held));
        const QPointer<TabPage> heldTab = currentTab(secondWindow);
        holdOperation(heldTab, held);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(testing::openAndWait(*secondWindow, later));
        QVERIFY(addNote(currentTab(secondWindow), "Later edit") >= 0);
        answers_ << QMessageBox::Discard << QMessageBox::Cancel;
        manager.closeAllForQuit();
        QTRY_VERIFY(barShown(heldTab));
        QCOMPARE(firstWindow->tabCount(), 1);
        QCOMPARE(secondWindow->tabCount(), 2);
        job::testing::release();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!firstTab->isClosing());
        QVERIFY(firstWindow && secondWindow);
        QCOMPARE(manager.openTabPaths().size(), 3);
        QVERIFY(firstTab->hasUnsavedEdits());
        QVERIFY(!relaunch::requested());
    }

    // Supplies tab, window and process scopes for resuming after canceled close.
    void canceledCloseResumesSelectedSuspendedTabs_data()
    {
        QTest::addColumn<QString>("scope");
        QTest::newRow("tab") << QStringLiteral("tab");
        QTest::newRow("window") << QStringLiteral("window");
        QTest::newRow("quit") << QStringLiteral("quit");
        QTest::newRow("quit across two windows") << QStringLiteral("multiwindow");
    }

    // Checks that Cancel resumes selected recovery documents even when their originals are gone.
    void canceledCloseResumesSelectedSuspendedTabs()
    {
        QFETCH(QString, scope);
        QTemporaryDir files;
        WindowManager manager;
        QList<QPointer<MainWindow>> windows;
        QList<QPointer<TabPage>> edited;
        const int count = scope == QStringLiteral("multiwindow") ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            MainWindow *window = manager.createWindow();
            windows.append(window);
            const QString path = copy(files, i ? "second.pdf" : "first.pdf");
            QVERIFY(testing::openAndWait(*window, path));
            const QPointer<TabPage> tab = currentTab(window);
            edited.append(tab);
            QVERIFY(addNote(tab, i ? "Second retained edit" : "First retained edit") >= 0);
            QVERIFY(testing::openAndWait(*window, copy(files, i ? "second-local.pdf" : "first-local.pdf")));
            QVERIFY(tab->suspend());
            QTRY_VERIFY(tab->isSuspended() && tab->hasUnsavedEdits());
            // Resume must use the recovery copy and preserve its edits.
            QVERIFY(QFile::remove(path));
        }
        beforeAnswer_ = [&](QMessageBox *box) {
            auto *window = qobject_cast<MainWindow *>(box->parentWidget());
            QVERIFY(window);
            const QPointer<TabPage> tab = currentTab(window);
            QVERIFY(edited.contains(tab));
            QVERIFY(tab->isClosing() && tab->isSuspended());
        };
        if (count == 2)
            answers_.append(QMessageBox::Discard);
        answers_.append(QMessageBox::Cancel);
        if (scope == QStringLiteral("tab"))
            closeTab(windows.front(), 0);
        else if (scope == QStringLiteral("window"))
            QVERIFY(!windows.front()->close());
        else
            manager.quitAll();
        QTRY_VERIFY(answers_.isEmpty());
        for (int i = 0; i < count; ++i) {
            const QPointer<TabPage> tab = edited[i];
            QTRY_VERIFY(tab && !tab->isClosing());
            QVERIFY(windows[i]);
            QCOMPARE(currentTab(windows[i]), tab.data());
            QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 5000);
            QVERIFY(tab->hasUnsavedEdits());
            QCOMPARE(tab->viewer()->annotModel()->pageAnnots(0).front().contents,
                     i ? QStringLiteral("Second retained edit") : QStringLiteral("First retained edit"));
            QCOMPARE(windows[i]->tabCount(), 2);
        }
        QCOMPARE(questions_.size(), count);
        QVERIFY(!relaunch::requested());
    }

    // Supplies committed and still-open editor variants for edits made after Don't save.
    void editedAfterDontSaveIsAskedAgain_data()
    {
        QTest::addColumn<bool>("inlineEditor");
        QTest::newRow("committed annotation") << false;
        QTest::newRow("uncommitted inline editor") << true;
    }

    // Checks that later edits invalidate an earlier Don't save decision before Quit.
    void editedAfterDontSaveIsAskedAgain()
    {
        QFETCH(bool, inlineEditor);
        QTemporaryDir files;
        const QString first = copy(files, "edited.pdf");
        const QString held = copy(files, "held.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, first));
        const QPointer<TabPage> firstTab = currentTab(window);
        const int note = addNote(firstTab, "Before decision");
        QVERIFY(note >= 0);
        QVERIFY(testing::openAndWait(*window, held));
        const QPointer<TabPage> heldTab = currentTab(window);
        holdOperation(heldTab, held);
        QTRY_COMPARE(job::testing::held(), 1);
        answers_ << QMessageBox::Discard;
        manager.closeAllForQuit();
        QTRY_VERIFY(barShown(heldTab));
        QCOMPARE(questions_.size(), 1);
        const quint64 generation = firstTab->editGeneration();
        window->findChild<QTabWidget *>()->setCurrentWidget(firstTab);
        firstTab->viewer()->setCommentToolEnabled(true);
        firstTab->viewer()->setAnnotSubMode(AnnotSubMode::Note);
        firstTab->viewer()->revealAnnotation(0, note);
        auto *editor = firstTab->viewer()->findChild<QPlainTextEdit *>();
        QVERIFY(editor);
        QVERIFY(editor->isVisible() && !editor->isReadOnly());
        editor->setPlainText(QStringLiteral("Typed after decision"));
        if (inlineEditor) {
            QCOMPARE(firstTab->editGeneration(), generation);
        } else {
            firstTab->viewer()->commitActiveAnnotEditor();
            QVERIFY(firstTab->editGeneration() > generation);
        }
        answers_ << QMessageBox::Cancel;
        job::testing::release();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!firstTab->isClosing());
        QCOMPARE(questions_, (QStringList{QStringLiteral("Save changes to edited.pdf?"),
                                         QStringLiteral("Save changes to edited.pdf?")}));
        QCOMPARE(window->tabCount(), 2);
    }

    // Supplies close scopes that either include or preserve newly arrived tabs.
    void arrivingTabsFollowIntent_data()
    {
        QTest::addColumn<bool>("windowClose");
        QTest::newRow("Close all tabs leaves arrival") << false;
        QTest::newRow("Window close includes arrival") << true;
    }

    // Checks whether a newly opened tab joins the pending close according to its scope.
    void arrivingTabsFollowIntent()
    {
        QFETCH(bool, windowClose);
        QTemporaryDir files;
        const QString held = copy(files, "held.pdf");
        const QString arrived = copy(files, "arrived.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, held));
        const QPointer<TabPage> heldTab = currentTab(window);
        holdOperation(heldTab, held);
        QTRY_COMPARE(job::testing::held(), 1);
        if (windowClose)
            QVERIFY(!window->close());
        else
            QVERIFY(QMetaObject::invokeMethod(window, "closeAllTabs"));
        QTRY_VERIFY(barShown(heldTab));
        QVERIFY(testing::openAndWait(*window, arrived));
        const QPointer<TabPage> arrivedTab = currentTab(window);
        QVERIFY(addNote(arrivedTab, "Arrived") >= 0);
        if (windowClose)
            answers_ << QMessageBox::Cancel;
        job::testing::release();
        if (windowClose) {
            QTRY_VERIFY(answers_.isEmpty());
            QTRY_VERIFY(!arrivedTab->isClosing());
            QVERIFY(window && heldTab);
            QCOMPARE(window->tabCount(), 2);
        } else {
            QTRY_COMPARE(window->tabCount(), 1);
            QVERIFY(!heldTab);
            QVERIFY(arrivedTab);
            QVERIFY(questions_.isEmpty());
        }
    }

    // Supplies close scopes for moving a waiting tab to another window.
    void movedTabFollowsIntent_data()
    {
        QTest::addColumn<bool>("allTabs");
        QTest::newRow("Single close follows identity") << false;
        QTest::newRow("Close all skips moved-out tab") << true;
    }

    // Checks whether a waiting tab remains covered after its owner changes.
    void movedTabFollowsIntent()
    {
        QFETCH(bool, allTabs);
        QTemporaryDir files;
        const QString held = copy(files, "held.pdf");
        WindowManager manager;
        MainWindow *source = manager.createWindow();
        MainWindow *target = manager.createWindow();
        QVERIFY(testing::openAndWait(*source, held));
        const QPointer<TabPage> tab = currentTab(source);
        holdOperation(tab, held);
        QTRY_COMPARE(job::testing::held(), 1);
        if (allTabs)
            QVERIFY(QMetaObject::invokeMethod(source, "closeAllTabs"));
        else
            closeTab(source);
        QTRY_VERIFY(barShown(tab));
        source->whenTabsCanChange([source, target] { target->adoptTab(source->releaseTab(0)); });
        QTRY_COMPARE(target->tabCount(), 1);
        job::testing::release();
        if (allTabs) {
            QTRY_VERIFY(!tab->isClosing());
            QCOMPARE(target->tabCount(), 1);
        } else {
            QTRY_VERIFY(!tab);
            QCOMPARE(target->tabCount(), 0);
        }
    }

    // Supplies tab and window destruction during attention transfer to a moved tab.
    void movedOwnerAttentionCanDestroyWaitingObjects_data()
    {
        QTest::addColumn<bool>("deleteTab");
        QTest::newRow("currentChanged deletes waiting tab") << true;
        QTest::newRow("currentChanged deletes standalone flow owner") << false;
    }

    // Checks that selecting a moved waiting tab tolerates destruction from its signals.
    void movedOwnerAttentionCanDestroyWaitingObjects()
    {
        QFETCH(bool, deleteTab);
        QTemporaryDir files;
        const QString first = copy(files, "held.pdf");
        const QString second = copy(files, "other.pdf");
        RenderEngine engine;
        const QPointer<MainWindow> source = new MainWindow(&engine, nullptr);
        const QPointer<MainWindow> target = new MainWindow(&engine, nullptr);
        const auto cleanup = qScopeGuard([source, target] {
            delete source.data();
            delete target.data();
        });
        source->show();
        target->show();
        QVERIFY(testing::openAndWait(*source, first));
        const QPointer<TabPage> heldTab = currentTab(source);
        QVERIFY(testing::openAndWait(*target, second));
        const QPointer<TabPage> otherTab = currentTab(target);
        holdOperation(heldTab, first);
        QTRY_COMPARE(job::testing::held(), 1);
        closeTab(source);
        QTRY_VERIFY(barShown(heldTab));
        source->whenTabsCanChange([this, source, target, heldTab, otherTab, deleteTab] {
            target->adoptTab(source->releaseTab(0));
            auto *tabs = target->findChild<QTabWidget *>();
            tabs->setCurrentWidget(otherTab);
            connect(tabs, &QTabWidget::currentChanged, this,
                    [source, target, heldTab, deleteTab](int) {
                if (heldTab && currentTab(target) == heldTab) {
                    if (deleteTab)
                        delete heldTab.data();
                    else
                        delete source.data();
                }
            });
        });
        if (deleteTab) {
            QTRY_VERIFY(!heldTab);
            QCOMPARE(target->tabCount(), 1);
        } else {
            QTRY_VERIFY(!source);
            QVERIFY(heldTab && heldTab->isBusy() && !heldTab->isClosing());
            QCOMPARE(target->tabCount(), 2);
        }
        QVERIFY(otherTab && otherTab->isLoaded());
        QCOMPARE(job::testing::held(), 1);
    }

    // Checks that broader requests replace narrow flows and repeated closes share one decision.
    void broaderCloseSupersedesAndRepeatedCloseCoalesces()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Kept") >= 0);
        holdOperation(tab, path);
        QTRY_COMPARE(job::testing::held(), 1);
        closeTab(window);
        closeTab(window);
        QTRY_VERIFY(barShown(tab));
        QVERIFY(!window->close());
        QVERIFY(!window->close());
        QVERIFY(tab->isBusy());
        answers_ << QMessageBox::Cancel;
        job::testing::release();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!tab->isClosing());
        QCOMPARE(questions_.size(), 1);
        QCOMPARE(window->tabCount(), 1);
    }

    // Checks that a question's Cancel transfers to the Quit that supersedes its original flow.
    void quitSupersedesOpenQuestionAndKeepsCancel()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        const QPointer<MainWindow> other = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Kept") >= 0);
        bool started = false;
        beforeAnswer_ = [&](QMessageBox *) {
            if (!started) {
                started = true;
                manager.quitAll();
            }
        };
        answers_ << QMessageBox::Cancel;
        closeTab(window);
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(tab && !tab->isClosing());
        QVERIFY(window && other);
        QCOMPARE(questions_.size(), 1);
    }

    // Checks that a tab moved out of a closing window no longer holds that window open.
    void movedOutTabDoesNotDelayWindowClose()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> source = manager.createWindow();
        MainWindow *target = manager.createWindow();
        QVERIFY(testing::openAndWait(*source, path));
        const QPointer<TabPage> tab = currentTab(source);
        holdOperation(tab, path);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(!source->close());
        QTRY_VERIFY(barShown(tab));
        source->whenTabsCanChange([source, target] { target->adoptTab(source->releaseTab(0)); });
        QTRY_COMPARE(target->tabCount(), 1);
        QTRY_VERIFY(!source);
        QVERIFY(tab && tab->isBusy());
        QCOMPARE(job::testing::held(), 1);
        QVERIFY(!tab->isClosing());
    }

    // Checks that discarding a former owner's queue still closes a moved tab in its new window.
    void queuedCloseSurvivesOldOwnerDestruction()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> source = manager.createWindow();
        MainWindow *target = manager.createWindow();
        QVERIFY(testing::openAndWait(*source, path));
        const QPointer<TabPage> tab = currentTab(source);
        QDialog modal;
        modal.setWindowModality(Qt::ApplicationModal);
        modal.show();
        closeTab(source);
        QVERIFY(tab->isClosing());
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QVERIFY(!source->canChangeTabs());
        // Direct movement and destruction exercise a queue owner disappearing,
        // rather than the normal move path that obeys the modal guard.
        target->adoptTab(source->releaseTab(0));
        delete source.data();
        modal.hide();
        QTRY_VERIFY(!tab);
        QCOMPARE(target->tabCount(), 0);
    }

    // Checks that Quit includes a window created before its final approval.
    void quitIncludesNewWindowBeforeApproval()
    {
        QTemporaryDir files;
        const QString path = copy(files, "held.pdf");
        const QString arrived = copy(files, "arrived.pdf");
        WindowManager manager;
        const QPointer<MainWindow> first = manager.createWindow();
        QVERIFY(testing::openAndWait(*first, path));
        const QPointer<TabPage> tab = currentTab(first);
        holdOperation(tab, path);
        QTRY_COMPARE(job::testing::held(), 1);
        manager.closeAllForQuit();
        QTRY_VERIFY(barShown(tab));
        const QPointer<MainWindow> newcomer = manager.createWindow();
        QVERIFY(testing::openAndWait(*newcomer, arrived));
        QVERIFY(addNote(currentTab(newcomer), "New window") >= 0);
        answers_ << QMessageBox::Cancel;
        job::testing::release();
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!tab->isClosing());
        QVERIFY(first && newcomer);
        QCOMPARE(manager.openTabPaths().size(), 2);
        QCOMPARE(questions_, QStringList{QStringLiteral("Save changes to arrived.pdf?")});
    }

    // Checks that window close also resolves a tab inserted while its question runs.
    void tabArrivingDuringQuestionIsIncluded()
    {
        QTemporaryDir files;
        const QString first = copy(files, "first.pdf");
        const QString arrived = copy(files, "arrived.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        MainWindow *source = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, first));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Initial edit") >= 0);
        QVERIFY(testing::openAndWait(*source, arrived));
        QVERIFY(addNote(currentTab(source), "Arrived edit") >= 0);
        bool moved = false;
        beforeAnswer_ = [&](QMessageBox *) {
            if (!moved) {
                moved = true;
                window->adoptTab(source->releaseTab(0));
            }
        };
        answers_ << QMessageBox::Discard << QMessageBox::Cancel;
        QVERIFY(!window->close());
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!tab->isClosing());
        QVERIFY(window);
        QCOMPARE(window->tabCount(), 2);
        QCOMPARE(questions_, (QStringList{QStringLiteral("Save changes to first.pdf?"),
                                         QStringLiteral("Save changes to arrived.pdf?")}));
    }

    // Checks that updater refusal returns promptly and never closes windows after work finishes.
    void updaterDeclinesHeldOperationWithoutLaterClose()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        holdOperation(tab, path);
        QTRY_COMPARE(job::testing::held(), 1);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!manager.tryCloseAllForUpdate());
        QVERIFY(timer.elapsed() < 1000);
        QVERIFY(!tab->isClosing());
        job::testing::release();
        QTRY_VERIFY(tab->isUsable());
        QVERIFY(window);
        QCOMPARE(window->tabCount(), 1);
        QVERIFY(questions_.isEmpty());
    }

    // Supplies loaded and recovery-backed documents for updater-requested Save.
    void updaterQueuesSaveAndKeepsWindows_data()
    {
        QTest::addColumn<bool>("recovery");
        QTest::newRow("loaded Save starts after callback") << false;
        QTest::newRow("recovery Save copies after callback") << true;
    }

    // Checks that updater-requested Save runs later while its refusal keeps the windows open.
    void updaterQueuesSaveAndKeepsWindows()
    {
        QFETCH(bool, recovery);
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Chosen Save") >= 0);
        if (recovery) {
            QVERIFY(mervin::testing::suspendAndWait(*tab));
            // Keep the suspended tab in the background, so its independent
            // automatic current-tab resume cannot start inside the question.
            QVERIFY(testing::openAndWait(*window, copy(files, "other.pdf")));
            QVERIFY(!tab->isLoaded() && !tab->isLoading());
            const QString snapshot = tab->recoveryPath();
            QVERIFY(!snapshot.isEmpty());
            job::testing::holdWhen([snapshot](job::Step step, const QString &where) {
                return step == job::Step::Reading && where == snapshot;
            });
        }
        answers_ << QMessageBox::Save;
        QVERIFY(!manager.tryCloseAllForUpdate());
        // The adapter must return before any save or recovery read starts.
        QVERIFY(tab->hasUnsavedEdits());
        QVERIFY(!tab->isLoading());
        QCOMPARE(job::testing::held(), 0);
        if (recovery) {
            QTRY_COMPARE(job::testing::held(), 1);
            QVERIFY(tab->isBusy() && !tab->isLoading() && window && window->tabCount() == 2);
            job::testing::release();
        }
        QTRY_VERIFY(!tab->hasUnsavedEdits());
        QTRY_VERIFY(!tab->isClosing());
        QVERIFY(window && tab);
        QCOMPARE(window->tabCount(), recovery ? 2 : 1);
        auto saved = manager.engine()->openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents,
                 QStringLiteral("Chosen Save"));
    }

    // Checks that updater refusal still completes a Save chosen before another tab's Cancel.
    void updaterKeepsSaveChosenBeforeLaterCancel()
    {
        QTemporaryDir files;
        const QString first = copy(files, "first.pdf");
        const QString second = copy(files, "second.pdf");
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, first));
        const QPointer<TabPage> firstTab = currentTab(window);
        QVERIFY(addNote(firstTab, "Chosen Save") >= 0);
        QVERIFY(testing::openAndWait(*window, second));
        const QPointer<TabPage> secondTab = currentTab(window);
        QVERIFY(addNote(secondTab, "Kept edit") >= 0);
        answers_ << QMessageBox::Save << QMessageBox::Cancel;
        QVERIFY(!manager.tryCloseAllForUpdate());
        QVERIFY(firstTab->hasUnsavedEdits());
        QTRY_VERIFY(!firstTab->hasUnsavedEdits());
        QVERIFY(secondTab->hasUnsavedEdits());
        QCOMPARE(window->tabCount(), 2);
    }

    // Checks that a restart raised inside an updater question prevents update shutdown.
    void updaterDeclinesRestartStartedInQuestion()
    {
        QTemporaryDir files;
        const QString path = copy(files, "plan.pdf");
        WindowManager manager;
        const QPointer<MainWindow> window = manager.createWindow();
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = currentTab(window);
        QVERIFY(addNote(tab, "Pending") >= 0);
        bool started = false;
        beforeAnswer_ = [&](QMessageBox *) {
            if (!started) {
                started = true;
                manager.restart();
            }
        };
        answers_ << QMessageBox::Discard << QMessageBox::Cancel;
        QVERIFY(!manager.tryCloseAllForUpdate());
        QVERIFY(started && window && tab);
        QVERIFY(!relaunch::requested());
        QTRY_VERIFY(answers_.isEmpty());
        QTRY_VERIFY(!tab->isClosing());
        QVERIFY(window && !relaunch::requested());
    }

private:
    // Creates a writable fixture PDF for a test's document path.
    QString copy(const QTemporaryDir &files, const char *name)
    {
        const QString path = files.filePath(QString::fromLatin1(name));
        if (!QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path))
            return {};
        return path;
    }

    // Adds an annotation so the tab has a concrete unsaved edit.
    int addNote(TabPage *tab, const char *text)
    {
        return tab->viewer()->annotModel()->addTextNote(
            0, {40, 40}, Qt::yellow, {}, QString::fromUtf8(text));
    }

    std::unique_ptr<QTemporaryDir> profile_;
    QTimer dialogs_;
    QList<QMessageBox::StandardButton> answers_;
    QStringList questions_;
    QStringList unexpected_;
    std::function<void(QMessageBox *)> beforeAnswer_;
};

QTEST_MAIN(TstCloseFlow)
#include "tst_close_flow.moc"
