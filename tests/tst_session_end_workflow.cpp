#include "TestWaits.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/SecurityDialog.h"
#include "io/SaveCheck.h"
#include "net/UrlOpen.h"
#include "net/UrlDownloadLocation.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "session/SessionEnd.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QKeyEvent>
#include <QSessionManager>
#include <QWindow>
#include <QTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <cstdio>
#include <latch>
#endif

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace mervin;

namespace {
/// Reads exact file contents for source and destination preservation assertions.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Replaces a local test destination and reports whether every byte was written.
bool writeBytes(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

/// Realizes an isolated test window without showing it on the desktop.
void showWindow(MainWindow &window)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1000, 800);
    window.show();
    QCoreApplication::processEvents();
}

/// Finds the selected tab through the actual window tab widget.
TabPage *currentTab(MainWindow &window)
{
    return qobject_cast<TabPage *>(window.findChild<QTabWidget *>()->currentWidget());
}

// Drives the actual modal controls. Submit each answer once before it can enter
// another event loop, and keep answering nested overwrite questions.
class Interaction
{
public:
    /// Starts modal-control polling with one answer per dialog and guarded nested actions.
    Interaction()
    {
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (intercept && intercept(modal))
                return;
            if (auto *dialog = qobject_cast<SecurityDialog *>(modal)) {
                if (seenSecurity == dialog)
                    return;
                seenSecurity = dialog;
                ++securityCount;
                securityOwners.append(dialog->parentWidget());
                QMetaObject::invokeMethod(dialog, [this, dialog] {
                    const auto dismiss = qScopeGuard([guard = QPointer<SecurityDialog>(dialog)] {
                        if (guard && guard->isVisible())
                            guard->reject();
                    });
                    if (security)
                        security(*dialog);
                    else
                        dialog->reject();
                }, Qt::QueuedConnection);
            } else if (auto *dialog = qobject_cast<MergeDialog *>(modal)) {
                if (seenMerge == dialog)
                    return;
                seenMerge = dialog;
                ++mergeCount;
                QMetaObject::invokeMethod(dialog, [this, dialog] {
                    const auto dismiss = qScopeGuard([guard = QPointer<MergeDialog>(dialog)] {
                        if (guard && guard->isVisible())
                            guard->reject();
                    });
                    if (merge)
                        merge(*dialog);
                    else
                        dialog->reject();
                }, Qt::QueuedConnection);
            } else if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
                if (seenPicker == picker)
                    return;
                seenPicker = picker;
                suggested.append(picker->selectedFiles().value(0));
                if (files.isEmpty()) {
                    unexpected.append(picker->windowTitle());
                    picker->reject();
                    return;
                }
                const QString path = files.takeFirst();
                picker->selectFile(path);
                if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                    name->setText(QDir::toNativeSeparators(path));
                QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
            } else if (auto *prompt = qobject_cast<QInputDialog *>(modal)) {
                if (answered == prompt)
                    return;
                answered = prompt;
                ++passwordCount;
                if (passwords.isEmpty()) {
                    unexpected.append(prompt->windowTitle());
                    prompt->reject();
                    return;
                }
                const auto password = passwords.takeFirst();
                if (password) {
                    prompt->setTextValue(*password);
                    prompt->accept();
                } else {
                    prompt->reject();
                }
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (answered == box)
                    return;
                answered = box;
                messages.append(box->text());
                messageOwners.append(box->parentWidget());
                const auto choice = answer ? answer(*box)
                    : answers.isEmpty() ? QMessageBox::NoButton : answers.takeFirst();
                QAbstractButton *button = box->button(choice);
                if (!button) {
                    unexpected.append(box->windowTitle() + QStringLiteral(": ") + box->text());
                    box->reject();
                } else {
                    button->click();
                }
            }
        });
        timer.start();
    }

    QStringList files;
    QList<std::optional<QString>> passwords;
    QList<QMessageBox::StandardButton> answers;
    std::function<QMessageBox::StandardButton(const QMessageBox &)> answer;
    std::function<void(SecurityDialog &)> security;
    std::function<void(MergeDialog &)> merge;
    std::function<bool(QWidget *)> intercept;
    QStringList messages;
    QStringList suggested;
    QStringList unexpected;
    QList<QPointer<QWidget>> securityOwners;
    QList<QPointer<QWidget>> messageOwners;
    int securityCount = 0;
    int mergeCount = 0;
    int passwordCount = 0;

private:
    QTimer timer;
    QPointer<SecurityDialog> seenSecurity;
    QPointer<MergeDialog> seenMerge;
    QPointer<QFileDialog> seenPicker;
    QPointer<QWidget> answered;
};

/// Selects distinct passwords, AES-128 and restricted permissions for retry assertions.
void setEncryptionChoices(SecurityDialog &dialog)
{
    dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->setText(QStringLiteral("new password"));
    dialog.findChild<QLineEdit *>(QStringLiteral("securityOwnerPassword"))->setText(QStringLiteral("owner guard"));
    dialog.findChild<QComboBox *>(QStringLiteral("securityAlgorithm"))->setCurrentIndex(1);
    for (const QString &name : {QStringLiteral("securityAllowPrint"), QStringLiteral("securityAllowCopy"),
                               QStringLiteral("securityAllowModify"), QStringLiteral("securityAllowAnnotate")})
        dialog.findChild<QCheckBox *>(name)->setChecked(false);
}

/// Checks that failure recovery preserves the chosen output and every encryption control.
void verifyEncryptionChoices(const SecurityDialog &dialog, const QString &output)
{
    QVERIFY(dialog.request());
    QCOMPARE(QDir::fromNativeSeparators(dialog.request()->output), output);
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityUserPassword"))->text(),
             QStringLiteral("new password"));
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("securityOwnerPassword"))->text(),
             QStringLiteral("owner guard"));
    QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("securityAlgorithm"))->currentIndex(), 1);
    for (QCheckBox *permission : dialog.findChildren<QCheckBox *>())
        QVERIFY(!permission->isChecked());
}

/// Selects a nondefault Merge range and waits for its destination validation.
void setMergeChoices(MergeDialog &dialog, const QString &output)
{
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.reading(), 10000);
    auto *check = dialog.findChild<SaveCheck *>();
    QVERIFY(check);
    QTRY_VERIFY_WITH_TIMEOUT(!check->pending(), 10000);
    dialog.findChild<QLineEdit *>(QStringLiteral("mergeRowSpec"))->setText(QStringLiteral("2-3"));
    auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("mergeOutput"));
    field->setFocus();
    field->selectAll();
    QTest::keyClicks(field, QDir::toNativeSeparators(output));
    QTRY_VERIFY_WITH_TIMEOUT(!check->pending(), 10000);
    QTRY_VERIFY(dialog.findChild<QPushButton *>(QStringLiteral("mergeAccept"))->isEnabled());
}

/// Verifies that a completed failure keeps the accepted Merge plan and output.
void verifyMergeChoices(const MergeDialog &dialog, const QString &output)
{
    QCOMPARE(dialog.findChild<QLineEdit *>(QStringLiteral("mergeRowSpec"))->text(), QStringLiteral("2-3"));
    QCOMPARE(QDir::fromNativeSeparators(dialog.findChild<QLineEdit *>(QStringLiteral("mergeOutput"))->text()), output);
    QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("mergeError"))->text().isEmpty());
}

