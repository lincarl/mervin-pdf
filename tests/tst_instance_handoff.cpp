#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "ipc/Handoff.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"
#include "recent/PathKey.h"
#include "render/Document.h"
#include "ui/MainWindow.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>
#include <optional>

using namespace mervin;
using ipc::Handoff;
using ipc::Message;
using ipc::SingleInstanceServer;
using Start = SingleInstanceServer::Start;

namespace {

// A group of its own, so the test never meets a running copy.
const QString kGroup = QStringLiteral("MervinPDF-unittest-wm");

// Offers `path` to the group's primary, as a later launch does.
Handoff offer(const QString &path)
{
    return ipc::handOff(ipc::instancePaths(kGroup).server,
                        Message::open({path}, QStringLiteral("new-tab")), QDeadlineTimer(2000));
}

// Whether a tab of `manager` shows `path`.
bool shows(const WindowManager &manager, const QString &path)
{
    const QString key = normalizePathKey(QFileInfo(path).canonicalFilePath());
    for (const QString &open : manager.openTabPaths())
        if (normalizePathKey(open) == key)
            return true;
    return false;
}

// Whether a launch could connect to the group's endpoint now.
bool endpointAnswers()
{
    QLocalSocket probe;
    probe.connectToServer(ipc::instancePaths(kGroup).server);
    const bool connected = probe.waitForConnected(100);
    probe.abort();
    return connected;
}

// Whether a live process holds the group's lock.
bool lockHeld()
{
    SingleInstanceServer other;
    return other.start(kGroup) == Start::Running;
}

} // namespace

// Checks how WindowManager uses the single-instance server: the opens it takes, when it
// stops taking them, and which files a standalone copy leaves alone.
class TstInstanceHandoff : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void opensWaitingBeforeAdoptionOpen();
    void finalQuitStopsHandoffs_data();
    void finalQuitStopsHandoffs();
    void exitWaitKeepsLockWithoutEndpoint();
    void standaloneWritesNoStores_data();
    void standaloneWritesNoStores();

private:
    // A started server for kGroup, or null after a failed check.
    std::unique_ptr<SingleInstanceServer> startedServer();

    QTemporaryDir runtime_;
    std::unique_ptr<QTemporaryDir> profile_;
    QString pdf_;
};

void TstInstanceHandoff::initTestCase()
{
    QVERIFY(runtime_.isValid());
#ifdef Q_OS_UNIX
    // Keeps the lock and socket out of the user's runtime folder.
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime_.path()));
#endif
}

