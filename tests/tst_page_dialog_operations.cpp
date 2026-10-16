#include "TestWaits.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/ExtractDialog.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "io/SaveCheck.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>

using namespace mervin;

namespace {
/// Finds the current tab through the real window's tab widget.
TabPage *current(MainWindow &window)
{
    return qobject_cast<TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
}

/// Realizes the main window while keeping it off the desktop.
void showWindow(MainWindow &window)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1000, 800);
    window.show();
    QCoreApplication::processEvents();
}

/// Sets field text and emits textEdited so production validation runs.
void edit(QLineEdit *field, const QString &text)
{
    field->setText(text);
    QMetaObject::invokeMethod(field, "textEdited", Q_ARG(QString, text));
}

/// Sets page ranges, destination and result-opening choices in the real editor.
void choose(ExtractDialog &dialog, const QString &output, const QString &ranges = QStringLiteral("1,3"),
            bool open = false)
{
    auto *range = dialog.findChild<QLineEdit *>(QStringLiteral("extractRowSpec"));
    QVERIFY(range);
    range->setText(ranges);
    QTest::qWait(1); // the separator splits the rows on the next event turn
    edit(dialog.findChild<QLineEdit *>(QStringLiteral("extractOutput")), output);
    dialog.findChild<QCheckBox *>(QStringLiteral("extractOpenWhenDone"))->setChecked(open);
    const auto *check = dialog.findChild<SaveCheck *>();
    QVERIFY(QTest::qWaitFor([check] { return !check->pending(); }, 5000));
    auto *accept = dialog.findChild<QPushButton *>(QStringLiteral("extractAccept"));
    QVERIFY(accept->isEnabled());
    accept->click();
}

/// Reads the visible failure reason from the real dialog.
QString error(ExtractDialog &dialog)
{
    return dialog.findChild<QLabel *>(QStringLiteral("extractError"))->text();
}

/// Blocks one matching worker step for deterministic cancellation checks.
void hold(job::Step step, const QString &path)
{
    job::testing::holdWhen([step, path](job::Step reported, const QString &where) {
        return reported == step && where == path;
    });
}
} // namespace