// Copies a PDF fixture and adds an annotation whose recovery content can be checked.
bool prepare(TabPage &tab, const QString &path)
{
    return QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path) && tab.open(path)
        && tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                  QStringLiteral("preserved")) >= 0;
}
// Holds a recovery manifest before commit admission for controlled grace checks.
void holdManifest()
{
    job::testing::holdWhen([](job::Step step, const QString &path) {
        return step == job::Step::Commit && path.endsWith(QStringLiteral(".pdf.json"));
    });
}

#ifdef Q_OS_WIN
// Observe messages after the application filter, before Qt's session handler.
class NativeSessionWindow : public QWindow
{
public:
    QList<std::pair<WPARAM, LPARAM>> queries;
    int keyPresses = 0;

protected:
    // Records forwarded query flags before allowing Qt to handle the message.
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        const auto *event = static_cast<const MSG *>(message);
        if (event->message == WM_QUERYENDSESSION)
            queries.append({event->wParam, event->lParam});
        return QWindow::nativeEvent(type, message, result);
    }

    // Counts delivered F12 presses to verify native session input suspension.
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_F12)
            ++keyPresses;
        return QWindow::event(event);
    }
};

// Sends an owned window an F12 press and release without changing desktop keyboard state.
void sendKey(HWND window)
{
    const LPARAM scan = static_cast<LPARAM>(MapVirtualKey(VK_F12, MAPVK_VK_TO_VSC)) << 16;
    SendMessage(window, WM_KEYDOWN, VK_F12, scan | 1);
    SendMessage(window, WM_KEYUP, VK_F12, scan | (LPARAM(3) << 30) | 1);
}

// Run the successful native outcome in a separate process so Qt's permanent
// aboutToQuit state and quit request cannot affect the other workflow checks.
int verifySuccessfulNativeOutcome()
{
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        return 1;
    QTemporaryDir profile;
    if (!profile.isValid())
        return 2;
    ConfigPaths::setOverrideDir(profile.path());
    Settings settings;
    settings.autoUpdate = false;
    if (!settings.save())
        return 3;
    WindowManager manager;
    NativeSessionWindow native;
    NativeSessionWindow second;
    native.create();
    second.create();
    const auto hwnd = reinterpret_cast<HWND>(native.winId());
    const auto other = reinterpret_cast<HWND>(second.winId());
    int quits = 0;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &native, [&] { ++quits; });
    if (SendMessage(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) != LRESULT(TRUE))
        return 4;
    SendMessage(hwnd, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
    SendMessage(other, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
    if (quits != 1 || !sessionend::active() || sessionend::RecoveryWrite::begin())
        return 5;
    return 0;
}
#endif
}

// Checks document recovery, session admission and native Windows query outcomes.
class TstSessionEndWorkflow : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> profile_;

public:
    /// Uses controllable Qt pickers for Security and non-Windows ordinary opens.
    TstSessionEndWorkflow()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }


private slots:
    // Creates an isolated profile with automatic updates disabled for each workflow.
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

    // Reopens admission and settles document work before removing the isolated profile.
    void cleanup()
    {
        sessionend::finishRequest(false);
        job::testing::release();
        job::testing::failWhen({});
        appfiles::testing::hold(false);
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    // Supplies loaded Save and activity-driven unload for worker durability checks.
    void manifestCompletionReleasesGraceBeforeGuiAdoption_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("loaded Save command") << true;
        QTest::newRow("edited activity unload") << false;
    }

    // Checks that manifest durability ends grace before the GUI adopts the worker result.
    void manifestCompletionReleasesGraceBeforeGuiAdoption()
    {
        QFETCH(bool, save);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        QVERIFY(testing::openAndWait(*window, path));
        const QPointer<TabPage> tab = window->findChild<TabPage *>();
        QVERIFY(tab && tab->isUsable());
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                       QStringLiteral("preserved")) >= 0);
        if (!save) {
            const QString active = files.filePath(QStringLiteral("active.pdf"));
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), active));
            QVERIFY(testing::openAndWait(*window, active));
        }
        holdManifest();
        const auto release = qScopeGuard([] { job::testing::release(); });
        if (save) {
            QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
        } else {
            window->updateDocumentActivity(0, 1);
            QVERIFY(window->updateDocumentActivity(61000, 1));
            QVERIFY(tab->isUnloading());
        }
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        const int guiSteps = job::testing::stepsOnMainThread();
        std::jthread releaseWorker([] {
            QThread::msleep(80);
            job::testing::release();
        });
        QElapsedTimer grace;
        grace.start();
        QCOMPARE(manager.prepareSessionEnd(1000), sessionend::Result::Settled);
        QVERIFY(grace.elapsed() >= 60);
        QVERIFY(grace.elapsed() < 1000);
        QVERIFY(tab->isBusy());
        QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
        manager.finishSessionEnd(false);
        if (save) {
            QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
            QVERIFY(!tab->hasUnsavedEdits());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(tab->isSuspended(), 10000);
            QVERIFY(tab->hasUnsavedEdits());
            QVERIFY(tab->hasRecoverySnapshot());
        }
        auto recovered = manager.engine()->openDocument(save ? path : tab->recoveryPath());
        QVERIFY(recovered);
        QCOMPARE(AnnotModel(*recovered).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

    // Checks that committed recovery avoids waiting for original replacement.
    void durableRecoveryDoesNotWaitForOriginalReplacement()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        TabPage tab(manager.engine());
        QVERIFY(prepare(tab, path));
        job::testing::holdWhen([path](job::Step step, const QString &file) {
            return step == job::Step::Commit && file == path;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        tab.startSave({});
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QElapsedTimer grace;
        grace.start();
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        QVERIFY(grace.elapsed() < 100);
        QVERIFY(tab.hasRecoverySnapshot());
        manager.finishSessionEnd(false);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(tab.isUsable(), 10000);
        QVERIFY(!tab.hasUnsavedEdits());
    }

    // Supplies snapshot kinds and OS outcomes for detached committed recovery disposal.
    void detachedStoppedSnapshotKeepsProtectionUntilOutcome_data()
    {
        QTest::addColumn<bool>("save");
        QTest::addColumn<bool>("ended");
        QTest::newRow("Save during successful end") << true << true;
        QTest::newRow("unload during successful end") << false << true;
        QTest::newRow("Save after cancellation") << true << false;
        QTest::newRow("unload after cancellation") << false << false;
    }

    // Checks Stop and owner deletion retain protection, with disposal following the OS outcome.
    void detachedStoppedSnapshotKeepsProtectionUntilOutcome()
    {
        QFETCH(bool, save);
        QFETCH(bool, ended);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        auto tab = std::make_unique<TabPage>(manager.engine());
        QVERIFY(prepare(*tab, path));
        job::testing::holdCommitWhen([](const QString &path) {
            return path.endsWith(QStringLiteral(".pdf.json"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        if (save)
            tab->startSave({});
        else
            QVERIFY(tab->suspend());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        tab->stopWaiting();
        QVERIFY(!tab->isBusy());
        tab.reset();
        QCOMPARE(manager.prepareSessionEnd(60), sessionend::Result::TimedOut);
        job::testing::release();
        manager.finishSessionEnd(ended);
        manager.engine()->shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        if (!ended) {
            QVERIFY(TabPage::recoverablePaths().isEmpty());
            return;
        }
        QCOMPARE(TabPage::recoverablePaths(), QStringList{path});
        const QDir recovery(QDir(profile_->path()).filePath(QStringLiteral("recovery")));
        const QStringList copies = recovery.entryList({QStringLiteral("*.pdf")}, QDir::Files);
        QCOMPARE(copies.size(), 1);
        auto recovered = manager.engine()->openDocument(recovery.filePath(copies.front()));
        QVERIFY(recovered);
        QCOMPARE(AnnotModel(*recovered).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

    // Supplies both OS outcomes for cleanup of a superseded adopted checkpoint.
    void supersededUnloadCleanupFollowsSessionOutcome_data()
    {
        QTest::addColumn<bool>("ended");
        QTest::newRow("successful end retains both old files") << true;
        QTest::newRow("cancellation removes both old files") << false;
    }

    // Starts session end during adoption before the old document's cleanup is scheduled.
    void supersededUnloadCleanupFollowsSessionOutcome()
    {
        QFETCH(bool, ended);
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        TabPage tab(manager.engine());
        const auto finish = qScopeGuard([&] { manager.finishSessionEnd(false); });
        QVERIFY(prepare(tab, path));
        QVERIFY(testing::suspendAndWait(tab));
        QVERIFY(Document::waitForCloses(10000));
        const QString priorCopy = tab.recoveryPath();
        const QString priorManifest = priorCopy + QStringLiteral(".json");
        QVERIFY(QFileInfo::exists(priorCopy));
        QVERIFY(QFileInfo::exists(priorManifest));
        QVERIFY(tab.resume());
        QVERIFY(tab.viewer()->annotModel()->addTextNote(0, {60, 60}, Qt::yellow, {},
                                                       QStringLiteral("second edit")) >= 0);
        std::optional<sessionend::Result> request;
        connect(tab.viewer(), &ViewerWidget::formModeChanged, &tab, [&](bool) {
            if (!request && tab.isBusy() && tab.hasRecoverySnapshot()
                && tab.recoveryPath() != priorCopy)
                request = manager.prepareSessionEnd(0);
        });
        QVERIFY(testing::suspendAndWait(tab));
        QVERIFY(request.has_value());
        QCOMPARE(*request, sessionend::Result::Settled);
        QVERIFY(sessionend::active());
        const QString currentCopy = tab.recoveryPath();
        QVERIFY(currentCopy != priorCopy);
        const bool waitedForOutcome = !Document::waitForCloses(80);
        manager.finishSessionEnd(ended);
        QVERIFY(Document::waitForCloses(10000));
        QCOMPARE(QFileInfo::exists(priorCopy), ended);
        QCOMPARE(QFileInfo::exists(priorManifest), ended);
        QVERIFY(waitedForOutcome);
        QVERIFY(QFileInfo::exists(currentCopy));
        QVERIFY(QFileInfo::exists(currentCopy + QStringLiteral(".json")));
        QVERIFY(tab.hasRecoverySnapshot() && tab.hasUnsavedEdits());
        auto recovered = manager.engine()->openDocument(currentCopy);
        QVERIFY(recovered);
        const auto annotations = AnnotModel(*recovered).pageAnnots(0);
        QCOMPARE(annotations.size(), size_t(2));
        QCOMPARE(annotations.front().contents, QStringLiteral("preserved"));
        QCOMPARE(annotations.back().contents, QStringLiteral("second edit"));
    }

    // Checks durable unload does not wait for its queued GUI ownership transfer.
    void durableUnloadDoesNotWaitForQueuedAdoption()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        TabPage tab(manager.engine());
        QVERIFY(prepare(tab, path));
        holdManifest();
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(tab.suspend());
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        job::testing::release();
        QDeadlineTimer worker(10000);
        while (tab.hasPendingRecoveryWrite() && !worker.hasExpired())
            QThread::msleep(1);
        QVERIFY(!tab.hasPendingRecoveryWrite());
        QVERIFY(tab.isBusy());
        QElapsedTimer grace;
        grace.start();
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        QVERIFY(grace.elapsed() < 100);
        QCOMPARE(TabPage::recoverablePaths(), QStringList{path});
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(tab.isSuspended(), 10000);
        QVERIFY(tab.hasRecoverySnapshot());
    }

    // Checks session cancellation restores edited unload without a false suspension error.
    void canceledSessionRestoresUnloadAdmission()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        TabPage tab(manager.engine());
        QVERIFY(prepare(tab, path));
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        QString error;
        QVERIFY(!tab.suspend(&error));
        QVERIFY(error.isEmpty());
        QVERIFY(tab.isUsable());
        QVERIFY(tab.hasUnsavedEdits());
        QVERIFY(!tab.isUnloading());
        manager.finishSessionEnd(false);
        QVERIFY(testing::suspendAndWait(tab, &error));
        QVERIFY(tab.isSuspended());
        QVERIFY(tab.hasRecoverySnapshot());
    }

    // Checks that Save admission closes during the request and cancellation restores it.
    void canceledSessionRestoresSaveAdmission()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        TabPage tab(manager.engine());
        QVERIFY(prepare(tab, path));
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        QVERIFY(manager.isClosing());
        std::optional<TabPage::SaveCompletion> answer;
        TabPage::SaveCallbacks callbacks;
        callbacks.finished = [&](const auto &result) { answer = result; };
        tab.startSave(callbacks);
        QVERIFY(answer);
        QCOMPARE(answer->result, TabPage::SaveResult::Canceled);
        QCOMPARE(answer->generation, tab.saveGeneration());
        QVERIFY(tab.isUsable());
        QVERIFY(tab.hasUnsavedEdits());
        manager.finishSessionEnd(false);
        QVERIFY(!manager.isClosing());
        answer.reset();
        tab.startSave(callbacks);
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 10000);
        QCOMPARE(answer->result, TabPage::SaveResult::Saved);
        QVERIFY(!tab.hasUnsavedEdits());
    }

    // Checks that the "File changed" question waits for session cancellation.
    void changedSaveQuestionWaitsForCanceledSessionOutcome()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(path));
        auto *tab = window->findChild<TabPage *>();
        QTRY_VERIFY_WITH_TIMEOUT(tab && tab->isUsable(), 10000);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                       QStringLiteral("preserved")) >= 0);
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::Append));
        QVERIFY(changed.write("\n% External change\n") > 0);
        changed.close();
        int questions = 0;
        QTimer answers;
        answers.setInterval(5);
        connect(&answers, &QTimer::timeout, &manager, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (box && box->isVisible() && box->windowTitle()
                    == QCoreApplication::translate("MainWindow", "File changed")) {
                    ++questions;
                    box->done(QMessageBox::Cancel);
                }
            }
        });
        answers.start();
        QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
        QCOMPARE(manager.prepareSessionEnd(30), sessionend::Result::TimedOut);
        QTest::qWait(80);
        QCOMPARE(questions, 0);
        QVERIFY(tab->isBusy());
        manager.finishSessionEnd(false);
        QTRY_COMPARE_WITH_TIMEOUT(questions, 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(tab->hasUnsavedEdits());
    }

    /// Covers a completed confirmation and an already accepted replacement choice.
    void securityConsentWaitsForSessionOutcome_data()
    {
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<bool>("ended");
        QTest::newRow("completed confirmation after cancellation") << false << false;
        QTest::newRow("completed confirmation during successful end") << false << true;
        QTest::newRow("accepted replacement after cancellation") << true << false;
        QTest::newRow("accepted replacement during successful end") << true << true;
    }

    /// Retains the original destination snapshot and follows a moved tab after cancellation.
    void securityConsentWaitsForSessionOutcome()
    {
        QFETCH(bool, accepted);
        QFETCH(bool, ended);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray original("original contents");
        const QByteArray changed("changed contents!");
        QCOMPARE(original.size(), changed.size());
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(writeBytes(output, original));
        WindowManager manager;
        QPointer<MainWindow> window(manager.createWindow());
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        QPointer<TabPage> page(currentTab(*window));
        QPointer<MainWindow> expectedOwner(window);
        std::atomic<int> workerSteps{0};
        Interaction dialogs;
        dialogs.files << output;
        int questions = 0;
        QString failure;
        bool retained = false;
        std::optional<sessionend::Result> pending;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.objectName() == QStringLiteral("securityReplace")) {
                ++questions;
                if (accepted && !pending) {
                    job::testing::failWhen([&](job::Step, const QString &) {
                        ++workerSteps;
                        return false;
                    });
                    pending = manager.prepareSessionEnd(0);
                }
                return QMessageBox::Yes;
            }
            failure = box.text();
            return QMessageBox::Ok;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                if (!accepted) {
                    job::testing::holdWhen([output](job::Step step, const QString &path) {
                        return step == job::Step::Reading && QDir::fromNativeSeparators(path) == output;
                    });
                }
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                verifyEncryptionChoices(dialog, output);
                QCOMPARE(dialog.parentWidget(), expectedOwner.data());
                QCOMPARE(bytes(output), changed);
                retained = true;
                dialog.reject();
            }
        };
        const auto release = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        if (!accepted) {
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
            pending = manager.prepareSessionEnd(0);
            job::testing::release();
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        }
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QTest::qWait(80);
        QCOMPARE(questions, accepted ? 1 : 0);
        QCOMPARE(dialogs.securityCount, 1);
        QVERIFY(!QApplication::activeModalWidget());
        if (accepted)
            QCOMPARE(workerSteps.load(), 0);
        QVERIFY(writeBytes(output, changed));
        if (!accepted && !ended) {
            expectedOwner = manager.createWindow();
            showWindow(*expectedOwner);
            manager.mergeTab(window, 0, expectedOwner, -1);
            QTRY_VERIFY(page && page->window() == expectedOwner);
            if (window)
                delete window.data();
            QVERIFY(!window);
        }
        manager.finishSessionEnd(ended);
        if (ended) {
            QTest::qWait(80);
            QCOMPARE(dialogs.securityCount, 1);
            QCOMPARE(questions, accepted ? 1 : 0);
            delete window.data();
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
            QCOMPARE(questions, 1);
            QVERIFY(failure.contains(QStringLiteral("The destination changed.")));
            QCOMPARE(dialogs.securityCount, 2);
        }
        QCOMPARE(bytes(output), changed);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// A failed worker result keeps its editable Merge plan until logoff is canceled.
    void completedMergeFailureWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("merged.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        TabPage *page = currentTab(*window);
        Interaction dialogs;
        bool retained = false;
        dialogs.merge = [&](MergeDialog &dialog) {
            if (dialogs.mergeCount == 1) {
                setMergeChoices(dialog, output);
                job::testing::holdWhen([folder = files.path()](job::Step step, const QString &path) {
                    return step == job::Step::Writing && QDir::fromNativeSeparators(path) == folder;
                });
                job::testing::failWhen([output](job::Step step, const QString &path) {
                    return step == job::Step::Commit && QDir::fromNativeSeparators(path) == output;
                });
                dialog.findChild<QPushButton *>(QStringLiteral("mergeAccept"))->click();
                QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 10000);
            } else {
                verifyMergeChoices(dialog, output);
                QCOMPARE(dialog.parentWidget(), window);
                retained = true;
                dialog.reject();
            }
        };
        const auto clear = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        QVERIFY(QMetaObject::invokeMethod(window, "mergeDocuments"));
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        job::testing::release();
        QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
        QTest::qWait(80);
        QCOMPARE(dialogs.mergeCount, 1);
        QVERIFY(!QFile::exists(output));
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
        QCOMPARE(dialogs.mergeCount, 2);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// An accepted Merge keeps its original consent while the OS outcome is pending.
    void acceptedMergeWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("merged.pdf"));
        const QByteArray original("original contents");
        const QByteArray changed("changed contents!");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(writeBytes(output, original));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        std::atomic<int> workerSteps{0};
        Interaction dialogs;
        int questions = 0;
        bool retained = false;
        std::optional<sessionend::Result> pending;
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.text().contains(QStringLiteral("Replace it?"))) {
                ++questions;
                job::testing::failWhen([&](job::Step, const QString &) {
                    ++workerSteps;
                    return false;
                });
                pending = manager.prepareSessionEnd(0);
                return QMessageBox::Yes;
            }
            return QMessageBox::No;
        };
        dialogs.merge = [&](MergeDialog &dialog) {
            if (dialogs.mergeCount == 1) {
                setMergeChoices(dialog, output);
                dialog.findChild<QPushButton *>(QStringLiteral("mergeAccept"))->click();
                QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 10000);
            } else {
                verifyMergeChoices(dialog, output);
                QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("mergeError"))->text()
                            .contains(QStringLiteral("The destination changed.")));
                retained = true;
                dialog.reject();
            }
        };
        const auto clear = qScopeGuard([] { job::testing::failWhen({}); });
        QVERIFY(QMetaObject::invokeMethod(window, "mergeDocuments"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QVERIFY(currentTab(*window)->isUsable());
        QCOMPARE(workerSteps.load(), 0);
        QCOMPARE(bytes(output), original);
        QVERIFY(writeBytes(output, changed));
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
        QCOMPARE(questions, 1);
        QCOMPARE(bytes(output), changed);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers cancellation, successful end and a newer load replacing a queued password failure.
    void completedResumeWaitsForSessionOutcome_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("canceled password failure") << 0;
        QTest::newRow("successful session end") << 1;
        QTest::newRow("newer generation after cancellation") << 2;
    }

    /// A non-fresh resume retains its password reaction without replaying stale generations.
    void completedResumeWaitsForSessionOutcome()
    {
        QFETCH(int, outcome);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted.pdf"));
        const QString password = QStringLiteral("source password");
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {}, password,
                     {}, QpdfService::Algorithm::AES128, {}), QpdfService::Status::Ok);
        WindowManager manager;
        QPointer<MainWindow> window(manager.createWindow());
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source, false, -1, true, password));
        TabPage *page = currentTab(*window);
        QVERIFY(!page->freshOpen());
        QVERIFY(testing::suspendAndWait(*page));
        page->setPassword({});
        Interaction dialogs;
        dialogs.passwords << password;
        job::testing::holdWhen([source](job::Step step, const QString &path) {
            return step == job::Step::Reading && QDir::fromNativeSeparators(path) == source;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QSignalSpy resumed(page, &TabPage::resumeFinished);
        page->resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        const quint64 generation = page->loadGeneration();
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(!resumed.first().at(0).toBool());
        QVERIFY(resumed.first().at(2).toBool());
        QVERIFY(!page->isLoading());
        QTest::qWait(80);
        QCOMPARE(dialogs.passwordCount, 0);
        manager.finishSessionEnd(outcome == 1);
        if (outcome == 1) {
            QTest::qWait(80);
            QCOMPARE(dialogs.passwordCount, 0);
            QCOMPARE(page->loadGeneration(), generation);
            delete window.data();
        } else {
            if (outcome == 2) {
                page->setPassword(password);
                page->retryLoad();
            }
            QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
            QCOMPARE(dialogs.passwordCount, outcome == 2 ? 0 : 1);
            QCOMPARE(page->loadGeneration(), generation + 1);
            QCOMPARE(page->password(), password);
        }
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Successful resume keeps its hidden-window suspension reaction through cancellation.
    void successfulResumeWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        // A standalone owner isolates this reaction from the manager's desktop
        // tray-availability timer, which can restore windows when no tray exists.
        MainWindow window(manager.engine(), &manager);
        showWindow(window);
        window.applyMemorySettings(1, true);
        QVERIFY(testing::openAndWait(window, source));
        TabPage *page = currentTab(window);
        QVERIFY(testing::suspendAndWait(*page));
        job::testing::holdWhen([source](job::Step step, const QString &path) {
            return step == job::Step::Reading && QDir::fromNativeSeparators(path) == source;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QSignalSpy resumed(page, &TabPage::resumeFinished);
        page->resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        window.hideToTray();
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(resumed.first().at(0).toBool());
        QTest::qWait(80);
        QVERIFY(page->isLoaded());
        QVERIFY(window.isInTray());
        manager.finishSessionEnd(false);
        // This owner is intentionally outside the manager's registered windows.
        // Wake its ordinary queue as cancellation does for registered owners.
        window.whenTabsCanChange([] {});
        QTRY_VERIFY_WITH_TIMEOUT(page->isSuspended(), 10000);
        QCOMPARE(resumed.size(), 1);
    }

    /// Missing-file reconciliation is retained with the whole non-fresh resume reaction.
    void missingResumeWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        QPointer<TabPage> page(currentTab(*window));
        QVERIFY(testing::suspendAndWait(*page));
        QVERIFY(Document::waitForCloses(10000));
        job::testing::holdWhen([source](job::Step step, const QString &path) {
            return step == job::Step::Reading && QDir::fromNativeSeparators(path) == source;
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        QSignalSpy resumed(page, &TabPage::resumeFinished);
        page->resumeAsync();
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        QVERIFY(QFile::remove(source));
        job::testing::release();
        QTRY_COMPARE_WITH_TIMEOUT(resumed.size(), 1, 10000);
        QVERIFY(!resumed.first().at(0).toBool());
        QVERIFY(!resumed.first().at(2).toBool());
        QTest::qWait(80);
        QVERIFY(page);
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(!page, 10000);
        QCOMPARE(window->tabCount(), 0);
    }

    /// An accepted Security form executes once after canceled logoff.
    void acceptedSecurityFormWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        Interaction dialogs;
        dialogs.files << output;
        dialogs.answers << QMessageBox::No;
        std::optional<sessionend::Result> pending;
        std::atomic<int> reads{0};
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && qobject_cast<QFileDialog *>(modal))
                pending = manager.prepareSessionEnd(0);
            return false;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            setEncryptionChoices(dialog);
            job::testing::failWhen([&](job::Step step, const QString &path) {
                if (step == job::Step::Reading && QDir::fromNativeSeparators(path) == source)
                    ++reads;
                return false;
            });
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        const auto clear = qScopeGuard([] { job::testing::failWhen({}); });
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QCOMPARE(reads.load(), 0);
        QVERIFY(!QFile::exists(output));
        manager.finishSessionEnd(false);
        QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages.size(), 1, 10000);
        QCOMPARE(dialogs.securityCount, 1);
        QpdfService::Info info;
        QCOMPARE(QpdfService().readInfo(output, QStringLiteral("new password"), info), QpdfService::Status::Ok);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// An accepted source password keeps the original absent-destination consent.
    void acceptedSecurityPasswordWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QString password = QStringLiteral("source password");
        const QByteArray winner("another completed writer");
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {}, password,
                     {}, QpdfService::Algorithm::AES128, {}), QpdfService::Status::Ok);
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source, false, -1, true, password));
        currentTab(*window)->setPassword({});
        std::atomic<int> workerSteps{0};
        Interaction dialogs;
        dialogs.files << output;
        dialogs.passwords << QStringLiteral("wrong") << password;
        dialogs.answers << QMessageBox::Ok;
        std::optional<sessionend::Result> pending;
        bool retained = false;
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && dialogs.passwordCount == 1 && qobject_cast<QInputDialog *>(modal)) {
                job::testing::failWhen([&](job::Step, const QString &) {
                    ++workerSteps;
                    return false;
                });
                pending = manager.prepareSessionEnd(0);
            }
            return false;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                verifyEncryptionChoices(dialog, output);
                retained = true;
                dialog.reject();
            }
        };
        const auto clear = qScopeGuard([] { job::testing::failWhen({}); });
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QVERIFY(currentTab(*window)->isUsable());
        QCOMPARE(workerSteps.load(), 0);
        QCOMPARE(dialogs.passwordCount, 2);
        QVERIFY(!QFile::exists(output));
        QVERIFY(writeBytes(output, winner));
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.messages.first().contains(QStringLiteral("The destination changed.")));
        QCOMPARE(bytes(output), winner);
        QCOMPARE(dialogs.passwordCount, 2);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Dismissing a failure warning during pending logoff retains the Security form.
    void securityFailureDismissalWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        job::testing::failWhen([output](job::Step step, const QString &path) {
            return step == job::Step::Commit && QDir::fromNativeSeparators(path) == output;
        });
        const auto clear = qScopeGuard([] { job::testing::failWhen({}); });
        Interaction dialogs;
        dialogs.files << output;
        std::optional<sessionend::Result> pending;
        bool retained = false;
        dialogs.answer = [&](const QMessageBox &) {
            pending = manager.prepareSessionEnd(0);
            return QMessageBox::Ok;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                verifyEncryptionChoices(dialog, output);
                retained = true;
                dialog.reject();
            }
        };
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QCOMPARE(dialogs.securityCount, 1);
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
        QCOMPARE(dialogs.securityCount, 2);
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(!QFile::exists(output));
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Covers resumed password acceptance under both OS outcomes.
    void acceptedDocumentPasswordWaitsForSessionOutcome_data()
    {
        QTest::addColumn<bool>("ended");
        QTest::newRow("canceled session end") << false;
        QTest::newRow("successful session end") << true;
    }

    /// A modal password answer cannot cache credentials or start a load while logoff is pending.
    void acceptedDocumentPasswordWaitsForSessionOutcome()
    {
        QFETCH(bool, ended);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted.pdf"));
        const QString password = QStringLiteral("source password");
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {}, password,
                     {}, QpdfService::Algorithm::AES128, {}), QpdfService::Status::Ok);
        WindowManager manager;
        QPointer<MainWindow> window(manager.createWindow());
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source, false, -1, true, password));
        TabPage *page = currentTab(*window);
        QVERIFY(testing::suspendAndWait(*page));
        page->setPassword({});
        Interaction dialogs;
        dialogs.passwords << password;
        std::optional<sessionend::Result> pending;
        quint64 generation = 0;
        std::atomic<int> reads{0};
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && modal && modal->objectName() == QStringLiteral("documentPassword")) {
                generation = page->loadGeneration();
                job::testing::holdWhen([source](job::Step step, const QString &path) {
                    return step == job::Step::Reading && QDir::fromNativeSeparators(path) == source;
                });
                job::testing::failWhen([&](job::Step step, const QString &path) {
                    if (step == job::Step::Reading && QDir::fromNativeSeparators(path) == source)
                        ++reads;
                    return false;
                });
                pending = manager.prepareSessionEnd(0);
            }
            return false;
        };
        const auto clear = qScopeGuard([] {
            job::testing::release();
            job::testing::failWhen({});
        });
        page->resumeAsync();
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QCOMPARE(page->loadGeneration(), generation);
        QCOMPARE(reads.load(), 0);
        QCOMPARE(job::testing::held(), 0);
        QVERIFY(page->password().isEmpty());
        manager.finishSessionEnd(ended);
        if (ended) {
            QTest::qWait(80);
            QCOMPARE(page->loadGeneration(), generation);
            QCOMPARE(reads.load(), 0);
            QCOMPARE(job::testing::held(), 0);
            delete window.data();
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
            QCOMPARE(page->loadGeneration(), generation + 1);
            job::testing::release();
            QTRY_VERIFY_WITH_TIMEOUT(page->isUsable(), 10000);
            QCOMPARE(page->password(), password);
        }
        QCOMPARE(dialogs.passwordCount, 1);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// Rejected failed Duplicate prompts still close the failed window after canceled logoff.
    void rejectedDuplicatePasswordWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("encrypted.pdf"));
        QCOMPARE(QpdfService().encrypt(QStringLiteral(MERVIN_FIXTURE_PDF), source, {},
                     QStringLiteral("password"), {}, QpdfService::Algorithm::AES128, {}), QpdfService::Status::Ok);
        WindowManager manager;
        MainWindow *kept = manager.createWindow();
        Interaction dialogs;
        dialogs.passwords << std::nullopt;
        std::optional<sessionend::Result> pending;
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && modal && modal->objectName() == QStringLiteral("documentPassword"))
                pending = manager.prepareSessionEnd(0);
            return false;
        };
        manager.duplicateToNewWindow(source, {}, {}, {});
        QPointer<MainWindow> duplicate;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<MainWindow *>(widget); window && window != kept)
                duplicate = window;
        QVERIFY(duplicate);
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QVERIFY(duplicate);
        QCOMPARE(duplicate->tabCount(), 1);
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(!duplicate, 10000);
        QCOMPARE(dialogs.passwordCount, 1);
        QVERIFY(kept->isVisible());
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// An accepted output offer retains the encrypted path and password until cancellation.
    void acceptedOpenOfferWaitsForCancellation()
    {
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        Interaction dialogs;
        dialogs.files << output;
        std::optional<sessionend::Result> pending;
        dialogs.security = [&](SecurityDialog &dialog) {
            setEncryptionChoices(dialog);
            dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
        };
        dialogs.answer = [&](const QMessageBox &box) {
            if (box.windowTitle() == QStringLiteral("Done")) {
                pending = manager.prepareSessionEnd(0);
                return QMessageBox::Yes;
            }
            return QMessageBox::No;
        };
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QCOMPARE(window->tabCount(), 1);
        QVERIFY(QFile::exists(output));
        manager.finishSessionEnd(false);
        QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(currentTab(*window)->isUsable(), 10000);
        QCOMPARE(currentTab(*window)->path(), output);
        QCOMPARE(currentTab(*window)->password(), QStringLiteral("new password"));
        QCOMPARE(dialogs.passwordCount, 0);
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

#ifndef Q_OS_WIN
    /// A completed local HTTP download retains its new-window choice until cancellation.
    void completedDownloadWaitsForCancellation()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/source.pdf").arg(server.serverPort()));
        const QString destination = QDir(urlopen::downloadDirectory()).filePath(urlopen::cachedFileName(url));
        const QByteArray pdf = bytes(QStringLiteral(MERVIN_FIXTURE_PDF));
        QVERIFY(pdf.startsWith("%PDF-"));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        Interaction dialogs;
        dialogs.files << url.toString();
        QVERIFY(QMetaObject::invokeMethod(window, "onOpenInNewWindow"));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 10000);
        QTcpSocket *peer = server.nextPendingConnection();
        QVERIFY(peer);
        QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() > 0, 10000);
        QVERIFY(peer->readAll().startsWith("GET /source.pdf "));
        QCOMPARE(manager.prepareSessionEnd(0), sessionend::Result::Settled);
        const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\nContent-Length: "
            + QByteArray::number(pdf.size()) + "\r\nConnection: close\r\n\r\n" + pdf;
        QCOMPARE(peer->write(response), response.size());
        peer->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(destination), 10000);
        QCOMPARE(bytes(destination), pdf);
        QTest::qWait(80);
        QCOMPARE(window->tabCount(), 1);
        const auto destinationWindow = [&]() -> MainWindow * {
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *owner = qobject_cast<MainWindow *>(widget); owner && owner != window)
                    return owner;
            return nullptr;
        };
        QVERIFY(!destinationWindow());
        manager.finishSessionEnd(false);
        QTRY_VERIFY_WITH_TIMEOUT(destinationWindow(), 10000);
        MainWindow *opened = destinationWindow();
        QTRY_COMPARE_WITH_TIMEOUT(opened->tabCount(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(currentTab(*opened)->isUsable(), 10000);
        QCOMPARE(currentTab(*opened)->path(), destination);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    /// The ordinary open wrapper uses Qt controls only outside Windows.
    void acceptedPickerWaitsForCancellation_data()
    {
        QTest::addColumn<bool>("newWindow");
        QTest::newRow("ordinary open") << false;
        QTest::newRow("open in new window") << true;
    }

    /// Accepted picker paths cannot open or create an empty window while logoff is pending.
    void acceptedPickerWaitsForCancellation()
    {
        QFETCH(bool, newWindow);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, QStringLiteral(MERVIN_FIXTURE_PDF)));
        Interaction dialogs;
        dialogs.files << source;
        std::optional<sessionend::Result> pending;
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && qobject_cast<QFileDialog *>(modal))
                pending = manager.prepareSessionEnd(0);
            return false;
        };
        const auto windows = [] {
            QList<MainWindow *> result;
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *owner = qobject_cast<MainWindow *>(widget))
                    result.append(owner);
            return result;
        };
        QVERIFY(QMetaObject::invokeMethod(window, newWindow ? "onOpenInNewWindow" : "onOpen"));
        QVERIFY(pending.has_value());
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QCOMPARE(windows().size(), 1);
        QCOMPARE(window->tabCount(), 1);
        manager.finishSessionEnd(false);
        QTRY_COMPARE_WITH_TIMEOUT(windows().size(), newWindow ? 2 : 1, 10000);
        MainWindow *owner = window;
        if (newWindow)
            for (MainWindow *candidate : windows())
                if (candidate != window)
                    owner = candidate;
        QTRY_COMPARE_WITH_TIMEOUT(owner->tabCount(), newWindow ? 1 : 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(currentTab(*owner)->isUsable(), 10000);
        QCOMPARE(currentTab(*owner)->path(), source);
        QVERIFY(dialogs.unexpected.isEmpty());
    }