void TstInstanceHandoff::init()
{
    profile_ = std::make_unique<QTemporaryDir>();
    QVERIFY(profile_->isValid());
    ConfigPaths::setOverrideDir(profile_->path());
    Settings settings;
    settings.autoUpdate = false;
    settings.closeToTray = false;
    settings.unloadInactiveMinutes = 0;
    QVERIFY(settings.save());
    pdf_ = QDir(profile_->path()).filePath(QStringLiteral("doc.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), pdf_));
}

void TstInstanceHandoff::cleanup()
{
    appfiles::testing::hold(false);
    QVERIFY(background::settle(10000));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(Document::waitForCloses(10000));
    QVERIFY(appfiles::flush(10000));
    ConfigPaths::setOverrideDir({});
    profile_.reset();
}

std::unique_ptr<SingleInstanceServer> TstInstanceHandoff::startedServer()
{
    auto server = std::make_unique<SingleInstanceServer>();
    if (server->start(kGroup) != Start::Started)
        return nullptr;
    return server;
}

// Launches the server acknowledged before WindowManager took it over open as soon as
// the event loop runs, and later ones open as they come.
void TstInstanceHandoff::opensWaitingBeforeAdoptionOpen()
{
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    QCOMPARE(offer(pdf_), Handoff::Delivered);
    // The server's notice of that open goes out now, with nobody listening.
    QTest::qWait(200);

    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    QTRY_VERIFY_WITH_TIMEOUT(shows(manager, pdf_), 10000);

    const QString later = QDir(profile_->path()).filePath(QStringLiteral("later.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), later));
    QCOMPARE(offer(later), Handoff::Delivered);
    QTRY_VERIFY_WITH_TIMEOUT(shows(manager, later), 10000);
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(manager.openTabPaths().isEmpty(), 10000);
}

void TstInstanceHandoff::finalQuitStopsHandoffs_data()
{
    QTest::addColumn<int>("path");
    QTest::newRow("Quit") << 0;
    QTest::newRow("close for an update") << 1;
    QTest::newRow("last window closed") << 2;
}

// Once a quit is final the server stops taking launches before the windows go, so no
// launch hands a file to a copy that will not show it. The lock stays with this copy.
void TstInstanceHandoff::finalQuitStopsHandoffs()
{
    QFETCH(int, path);
    std::unique_ptr<SingleInstanceServer> owned = startedServer();
    QVERIFY(owned);
    SingleInstanceServer *server = owned.get();
    WindowManager manager;
    manager.adoptInstanceServer(std::move(owned));

    // With two windows the one that goes first is not the last one, so it shows whether
    // the quit itself stopped the server, before the last-window path does.
    const QPointer<MainWindow> first = manager.createWindow();
    const QPointer<MainWindow> second = path == 2 ? nullptr : manager.createWindow();
    std::optional<bool> listeningAtFirstClose;
    for (MainWindow *window : {first.data(), second.data()})
        if (window)
            connect(window, &QObject::destroyed, this, [&] {
                if (!listeningAtFirstClose)
                    listeningAtFirstClose = server->isListening();
            });
    QVERIFY(server->isListening());

    switch (path) {
    case 0:
        manager.quitAll();
        break;
    case 1:
        QVERIFY(manager.tryCloseAllForUpdate());
        break;
    case 2:
        first->close();
        break;
    }
    QTRY_VERIFY_WITH_TIMEOUT(!first && !second, 10000);
    QCOMPARE(listeningAtFirstClose, std::optional(false));
    QVERIFY(!server->isListening());
    QVERIFY(!endpointAnswers());
    QVERIFY(lockHeld());
    QCOMPARE(offer(pdf_), Handoff::NoPrimary);
}

// During the exit wait for the last writes the endpoint is closed and the lock is still
// held, so a launch then waits and becomes the next primary.
void TstInstanceHandoff::exitWaitKeepsLockWithoutEndpoint()
{
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    auto manager = std::make_unique<WindowManager>();
    manager->adoptInstanceServer(std::move(server));
    QVERIFY(endpointAnswers());

    // A write that cannot start keeps the destructor in its exit wait.
    appfiles::testing::hold(true);
    appfiles::write(QDir(profile_->path()).filePath(QStringLiteral("held.json")), "{}");
    std::atomic<bool> endpointClosed = false;
    std::atomic<bool> heldDuringWait = false;
    const std::unique_ptr<QThread> launch(QThread::create([&] {
        QElapsedTimer timer;
        timer.start();
        while (endpointAnswers() && timer.elapsed() < 4000)
            QThread::msleep(20);
        endpointClosed = !endpointAnswers();
        heldDuringWait = lockHeld();
        appfiles::testing::hold(false);
    }));
    launch->start();
    manager.reset();
    QVERIFY(launch->wait(10000));
    QVERIFY(endpointClosed);
    QVERIFY(heldDuringWait);
    QVERIFY(!lockHeld());
}

void TstInstanceHandoff::standaloneWritesNoStores_data()
{
    QTest::addColumn<bool>("ownsStores");
    QTest::newRow("primary") << true;
    QTest::newRow("standalone") << false;
}

// A standalone copy beside a live primary reads Recent but writes none of the stores
// that primary owns.
void TstInstanceHandoff::standaloneWritesNoStores()
{
    QFETCH(bool, ownsStores);
    const QDir profile(profile_->path());
    for (const char *name : {"recent.json", "viewstate.json", "session.json"})
        QVERIFY(!profile.exists(QString::fromLatin1(name)));

    WindowManager manager(ownsStores);
    const QPointer<MainWindow> window = manager.createWindow();
    const QString canonical = QFileInfo(pdf_).canonicalFilePath();
    manager.recordOpen(canonical, false, 1);
    manager.setFavorite(canonical, true);
    manager.saveViewStates({{canonical, ViewState{}}});
    manager.updateSession();
    QVERIFY(appfiles::flush(5000));

    for (const char *name : {"recent.json", "viewstate.json", "session.json"})
        QVERIFY2(profile.exists(QString::fromLatin1(name)) == ownsStores, name);
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
}

QTEST_MAIN(TstInstanceHandoff)
#include "tst_instance_handoff.moc"