/// Checks page-tool worker ownership, retained choices and result delivery.
class TstPageDialogOperations : public QObject
{
    Q_OBJECT

public:
    /// Answers each nested editor once while recording unexpected dialogs.
    TstPageDialogOperations()
    {
        connect(&dialogs_, &QTimer::timeout, this, [this] {
            // A result that should open with the tab's password must not ask for one.
            if (auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                unexpected_ << prompt->windowTitle();
                prompt->reject();
                return;
            }
            auto *dialog = qobject_cast<ExtractDialog *>(QApplication::activeModalWidget());
            if (!dialog || answered_ == dialog)
                return;
            answered_ = dialog; // editing and acceptance can emit nested events
            if (choices_.isEmpty()) {
                unexpected_ << dialog->windowTitle();
                dialog->reject();
                return;
            }
            ++shown_;
            choices_.takeFirst()(*dialog);
            if (QTest::currentTestFailed() && answered_)
                answered_->reject();
        });
    }

private slots:
    /// Creates an isolated profile and resets the dialog-driving state.
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        ConfigPaths::setOverrideDir(profile_->path());
        Settings settings;
        settings.autoUpdate = false;
        settings.closeToTray = false;
        settings.unloadInactiveMinutes = 0;
        QVERIFY(settings.save());
        choices_.clear();
        unexpected_.clear();
        answered_.clear();
        shown_ = 0;
        dialogs_.start(10);
    }

    /// Drains worker cleanup and rejects leftover or unexpected dialog interactions.
    void cleanup()
    {
        job::testing::failWhen({});
        job::testing::release();
        QVERIFY(background::settle(10000));
        QVERIFY(Document::waitForCloses(10000));
        dialogs_.stop();
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
        QVERIFY2(unexpected_.isEmpty(), qPrintable(unexpected_.join(u'\n')));
        QVERIFY(choices_.isEmpty());
    }

    /// Keeps a stalled source probe responsive and suppresses abandoned UI delivery.
    void heldProbeReturnsAndStopSuppressesTheDialog()
    {
        QTemporaryDir files;
        const QString path = copy(files);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        TabPage *tab = current(window);
        const int tasks = background::running();
        hold(job::Step::Reading, path);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QVERIFY(elapsed.elapsed() < 1000);
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab->isBusy());
        QCOMPARE(job::testing::stepsOnMainThread(), 0);
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
        QCoreApplication::processEvents();
        QCOMPARE(shown_, 0);
    }

    /// Covers asynchronous acceptance for both Extract and Delete.
    void acceptedDialogClosesBeforeTheWrite_data()
    {
        QTest::addColumn<bool>("deleting");
        QTest::newRow("extract") << false;
        QTest::newRow("delete") << true;
    }

    /// Closes the editor before stalled output work and preserves the destination on Stop.
    void acceptedDialogClosesBeforeTheWrite()
    {
        QFETCH(bool, deleting);
        QTemporaryDir files;
        const QString path = copy(files);
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        TabPage *tab = current(window);
        hold(job::Step::Commit, out);
        choices_ << [out](ExtractDialog &dialog) { choose(dialog, out); };
        QVERIFY(QMetaObject::invokeMethod(&window, deleting ? "deletePagesOp" : "extractPagesOp"));
        QTRY_COMPARE(job::testing::held(), 1);
        QVERIFY(tab->isBusy());
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(job::testing::stepsOnMainThread(), 0);
        const QString other = copy(files, QStringLiteral("other.pdf"));
        QVERIFY(testing::openAndWait(window, other));
        QVERIFY(current(window)->viewer()->annotModel()->addTextNote(
                    0, {50, 50}, Qt::yellow, {}, QStringLiteral("Kept")) >= 0);
        const int tasks = background::running() - 1; // excludes the held page job
        tab->stopWaiting();
        QVERIFY(tab->isUsable());
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
        QVERIFY(!QFileInfo::exists(out));
        QCOMPARE(shown_, 1);
    }

    /// Retains edited ranges and destination when a failed output is retried.
    void failedWriteReopensWithTheSameChoices()
    {
        QTemporaryDir files;
        const QString path = copy(files);
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        job::testing::failWhen([out](job::Step step, const QString &where) {
            return step == job::Step::Commit && where == out;
        });
        choices_ << [out](ExtractDialog &dialog) { choose(dialog, out); };
        choices_ << [out](ExtractDialog &dialog) {
            const auto chosen = dialog.choices();
            QCOMPARE(chosen.specs, QStringList({QStringLiteral("1"), QStringLiteral("3")}));
            QCOMPARE(chosen.output, out);
            QVERIFY(chosen.outputEdited);
            QVERIFY(!chosen.openWhenDone);
            QVERIFY(error(dialog).contains(QStringLiteral("Could not write")));
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QTRY_COMPARE(shown_, 2);
        QVERIFY(current(window)->isUsable());
        QVERIFY(!QFileInfo::exists(out));
    }

    /// Requires new acceptance when the source count changes after the initial probe.
    void changedPageCountReopensBeforeWriting()
    {
        QTemporaryDir files;
        const QString path = copy(files);
        const QString changed = files.filePath(QStringLiteral("changed.pdf"));
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        QCOMPARE(PageOps::deletePages(path, changed, {2}), PageOps::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        choices_ << [path, changed, out](ExtractDialog &dialog) {
            QVERIFY(QFile::remove(path));
            QVERIFY(QFile::copy(changed, path));
            choose(dialog, out, QStringLiteral("1"));
        };
        choices_ << [](ExtractDialog &dialog) {
            QCOMPARE(dialog.choices().specs, QStringList({QStringLiteral("1")}));
            QCOMPARE(error(dialog), QStringLiteral("The page count changed. Check the ranges and try again."));
            QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("extractOf"))->text(), QStringLiteral("of 3"));
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QTRY_COMPARE(shown_, 2);
        QVERIFY(!QFileInfo::exists(out));
    }

    /// Routes a delayed source probe into the tab's new window.
    void probeCompletionFollowsTheCurrentOwner()
    {
        QTemporaryDir files;
        const QString path = copy(files);
        WindowManager manager;
        MainWindow *first = manager.createWindow();
        MainWindow *second = manager.createWindow();
        showWindow(*first);
        showWindow(*second);
        QVERIFY(testing::openAndWait(*first, path));
        TabPage *tab = current(*first);
        hold(job::Step::Reading, path);
        choices_ << [second](ExtractDialog &dialog) {
            QCOMPARE(dialog.parentWidget(), second);
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(first, "extractPagesOp"));
        QTRY_COMPARE(job::testing::held(), 1);
        QCOMPARE(first->releaseTab(0), tab);
        second->adoptTab(tab);
        job::testing::release();
        QTRY_COMPARE(shown_, 1);
        QCOMPARE(current(*second), tab);
        QVERIFY(tab->isUsable());
    }

    /// Covers source-probe and failed-write delivery after the previous owner closes.
    void queuedResultSurvivesItsPreviousOwner_data()
    {
        QTest::addColumn<bool>("writing");
        QTest::newRow("source probe") << false;
        QTest::newRow("failed write") << true;
    }

    /// Keeps deferred page results attached to the surviving tab after owner changes.
    void queuedResultSurvivesItsPreviousOwner()
    {
        QFETCH(bool, writing);
        QTemporaryDir files;
        const QString path = copy(files);
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        WindowManager manager;
        const QPointer<MainWindow> first(manager.createWindow());
        MainWindow *second = manager.createWindow();
        showWindow(*first);
        showWindow(*second);
        QVERIFY(testing::openAndWait(*first, path));
        TabPage *tab = current(*first);
        QSignalSpy finished(tab, &TabPage::operationFinished);
        if (writing) {
            hold(job::Step::Commit, out);
            job::testing::failWhen([out](job::Step step, const QString &where) {
                return step == job::Step::Commit && where == out;
            });
            choices_ << [out](ExtractDialog &dialog) { choose(dialog, out); };
        } else {
            hold(job::Step::Reading, path);
        }
        choices_ << [second, writing, out](ExtractDialog &dialog) {
            QCOMPARE(dialog.parentWidget(), second);
            if (writing) {
                QCOMPARE(dialog.choices().output, out);
                QVERIFY(error(dialog).contains(QStringLiteral("Could not write")));
            }
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(first, "extractPagesOp"));
        QTRY_COMPARE(job::testing::held(), 1);
        QDialog blocker;
        blocker.setWindowModality(Qt::ApplicationModal);
        blocker.show();
        job::testing::release();
        QTRY_COMPARE(finished.size(), writing ? 2 : 1);
        manager.mergeTab(first, 0, second, 0);
        QTRY_VERIFY(first.isNull());
        blocker.reject();
        QTRY_COMPARE(shown_, writing ? 2 : 1);
        QVERIFY(tab->isUsable());
    }

#ifdef Q_OS_UNIX
    /// Covers current-source identity protection for Extract and Delete.
    void redirectedSourceCannotReplaceItsNewTarget_data()
    {
        QTest::addColumn<bool>("deleting");
        QTest::newRow("extract") << false;
        QTest::newRow("delete") << true;
    }

    /// Refuses output that aliases a source redirected while its editor was open.
    void redirectedSourceCannotReplaceItsNewTarget()
    {
        const int mainThreadSteps = job::testing::stepsOnMainThread();
        QFETCH(bool, deleting);
        QTemporaryDir files;
        const QString path = copy(files);
        const QString target = copy(files, QStringLiteral("replacement.pdf"));
        QFile original(target);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray before = original.readAll();
        original.close();
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        QSignalSpy finished(current(window), &TabPage::operationFinished);
        QTimer overwrite;
        overwrite.setInterval(5);
        connect(&overwrite, &QTimer::timeout, this, [] {
            auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!question || question->property("testSubmitted").toBool())
                return;
            question->setProperty("testSubmitted", true);
            question->button(QMessageBox::Yes)->click();
        });
        overwrite.start();
        choices_ << [path, target](ExtractDialog &dialog) {
            QVERIFY(QFile::remove(path));
            QVERIFY(QFile::link(target, path));
            choose(dialog, target);
        };
        choices_ << [target, deleting](ExtractDialog &dialog) {
            QCOMPARE(dialog.choices().output, target);
            QCOMPARE(error(dialog), deleting
                ? QStringLiteral("The edited copy cannot replace the document it comes from.")
                : QStringLiteral("The extract cannot replace the document it comes from."));
            dialog.reject();
        };
        QVERIFY(QMetaObject::invokeMethod(&window, deleting ? "deletePagesOp" : "extractPagesOp"));
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), before);
        QTRY_COMPARE(shown_, 2);
        QCOMPARE(job::testing::stepsOnMainThread(), mainThreadSteps);
    }