#endif

    /// Covers cancellation wakeup, successful suppression and ordinary later visibility.
    void retainedSecurityFormWaitsForSessionOutcome_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::newRow("restore while pending then cancel") << 0;
        QTest::newRow("restore while pending then end") << 1;
        QTest::newRow("cancel while hidden then restore") << 2;
    }

    /// A hidden retained form survives pending state and needs no later visibility wakeup.
    void retainedSecurityFormWaitsForSessionOutcome()
    {
        QFETCH(int, outcome);
        QTemporaryDir files;
        const QString source = files.filePath(QStringLiteral("source.pdf"));
        const QString output = files.filePath(QStringLiteral("output.pdf"));
        const QByteArray original("existing destination");
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), source));
        QVERIFY(writeBytes(output, original));
        WindowManager manager;
        QPointer<MainWindow> window(manager.createWindow());
        showWindow(*window);
        QVERIFY(testing::openAndWait(*window, source));
        Interaction dialogs;
        dialogs.files << output;
        dialogs.answers << QMessageBox::No;
        std::optional<sessionend::Result> pending;
        bool retained = false;
        dialogs.intercept = [&](QWidget *modal) {
            if (!pending && modal && modal->objectName() == QStringLiteral("securityReplace")) {
                pending = manager.prepareSessionEnd(0);
                window->hide();
            }
            return false;
        };
        dialogs.security = [&](SecurityDialog &dialog) {
            if (dialogs.securityCount == 1) {
                setEncryptionChoices(dialog);
                dialog.findChild<QPushButton *>(QStringLiteral("securityEncrypt"))->click();
            } else {
                verifyEncryptionChoices(dialog, output);
                retained = true;
                dialog.reject();
            }
        };
        QVERIFY(QMetaObject::invokeMethod(window, "openSecurity"));
        QTRY_VERIFY_WITH_TIMEOUT(pending.has_value(), 10000);
        QCOMPARE(*pending, sessionend::Result::Settled);
        QTest::qWait(80);
        QVERIFY(!window->isVisible());
        QCOMPARE(dialogs.securityCount, 1);
        if (outcome != 2) {
            window->show();
            QTest::qWait(80);
            QCOMPARE(dialogs.securityCount, 1);
        }
        manager.finishSessionEnd(outcome == 1);
        if (outcome == 1) {
            QTest::qWait(80);
            QCOMPARE(dialogs.securityCount, 1);
            delete window.data();
        } else {
            if (outcome == 2) {
                QTest::qWait(80);
                QCOMPARE(dialogs.securityCount, 1);
                window->show();
            }
            QTRY_VERIFY_WITH_TIMEOUT(retained, 10000);
            QCOMPARE(dialogs.securityCount, 2);
        }
        QCOMPARE(bytes(output), original);
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.unexpected.isEmpty());
    }

    // Supplies ordinary file work and actual windows with held Save or unload snapshots.
    void successfulSessionEndAddsNoOrdinaryExitWait_data()
    {
        QTest::addColumn<QString>("work");
        QTest::newRow("application file writes") << QStringLiteral("appfiles");
        QTest::newRow("actual window with held Save") << QStringLiteral("save");
        QTest::newRow("actual window with held unload") << QStringLiteral("unload");
    }

    // Checks successful session teardown adds no ordinary budget while real work remains held.
    void successfulSessionEndAddsNoOrdinaryExitWait()
    {
        QFETCH(QString, work);
        auto manager = std::make_unique<WindowManager>();
        const auto release = qScopeGuard([] {
            job::testing::release();
            appfiles::testing::hold(false);
        });
        QTemporaryDir files;
        if (work == QStringLiteral("appfiles")) {
            appfiles::testing::hold(true);
            appfiles::write(profile_->filePath(QStringLiteral("held.json")), "pending");
            QVERIFY(!appfiles::flush(0));
            QCOMPARE(manager->prepareSessionEnd(0), sessionend::Result::Settled);
        } else {
            const QString path = files.filePath(QStringLiteral("edited.pdf"));
            QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
            MainWindow *window = manager->createWindow();
            QVERIFY(testing::openAndWait(*window, path));
            const QPointer<TabPage> tab = window->findChild<TabPage *>();
            QVERIFY(tab && tab->isUsable());
            QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                           QStringLiteral("preserved")) >= 0);
            QVERIFY(testing::suspendAndWait(*tab));
            tab->resumeAsync();
            QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
            QVERIFY(tab->hasRecoverySnapshot());
            job::testing::holdWhen([](job::Step step, const QString &path) {
                const QString name = QFileInfo(path).fileName();
                return step == job::Step::Writing && (name.startsWith(QStringLiteral(".save-"))
                    || name.startsWith(QStringLiteral(".unload-")));
            });
            if (work == QStringLiteral("save"))
                QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
            else
                QVERIFY(tab->suspend());
            QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
            QVERIFY(tab->isBusy());
            QCOMPARE(manager->prepareSessionEnd(0), sessionend::Result::TimedOut);
        }
        manager->finishSessionEnd(true);
        QElapsedTimer exit;
        exit.start();
        manager.reset();
        QVERIFY(exit.elapsed() < 1000);
    }

