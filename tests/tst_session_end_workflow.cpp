#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
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

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QKeyEvent>
#include <QSessionManager>
#include <QWindow>
#include <QTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <thread>

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
bool prepare(TabPage &tab, const QString &path)
{
    return QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path) && tab.open(path)
        && tab.viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                  QStringLiteral("preserved")) >= 0;
}
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
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        const auto *event = static_cast<const MSG *>(message);
        if (event->message == WM_QUERYENDSESSION)
            queries.append({event->wParam, event->lParam});
        return QWindow::nativeEvent(type, message, result);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_F12)
            ++keyPresses;
        return QWindow::event(event);
    }
};

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

class TstSessionEndWorkflow : public QObject
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
        QVERIFY(settings.save());
    }

    void cleanup()
    {
        sessionend::finishRequest(false);
        job::testing::release();
        appfiles::testing::hold(false);
        QVERIFY(background::settle(10000));
        QCoreApplication::processEvents();
        QVERIFY(Document::waitForCloses(10000));
        QVERIFY(appfiles::flush(10000));
        ConfigPaths::setOverrideDir({});
        profile_.reset();
    }

    void saveManifestCompletionReleasesGraceBeforeGuiAdoption()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), path));
        MainWindow *window = manager.createWindow();
        QVERIFY(window->openFile(path));
        auto *tab = window->findChild<TabPage *>();
        QTRY_VERIFY_WITH_TIMEOUT(tab && tab->isUsable(), 10000);
        QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                       QStringLiteral("preserved")) >= 0);
        holdManifest();
        const auto release = qScopeGuard([] { job::testing::release(); });
        QVERIFY(QMetaObject::invokeMethod(window, "saveMeasurements"));
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
        QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
        QVERIFY(!tab->hasUnsavedEdits());
        auto saved = manager.engine()->openDocument(path);
        QVERIFY(saved);
        QCOMPARE(AnnotModel(*saved).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

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

    void detachedStoppedSaveRemainsProtectedAndKeepsCommittedRecovery()
    {
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("edited.pdf"));
        WindowManager manager;
        auto tab = std::make_unique<TabPage>(manager.engine());
        QVERIFY(prepare(*tab, path));
        job::testing::holdCommitWhen([](const QString &path) {
            return path.endsWith(QStringLiteral(".pdf.json"));
        });
        const auto release = qScopeGuard([] { job::testing::release(); });
        tab->startSave({});
        QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
        tab->stopWaiting();
        tab.reset();
        QCOMPARE(manager.prepareSessionEnd(60), sessionend::Result::TimedOut);
        job::testing::release();
        manager.finishSessionEnd(true);
        manager.engine()->shutdown();
        QVERIFY(background::settle(10000));
        QVERIFY(appfiles::flush(10000));
        QCOMPARE(TabPage::recoverablePaths(), QStringList{path});
        const QDir recovery(QDir(profile_->path()).filePath(QStringLiteral("recovery")));
        const QStringList copies = recovery.entryList({QStringLiteral("*.pdf")}, QDir::Files);
        QCOMPARE(copies.size(), 1);
        auto recovered = manager.engine()->openDocument(recovery.filePath(copies.front()));
        QVERIFY(recovered);
        QCOMPARE(AnnotModel(*recovered).pageAnnots(0).front().contents, QStringLiteral("preserved"));
    }

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

    void successfulSessionEndAddsNoOrdinaryExitWait()
    {
        appfiles::testing::hold(true);
        const auto release = qScopeGuard([] { appfiles::testing::hold(false); });
        auto manager = std::make_unique<WindowManager>();
        appfiles::write(profile_->filePath(QStringLiteral("held.json")), "pending");
        QVERIFY(!appfiles::flush(0));
        QCOMPARE(manager->prepareSessionEnd(0), sessionend::Result::Settled);
        manager->finishSessionEnd(true);
        QElapsedTimer exit;
        exit.start();
        manager.reset();
        QVERIFY(exit.elapsed() < 300);
    }

#ifdef Q_OS_WIN
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