#endif

    /// Opens a completed extracted copy through the normal placeholder flow.
    void successfulExtractOpensItsBackgroundResult()
    {
        QTemporaryDir files;
        const QString path = copy(files);
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, path));
        choices_ << [out](ExtractDialog &dialog) { choose(dialog, out, QStringLiteral("1,3"), true); };
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QTRY_COMPARE(window.tabCount(), 2);
        QTRY_VERIFY(current(window)->isLoaded());
        QCOMPARE(current(window)->path(), out);
        QCOMPARE(current(window)->viewer()->pageCount(), 2);
    }

    /// Opens an encrypted tab's extract with the tab's password, which the copy keeps.
    void encryptedExtractOpensWithoutAPasswordPrompt()
    {
        QTemporaryDir files;
        const QString plain = copy(files);
        const QString locked = files.filePath(QStringLiteral("locked.pdf"));
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(plain, locked, {}, QStringLiteral("secret"), {},
                                 QpdfService::Algorithm::AES256, {}, nullptr), QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, locked, false, -1, true, QStringLiteral("secret")));
        choices_ << [out](ExtractDialog &dialog) {
            choose(dialog, out, QStringLiteral("2"), true);
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QTRY_COMPARE(window.tabCount(), 2);
        QTRY_VERIFY(current(window)->isLoaded());
        QCOMPARE(current(window)->path(), out);
        QCOMPARE(current(window)->viewer()->pageCount(), 1);
        QCOMPARE(current(window)->password(), QStringLiteral("secret"));
        int pages = 0;
        QCOMPARE(PageOps::probe(out, &pages), PageOps::Status::NeedsPassword);
    }

    /// Preserves choices and allows a corrected open password after worker rejection.
    void wrongPasswordReopensAndAChangedPasswordCanRetry()
    {
        QTemporaryDir files;
        const QString plain = copy(files);
        const QString locked = files.filePath(QStringLiteral("locked.pdf"));
        const QString changed = files.filePath(QStringLiteral("changed.pdf"));
        const QString out = files.filePath(QStringLiteral("chosen.pdf"));
        QpdfService service;
        QCOMPARE(service.encrypt(plain, locked, {}, QStringLiteral("secret"), {},
                                 QpdfService::Algorithm::AES256, {}, nullptr), QpdfService::Status::Ok);
        QCOMPARE(service.encrypt(plain, changed, {}, QStringLiteral("changed"), {},
                                 QpdfService::Algorithm::AES256, {}, nullptr), QpdfService::Status::Ok);
        RenderEngine engine;
        MainWindow window(&engine, nullptr);
        showWindow(window);
        QVERIFY(testing::openAndWait(window, locked, false, -1, true, QStringLiteral("secret")));
        TabPage *tab = current(window);
        QVERIFY(QFile::remove(locked));
        QVERIFY(QFile::copy(changed, locked));
        choices_ << [out](ExtractDialog &dialog) {
            auto *password = dialog.findChild<QLineEdit *>(QStringLiteral("extractPassword"));
            QVERIFY(password);
            edit(password, QStringLiteral("wrong"));
            choose(dialog, out, QStringLiteral("1"));
        };
        choices_ << [out](ExtractDialog &dialog) {
            const auto chosen = dialog.choices();
            QCOMPARE(chosen.password, QStringLiteral("wrong"));
            QCOMPARE(chosen.specs, QStringList({QStringLiteral("1")}));
            QCOMPARE(chosen.output, out);
            QCOMPARE(error(dialog), QStringLiteral("That password is not correct."));
            edit(dialog.findChild<QLineEdit *>(QStringLiteral("extractPassword")), QStringLiteral("changed"));
            QVERIFY(error(dialog).isEmpty());
            choose(dialog, out, QStringLiteral("1"));
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "extractPagesOp"));
        QTRY_COMPARE(shown_, 2);
        QTRY_VERIFY(tab->isUsable());
        QTRY_VERIFY(QFileInfo::exists(out));
        QTRY_COMPARE(tab->password(), QStringLiteral("changed"));
        // The extract keeps the source's encryption, so the password that worked opens it.
        int pages = 0;
        QCOMPARE(PageOps::probe(out, &pages), PageOps::Status::NeedsPassword);
        QCOMPARE(PageOps::probe(out, &pages, QStringLiteral("changed")), PageOps::Status::Ok);
        QCOMPARE(pages, 1);
    }

private:
    /// Creates an isolated multi-page input for the real command flow.
    QString copy(QTemporaryDir &files, const QString &name = QStringLiteral("plan.pdf"))
    {
        const QString path = files.filePath(name);
        return QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path) ? path : QString();
    }

    std::unique_ptr<QTemporaryDir> profile_;
    QTimer dialogs_;
    QPointer<ExtractDialog> answered_;
    QList<std::function<void(ExtractDialog &)>> choices_;
    QStringList unexpected_;
    int shown_ = 0;
};

QTEST_MAIN(TstPageDialogOperations)
#include "tst_page_dialog_operations.moc"