#ifdef Q_OS_WIN
    // Checks forwarding, callbacks, native input and cancellation across owned Qt windows.
    void nativeQueryAndCanceledEndReachQtAndRestoreAdmission()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        WindowManager manager;
        MainWindow *window = manager.createWindow();
        NativeSessionWindow native;
        NativeSessionWindow second;
        native.create();
        second.create();
        const auto hwnd = reinterpret_cast<HWND>(native.winId());
        const auto other = reinterpret_cast<HWND>(second.winId());
        int callbacks = 0;
        bool cancel = false;
        connect(qApp, &QGuiApplication::commitDataRequest, &native, [&](QSessionManager &session) {
            ++callbacks;
            if (cancel)
                session.cancel();
        }, Qt::DirectConnection);
        const auto reset = qScopeGuard([&] { SendMessage(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF); });
        sendKey(hwnd);
        QTRY_COMPARE_WITH_TIMEOUT(native.keyPresses, 1, 1000);
        auto write = sessionend::RecoveryWrite::begin();
        QVERIFY(write);
        std::jthread finish([copy = *write] {
            QThread::msleep(70);
            copy.finish();
        });
        constexpr LPARAM otherFlags = ENDSESSION_CLOSEAPP | ENDSESSION_CRITICAL;
        constexpr LPARAM flags = static_cast<LPARAM>(ENDSESSION_LOGOFF) | otherFlags;
        constexpr WPARAM queryParameter = 19;
        QElapsedTimer query;
        query.start();
        QCOMPARE(SendMessage(hwnd, WM_QUERYENDSESSION, queryParameter, flags), LRESULT(TRUE));
        QVERIFY(query.elapsed() >= 60);
        QVERIFY(query.elapsed() < 4000);
        QCOMPARE(callbacks, 1);
        QCOMPARE(native.queries.size(), 1);
        QCOMPARE(native.queries.front().first, queryParameter);
        QCOMPARE(native.queries.front().second, otherFlags);
        QVERIFY(sessionend::active());
        QVERIFY(!sessionend::RecoveryWrite::begin());
        sendKey(hwnd);
        QTest::qWait(30);
        QCOMPARE(native.keyPresses, 1);
        bool delivered = false;
        window->whenTabsCanChange([&] { delivered = true; });
        QTest::qWait(30);
        QVERIFY(!delivered);
        query.restart();
        QCOMPARE(SendMessage(hwnd, WM_QUERYENDSESSION, queryParameter, flags), LRESULT(TRUE));
        QCOMPARE(SendMessage(other, WM_QUERYENDSESSION, 0, flags), LRESULT(TRUE));
        QVERIFY(query.elapsed() < 1000);
        QCOMPARE(callbacks, 1);
        QCOMPARE(native.queries.size(), 2);
        QCOMPARE(second.queries.size(), 1);
        QCOMPARE(second.queries.front().second, otherFlags);
        SendMessage(hwnd, WM_ENDSESSION, FALSE, flags);
        QVERIFY(!sessionend::active());
        QVERIFY(sessionend::RecoveryWrite::begin());
        QTRY_VERIFY_WITH_TIMEOUT(delivered, 1000);
        sendKey(hwnd);
        QTRY_COMPARE_WITH_TIMEOUT(native.keyPresses, 2, 1000);

        cancel = true;
        QCOMPARE(SendMessage(hwnd, WM_QUERYENDSESSION, 0, flags), LRESULT(FALSE));
        QCOMPARE(SendMessage(other, WM_QUERYENDSESSION, 0, flags), LRESULT(FALSE));
        QCOMPARE(callbacks, 2);
        QVERIFY(sessionend::active());
        SendMessage(hwnd, WM_ENDSESSION, FALSE, flags);
        QVERIFY(!sessionend::active());
        QVERIFY(sessionend::RecoveryWrite::begin());
        sendKey(hwnd);
        QTRY_COMPARE_WITH_TIMEOUT(native.keyPresses, 3, 1000);

        cancel = false;
        QCOMPARE(SendMessage(hwnd, WM_QUERYENDSESSION, queryParameter, otherFlags), LRESULT(TRUE));
        QCOMPARE(callbacks, 3);
        QCOMPARE(native.queries.back().first, queryParameter);
        QCOMPARE(native.queries.back().second, otherFlags);
    }

    // Checks native timeout allows end with protection still pending and admission closed.
    void nativeTimeoutAllowsEndWithinFourSeconds()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        WindowManager manager;
        NativeSessionWindow native;
        native.create();
        const auto hwnd = reinterpret_cast<HWND>(native.winId());
        auto protection = sessionend::RecoveryWrite::begin();
        QVERIFY(protection);
        const auto reset = qScopeGuard([&] { SendMessage(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF); });
        bool withinLimit = false;
        qint64 lastWait = 0;
        // Independent query/cancel trials tolerate a single scheduler pause while
        // still requiring an observed return within the actual four-second limit.
        for (int attempt = 0; attempt < 3 && !withinLimit; ++attempt) {
            QElapsedTimer query;
            query.start();
            const LRESULT answer = SendMessage(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF);
            lastWait = query.elapsed();
            QCOMPARE(answer, LRESULT(TRUE));
            QVERIFY(lastWait >= sessionend::kGraceMs - 20);
            qInfo() << "Native logoff query returned after" << lastWait << "ms";
            QVERIFY(protection->pending());
            QVERIFY(sessionend::active());
            QVERIFY(!sessionend::RecoveryWrite::begin());
            SendMessage(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
            QVERIFY(!sessionend::active());
            auto admitted = sessionend::RecoveryWrite::begin();
            QVERIFY(admitted);
            admitted->finish();
            withinLimit = lastWait < 4000;
        }
        QVERIFY2(withinLimit, qPrintable(QStringLiteral(
            "No native query returned within four seconds. Last wait was %1 ms.").arg(lastWait)));
    }

    // Checks that successful native end emits Qt's quit signal once in an isolated child.
    void nativeSuccessfulEndPreservesQtQuitSignalOnce()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--native-successful-session-child")});
        QVERIFY(child.waitForStarted(10000));
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }

    // Checks that logoff query returns while a buffered C stream stays locked on a worker.
    void nativeLogoffDoesNotFlushLockedBufferedWriter()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        WindowManager manager;
        NativeSessionWindow native;
        native.create();
        const auto hwnd = reinterpret_cast<HWND>(native.winId());
        const auto reset = qScopeGuard([&] { SendMessage(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF); });
        const auto path = profile_->filePath(QStringLiteral("buffered.bin")).toStdWString();
        FILE *stream = _wfopen(path.c_str(), L"wb");
        QVERIFY(stream);
        const auto close = qScopeGuard([&] { fclose(stream); });
        QCOMPARE(setvbuf(stream, nullptr, _IOFBF, BUFSIZ), 0);
        QCOMPARE(fwrite("held", 1, 4, stream), size_t(4));
        std::latch locked(1);
        std::jthread writer([&] {
            _lock_file(stream);
            locked.count_down();
            QThread::msleep(1500);
            _unlock_file(stream);
        });
        locked.wait();
        QElapsedTimer query;
        query.start();
        QCOMPARE(SendMessage(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF), LRESULT(TRUE));
        QVERIFY(query.elapsed() < 1000);
        QVERIFY(sessionend::active());
        QCOMPARE(native.queries.size(), 1);
        QCOMPARE(native.queries.front().second, LPARAM(0));
    }

#endif
};

// Runs isolated native outcome mode or the workflow suite under QApplication.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (QCoreApplication::arguments().contains(QStringLiteral("--native-successful-session-child")))
        return verifySuccessfulNativeOutcome();
#endif
    TstSessionEndWorkflow test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_session_end_workflow.moc"
