#include "app/Relaunch.h"
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
#include "recent/RecentStore.h"
#include "render/AnnotModel.h"
#include "render/Document.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"
#include "TestWaits.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <aclapi.h>
#  include <winioctl.h>
#endif

#ifdef Q_OS_UNIX
#  include <unistd.h>
#endif

using namespace mervin;
using ipc::Handoff;
using ipc::Message;
using ipc::SingleInstanceServer;
using Start = SingleInstanceServer::Start;

namespace {

// A group of its own, so the test never meets a running copy.
const QString kGroup = QStringLiteral("MervinPDF-unittest-wm");

#ifdef Q_OS_UNIX
// A template for the suite's own folders, short so that their socket paths fit whatever
// the temp folder the suite runs with.
const QString kShortTemp = QStringLiteral("/tmp/mih-XXXXXX");
#endif

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

#ifdef Q_OS_WIN
// Fix ownership explicitly because elevated runners can default to Administrators.
bool setLegacyFixtureOwner(const QString &path)
{
    const QByteArray user = ipc::currentUser();
    if (user.isEmpty())
        return false;
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                 const_cast<char *>(user.constData()), nullptr, nullptr, nullptr)
        == ERROR_SUCCESS;
}

// Give the fixture a known legacy ACL, including Administrators and inheritance.
bool setLegacyFixtureAcl(const QString &path, bool everyone = false)
{
    const QByteArray user = ipc::currentUser();
    BYTE system[SECURITY_MAX_SID_SIZE];
    BYTE administrators[SECURITY_MAX_SID_SIZE];
    BYTE world[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(system);
    DWORD administratorsSize = sizeof(administrators);
    DWORD worldSize = sizeof(world);
    if (user.isEmpty() || !CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize)
        || !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                               administrators, &administratorsSize)
        || !CreateWellKnownSid(WinWorldSid, nullptr, world, &worldSize))
        return false;
    EXPLICIT_ACCESSW entries[4]{};
    const PSID sids[]{const_cast<char *>(user.constData()), system, administrators, world};
    for (int i = 0; i < 4; ++i) {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
    }
    PACL acl = nullptr;
    if (SetEntriesInAclW(everyone ? 4 : 3, entries, nullptr, &acl) != ERROR_SUCCESS)
        return false;
    const auto freeAcl = qScopeGuard([&] { LocalFree(acl); });
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
                                     | PROTECTED_DACL_SECURITY_INFORMATION,
                                 const_cast<char *>(user.constData()), nullptr, acl, nullptr)
        == ERROR_SUCCESS;
}

// Isolate the old TEMP path and restore both environment variables before cleanup.
struct LegacyTempFixture
{
    QTemporaryDir root;
    QString path;
    const QByteArray previousTemp = qgetenv("TEMP");
    const QByteArray previousTmp = qgetenv("TMP");

    // Inherit the known root ACL without protecting the simulated TEMP's DACL.
    bool create()
    {
        if (!root.isValid() || !setLegacyFixtureAcl(root.path()))
            return false;
        path = root.filePath(QStringLiteral("temp"));
        if (!QDir().mkdir(path) || !setLegacyFixtureOwner(path))
            return false;
        qputenv("TEMP", QFile::encodeName(path));
        qputenv("TMP", QFile::encodeName(path));
        return QDir::tempPath() == path;
    }

    ~LegacyTempFixture()
    {
        if (previousTemp.isNull())
            qunsetenv("TEMP");
        else
            qputenv("TEMP", previousTemp);
        if (previousTmp.isNull())
            qunsetenv("TMP");
        else
            qputenv("TMP", previousTmp);
    }
};

// Write the old format independently of the current carry writer.
bool writeLegacyFixture(const QString &path, const QList<Message> &messages)
{
    QJsonArray launches;
    for (const Message &message : messages)
        launches.append(message.toJson());
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("written"), QDateTime::currentMSecsSinceEpoch()},
        {QStringLiteral("launches"), launches}}).toJson(QJsonDocument::Compact);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return false;
    file.close();
    return setLegacyFixtureOwner(path);
}

// Build a directory junction without requiring the symbolic-link privilege.
bool makeLegacyFixtureJunction(const QString &path, const QString &target)
{
    if (!QDir().mkdir(path))
        return false;
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE directory = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                         nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return false;
    const auto close = qScopeGuard([&] { CloseHandle(directory); });
    const std::wstring print = QDir::toNativeSeparators(target).toStdWString();
    const std::wstring substitute = L"\\??\\" + print;
    struct JunctionHeader {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
    };
    static_assert(sizeof(JunctionHeader) == 16);
    const WORD substituteBytes = WORD(substitute.size() * sizeof(wchar_t));
    const WORD printBytes = WORD(print.size() * sizeof(wchar_t));
    const WORD length = WORD(8 + substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t));
    const JunctionHeader header{IO_REPARSE_TAG_MOUNT_POINT, length, 0, 0, substituteBytes,
                                WORD(substituteBytes + sizeof(wchar_t)), printBytes};
    QByteArray data(reinterpret_cast<const char *>(&header), sizeof(header));
    data.append(reinterpret_cast<const char *>(substitute.c_str()), substituteBytes + sizeof(wchar_t));
    data.append(reinterpret_cast<const char *>(print.c_str()), printBytes + sizeof(wchar_t));
    DWORD written = 0;
    return DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, data.data(), DWORD(data.size()),
                           nullptr, 0, &written, nullptr);
}
#endif

} // namespace

// Checks how WindowManager uses the single-instance server: the opens it takes, when it
// stops taking them, and which files a standalone copy leaves alone.
class TstInstanceHandoff : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void opensWaitingBeforeAdoptionOpen();
    void finalQuitStopsHandoffs_data();
    void finalQuitStopsHandoffs();
    void exitWaitKeepsLockWithoutEndpoint();
    void writesInFlightKeepTheLock();
    void standaloneWritesNoStores_data();
    void standaloneWritesNoStores();
    void realLaunchSelectsStateOwnership_data();
    void realLaunchSelectsStateOwnership();
    void standaloneSavesNoMounts_data();
    void standaloneSavesNoMounts();
    void launchBeforeFinalQuitGoesToNextCopy_data();
    void launchBeforeFinalQuitGoesToNextCopy();
    void quitWithNothingQueuedStartsNoCopy();
    void failedCarryWritePassesFilesAsArguments();
    void carryFileKeepsEveryPath();
    void primaryOpensCarriedLaunches();
#ifdef Q_OS_WIN
    void windowsLegacyCarryKeepsOrder();
    void windowsLegacyCarryWaitsForLock();
    void windowsLegacyCarryFailures_data();
    void windowsLegacyCarryFailures();
    void windowsLegacyCarryRejectsUnsafePaths_data();
    void windowsLegacyCarryRejectsUnsafePaths();
#endif
#ifdef Q_OS_UNIX
    void carryFileInAnotherFolder_data();
    void carryFileInAnotherFolder();
#endif
    void carryFileDateBounds_data();
    void carryFileDateBounds();
    void cancelledQuitOpensHeldLaunch_data();
    void cancelledQuitOpensHeldLaunch();

private:
    // A started server for kGroup, or null after a failed check.
    std::unique_ptr<SingleInstanceServer> startedServer();

#ifdef Q_OS_UNIX
    // Stand in for /run/user, /tmp and the cache folder, so that nothing reaches the
    // user's own. Each test has a profile folder of its own (init).
    QTemporaryDir runtime_{kShortTemp};
    QTemporaryDir sockets_{kShortTemp};
    QTemporaryDir cache_{kShortTemp};
    std::optional<QByteArray> cacheHome_; // the suite's XDG_CACHE_HOME, if it had one
#endif
    std::unique_ptr<QTemporaryDir> profile_;
    QString pdf_;
};

void TstInstanceHandoff::initTestCase()
{
#ifdef Q_OS_UNIX
    QVERIFY(runtime_.isValid() && sockets_.isValid() && cache_.isValid());
    // Keeps the lock, socket and carry file out of the user's runtime folder, using a
    // folder of its own in place of /run/user/<uid>, a socket fallback out of /tmp, and
    // anything the code under test caches out of the user's cache folder.
    const QString dir = QDir(runtime_.path()).filePath(QString::number(uint(::geteuid())));
    QVERIFY(QDir().mkpath(dir));
    QVERIFY(QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    ipc::setUserRuntimeRootForTests(runtime_.path());
    ipc::setSocketRootForTests(sockets_.path());
    if (qEnvironmentVariableIsSet("XDG_CACHE_HOME"))
        cacheHome_ = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache_.path()));
    // Checked with a profile folder of the suite's own, so that no fallback reaches the
    // user's profile either.
    const QTemporaryDir profile(kShortTemp);
    ConfigPaths::setOverrideDir(profile.path());
    const QString lockFile = ipc::instancePaths(kGroup).lockFile;
    ConfigPaths::setOverrideDir({});
    QVERIFY2(lockFile.startsWith(dir + QLatin1Char('/')),
             qPrintable(QStringLiteral("the lock is at %1, outside the suite's folder")
                            .arg(lockFile)));
#endif
}

void TstInstanceHandoff::cleanupTestCase()
{
#ifdef Q_OS_UNIX
    ipc::setUserRuntimeRootForTests({});
    ipc::setSocketRootForTests({});
    if (cacheHome_)
        qputenv("XDG_CACHE_HOME", *cacheHome_);
    else
        qunsetenv("XDG_CACHE_HOME");
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
    // Nothing a test carried reaches the next one.
    relaunch::writeCarried(QDir(profile_->path()).filePath(QStringLiteral("left.carried")));
    relaunch::nextCopyStarted();
    QFile::remove(ipc::instancePaths(kGroup).carriedFile);
    SingleInstanceServer::releaseKeptLocksForTests();
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
        QCOMPARE(manager.tryCloseAllForUpdate(), Updater::CloseResult::Closed);
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

// A primary whose own writes are still in flight when its exit wait ends keeps the lock
// until the process ends. The next copy can then neither read the files before those
// writes land nor write over them.
void TstInstanceHandoff::writesInFlightKeepTheLock()
{
    const auto release = qScopeGuard([] {
        appfiles::testing::hold(false);
        SingleInstanceServer::releaseKeptLocksForTests();
    });
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    auto manager = std::make_unique<WindowManager>();
    manager->adoptInstanceServer(std::move(server));
    // A write that cannot start outlasts the exit wait.
    appfiles::testing::hold(true);
    appfiles::write(QDir(profile_->path()).filePath(QStringLiteral("held.json")), "{}");
    manager.reset();
    QVERIFY(!appfiles::flush(0));
    QVERIFY(!endpointAnswers());
    QVERIFY(lockHeld());

    // A launch now waits for this process as for any live holder, and gives up as beside
    // one, without becoming the primary.
    SingleInstanceServer launch;
    QCOMPARE(ipc::claimOrHandOff(launch, Message::open({pdf_}), 300, kGroup),
             ipc::Launch::Standalone);
    QVERIFY(!launch.isListening());

    // The end of the process releases the lock.
    appfiles::testing::hold(false);
    QVERIFY(appfiles::flush(10000));
    SingleInstanceServer::releaseKeptLocksForTests();
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

void TstInstanceHandoff::realLaunchSelectsStateOwnership_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("primary") << 0;
    QTest::newRow("delivered") << 1;
    QTest::newRow("standalone") << 2;
}

// Exercise main's actual launch decision in another process with this test's profile.
void TstInstanceHandoff::realLaunchSelectsStateOwnership()
{
    QFETCH(int, mode);
    QVERIFY(appfiles::flush(10000));
#ifdef Q_OS_UNIX
    // The child has the real runtime-root configuration. The profile hash still
    // isolates these endpoints from every ordinary application instance.
    ipc::setUserRuntimeRootForTests({});
    ipc::setSocketRootForTests({});
    const auto restoreRoots = qScopeGuard([&] {
        ipc::setUserRuntimeRootForTests(runtime_.path());
        ipc::setSocketRootForTests(sockets_.path());
    });
#endif
    const ipc::InstancePaths paths = ipc::instancePaths();
    QVERIFY(!paths.lockFile.isEmpty());
    SingleInstanceServer primary;
    QLockFile held(paths.lockFile);
    held.setStaleLockTime(0);
    if (mode == 1)
        QCOMPARE(primary.start(), Start::Started);
    if (mode == 2)
        QVERIFY(held.tryLock());
    QFile config(ConfigPaths::configFile());
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QByteArray original = config.readAll();
    config.close();
    QProcess child;
    const auto stopChild = qScopeGuard([&] {
        if (child.state() != QProcess::NotRunning) {
            child.kill();
            child.waitForFinished(5000);
        }
    });
    child.start(QStringLiteral(MERVIN_APP_EXE), {
        QStringLiteral("--profile"), profile_->path(), QStringLiteral("--language=en"),
        QStringLiteral("--quit-after-startup"), pdf_,
    });
    QVERIFY(child.waitForStarted(5000));
    QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, ipc::kLaunchWaitMs + 15000);
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    const QByteArray diagnostics = child.readAllStandardError();
    QCOMPARE(child.exitCode(), 0);
    if (mode == 1) {
        const QList<Message> opens = primary.takeOpens();
        QCOMPARE(opens.size(), 1);
        QCOMPARE(opens.front().paths, QStringList{pdf_});
    }
    if (mode == 2)
        QVERIFY2(diagnostics.contains("running copy does not answer"), diagnostics.constData());
    const QString recent = QDir(profile_->path()).filePath(QStringLiteral("recent.json"));
    QCOMPARE(QFile::exists(recent), mode == 0);
    if (mode != 0) {
        QVERIFY(config.open(QIODevice::ReadOnly));
        QCOMPARE(config.readAll(), original);
        QVERIFY(!QFile::exists(QDir(profile_->path()).filePath(QStringLiteral("session.json"))));
    }
}

void TstInstanceHandoff::standaloneSavesNoMounts_data()
{
    QTest::addColumn<bool>("ownsStores");
    QTest::addColumn<bool>("onQuit"); // saved on the way out, else by the batching timer
    QTest::newRow("primary, by the timer") << true << false;
    QTest::newRow("standalone, by the timer") << false << false;
    QTest::newRow("primary, on quit") << true << true;
    QTest::newRow("standalone, on quit") << false << true;
}

// Mounts found for Recent entries are saved after kMountSaveMs, or on the way out, but
// only by the copy that owns Recent. A standalone copy leaves recent.json as the primary
// wrote it.
void TstInstanceHandoff::standaloneSavesNoMounts()
{
    QFETCH(bool, ownsStores);
    QFETCH(bool, onQuit);
    Settings settings = Settings::load();
    settings.recentKeepMissing = true; // no prune pass that would save at its end
    QVERIFY(settings.save());
    const QString path = QFileInfo(pdf_).canonicalFilePath();
    RecentStore recent; // as an older version wrote it, without the mount
    recent.add(path, QDateTime::currentMSecsSinceEpoch());
    QVERIFY(recent.save(RecentStore::defaultFile()));
    QFile file(RecentStore::defaultFile());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();

    {
        WindowManager manager(ownsStores);
        QSignalSpy checked(manager.fileProbe(), &FileProbe::checked);
        manager.fileProbe()->check({{path, QString()}});
        QVERIFY(checked.wait(10000));
        // Found, and waiting for the save that kMountSaveMs delays.
        if (!onQuit)
            QTest::qWait(WindowManager::kMountSaveMs + 300);
    }
    QVERIFY(appfiles::flush(10000));

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray after = file.readAll();
    if (!ownsStores) {
        QCOMPARE(after, before);
        return;
    }
    RecentStore onDisk;
    QVERIFY(onDisk.load(RecentStore::defaultFile()));
    QCOMPARE(onDisk.recordedMount(path), RecentStore::mountOf(path));
}

void TstInstanceHandoff::launchBeforeFinalQuitGoesToNextCopy_data()
{
    QTest::addColumn<int>("path");
    QTest::newRow("Quit") << 0;
    QTest::newRow("close for an update") << 1;
    QTest::newRow("WindowManager destroyed") << 2;
    QTest::newRow("last window closed") << 3;
}

// The server thread can acknowledge a launch just before a quit becomes final, before the
// GUI thread got to the open. This copy then does not open it, and the copy it starts
// after it exits gets the files.
void TstInstanceHandoff::launchBeforeFinalQuitGoesToNextCopy()
{
    QFETCH(int, path);
    int starts = 0;
    QStringList passed;
    relaunch::setStarterForTests([&](const QStringList &paths) {
        ++starts;
        passed = paths;
        return true;
    });
    const auto restore = qScopeGuard([] { relaunch::setStarterForTests({}); });
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    auto manager = std::make_unique<WindowManager>();
    manager->adoptInstanceServer(std::move(server));
    const QPointer<MainWindow> window = manager->createWindow();

    // Acknowledged, while the GUI thread has not run since.
    QCOMPARE(offer(pdf_), Handoff::Delivered);
    switch (path) {
    case 0:
        manager->quitAll();
        break;
    case 1:
        QCOMPARE(manager->tryCloseAllForUpdate(), Updater::CloseResult::Closed);
        break;
    case 2:
        manager.reset();
        break;
    case 3:
        window->close();
        break;
    }
    if (manager) {
        QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
        QTest::qWait(200);
        QVERIFY(!shows(*manager, pdf_));
        manager.reset();
    }
    // Written next to the lock before this copy let go of it.
    const QString carried = ipc::instancePaths(kGroup).carriedFile;
    QVERIFY(QFile::exists(carried));
    QVERIFY(relaunch::carrying());
    QCOMPARE(starts, 0);

    // As main() does once runUi has returned. The file, not the command line, has them.
    QVERIFY(relaunch::startIfDue(true));
    QCOMPARE(starts, 1);
    QVERIFY(passed.isEmpty());
    QVERIFY(!relaunch::carrying());

    // The copy that becomes the primary next opens the file and removes the carry file.
    std::unique_ptr<SingleInstanceServer> next = startedServer();
    QVERIFY(next);
    WindowManager nextManager;
    nextManager.adoptInstanceServer(std::move(next));
    QTRY_VERIFY_WITH_TIMEOUT(shows(nextManager, pdf_), 10000);
    QVERIFY(!QFile::exists(carried));
    nextManager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(nextManager.openTabPaths().isEmpty(), 10000);
}

// Without a launch waiting, a quit starts no copy.
void TstInstanceHandoff::quitWithNothingQueuedStartsNoCopy()
{
    int starts = 0;
    relaunch::setStarterForTests([&](const QStringList &) {
        ++starts;
        return true;
    });
    const auto restore = qScopeGuard([] { relaunch::setStarterForTests({}); });
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    {
        WindowManager manager;
        manager.adoptInstanceServer(std::move(server));
        const QPointer<MainWindow> window = manager.createWindow();
        manager.quitAll();
        QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
    }
    QVERIFY(!relaunch::carrying());
    QVERIFY(!QFile::exists(ipc::instancePaths(kGroup).carriedFile));
    QVERIFY(relaunch::startIfDue(true));
    QCOMPARE(starts, 0);
}

// Carried launches stay until a carry file holds them. If ~WindowManager cannot write it,
// the copy that startIfDue starts gets their files as arguments.
void TstInstanceHandoff::failedCarryWritePassesFilesAsArguments()
{
    const QDir profile(profile_->path());
    relaunch::carry(Message::open({pdf_}, QStringLiteral("new-tab")));
    QVERIFY(!relaunch::writeCarried(profile.filePath(QStringLiteral("missing/group.carried"))));
    const QString file = profile.filePath(QStringLiteral("group.carried"));
    QVERIFY(relaunch::writeCarried(file));
    QCOMPARE(relaunch::takeCarried(file).size(), 1);
    relaunch::nextCopyStarted();

    // A folder in the carry file's place makes the destructor's write fail.
    const QString carried = ipc::instancePaths(kGroup).carriedFile;
    QVERIFY(QDir().mkdir(carried));
    const auto removeFolder = qScopeGuard([&] { QDir().rmdir(carried); });
    int starts = 0;
    QStringList passed;
    relaunch::setStarterForTests([&](const QStringList &paths) {
        ++starts;
        passed = paths;
        return true;
    });
    const auto restore = qScopeGuard([] { relaunch::setStarterForTests({}); });
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    auto manager = std::make_unique<WindowManager>();
    manager->adoptInstanceServer(std::move(server));
    const QPointer<MainWindow> window = manager->createWindow();
    QCOMPARE(offer(pdf_), Handoff::Delivered);
    manager->quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
    manager.reset();
    QVERIFY(QFileInfo(carried).isDir());
    QVERIFY(relaunch::carrying());

    // As main() does, here after writes that did not land, which allow no restart.
    QVERIFY(relaunch::startIfDue(false));
    QCOMPARE(starts, 1);
    QCOMPARE(passed, QStringList{pdf_});
    // Given away once.
    QVERIFY(!relaunch::carrying());
    QVERIFY(relaunch::writeCarried(file));
    QVERIFY(!QFile::exists(file));
}

// The carry file keeps any path, in any number, as UTF-8 JSON that only this user can read,
// and adds to launches a previous primary left there.
void TstInstanceHandoff::carryFileKeepsEveryPath()
{
    const QString file = QDir(profile_->path()).filePath(QStringLiteral("group.carried"));
    QVERIFY(relaunch::writeCarried(file)); // nothing carried, nothing written
    QVERIFY(!QFile::exists(file));

    QStringList many;
    for (int i = 0; i < 3000; ++i)
        many.append(QStringLiteral("/srv/scans/2026/%1/scan %2.pdf").arg(i % 37).arg(i));
    const QList<Message> first = {
        Message::open({QStringLiteral(u"/home/a/Pr\u00fcfung \u8cc7\u6599.pdf"),
                       QStringLiteral("C:\\a&b\\100%, \"x\".pdf")},
                      QStringLiteral("new-window")),
        Message::open(many, QStringLiteral("new-tab")),
    };
    for (const Message &open : first)
        relaunch::carry(open);
    QVERIFY(relaunch::writeCarried(file));
#ifdef Q_OS_UNIX
    QCOMPARE(QFile::permissions(file) & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                                         | QFileDevice::WriteGroup | QFileDevice::WriteOther),
             QFileDevice::Permissions());
#endif
    relaunch::carry(Message::open({})); // a launch without files, after the others
    QVERIFY(relaunch::writeCarried(file));

    const QList<Message> read = relaunch::takeCarried(file);
    QCOMPARE(read.size(), 3);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(read.at(i).paths, first.at(i).paths);
        QCOMPARE(read.at(i).behavior, first.at(i).behavior);
    }
    QVERIFY(read.at(2).paths.isEmpty());
    QVERIFY(!QFile::exists(file));
    QVERIFY(relaunch::takeCarried(file).isEmpty());

    // A copy that whatever started it will open them is not started again.
    QVERIFY(relaunch::carrying());
    relaunch::nextCopyStarted();
    int starts = 0;
    relaunch::setStarterForTests([&](const QStringList &) {
        ++starts;
        return true;
    });
    const auto restore = qScopeGuard([] { relaunch::setStarterForTests({}); });
    QVERIFY(relaunch::startIfDue(true));
    QCOMPARE(starts, 0);
}

// The primary takes the launches the previous one left in the carry file when it takes over
// the server, opens them and removes the file.
void TstInstanceHandoff::primaryOpensCarriedLaunches()
{
    const QString named =
        QDir(profile_->path()).filePath(QStringLiteral(u"Pr\u00fcfung \u8cc7\u6599.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), named));
    const QString carried = ipc::instancePaths(kGroup).carriedFile;
    relaunch::carry(Message::open({named, pdf_}, QStringLiteral("new-tab")));
    QVERIFY(relaunch::writeCarried(carried));
    relaunch::nextCopyStarted();

    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    QTRY_VERIFY_WITH_TIMEOUT(shows(manager, named) && shows(manager, pdf_), 10000);
    QVERIFY(!QFile::exists(carried));
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(manager.openTabPaths().isEmpty(), 10000);
}

#ifdef Q_OS_WIN
// Default group naming exercises the old username hash and the new SID hash.
void TstInstanceHandoff::windowsLegacyCarryKeepsOrder()
{
    LegacyTempFixture temp;
    QVERIFY(temp.create());
    const auto current = ipc::instancePaths();
    const auto legacy = ipc::legacyInstancePaths();
    QCOMPARE(legacy.carriedFile,
             QDir(temp.path).filePath(ipc::legacyEndpoint() + QStringLiteral(".carried")));
    QVERIFY(current.carriedFile != legacy.carriedFile);
    QVERIFY(current.server != legacy.server);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    const std::wstring nativeTemp = QDir::toNativeSeparators(temp.path).toStdWString();
    QCOMPARE(GetNamedSecurityInfoW(nativeTemp.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, &acl, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
    const auto freeDescriptor = qScopeGuard([&] { LocalFree(descriptor); });
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision));
    QVERIFY(!(control & SE_DACL_PROTECTED));
    QVERIFY(acl && acl->AceCount == 3);
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void *raw = nullptr;
        QVERIFY(GetAce(acl, index, &raw));
        QVERIFY(static_cast<ACE_HEADER *>(raw)->AceFlags & INHERITED_ACE);
    }

    const QList<Message> expected{
        Message::open({QStringLiteral("C:/current.pdf")}, QStringLiteral("new-tab")),
        Message::open({QStringLiteral(u"C:/Pr\u00fcfung \u8cc7\u6599.pdf")},
                      QStringLiteral("new-window")),
        Message::open({}, QStringLiteral("new-window"))};
    QVERIFY(writeLegacyFixture(current.carriedFile, {expected.first()}));
    QVERIFY(writeLegacyFixture(legacy.carriedFile, expected.mid(1)));
    SingleInstanceServer server;
    QVERIFY(server.carriedFiles().isEmpty());
    QCOMPARE(server.start(), Start::Started);
    const QStringList files{current.carriedFile, legacy.carriedFile};
    QCOMPARE(server.carriedFiles(), files);
    QLockFile excluded(legacy.lockFile);
    excluded.setStaleLockTime(0);
    QVERIFY(!excluded.tryLock(0));
    QList<Message> consumed;
    for (const QString &file : server.carriedFiles())
        consumed += relaunch::takeCarried(file);
    QCOMPARE(consumed.size(), expected.size());
    for (qsizetype index = 0; index < expected.size(); ++index) {
        QCOMPARE(consumed.at(index).paths, expected.at(index).paths);
        QCOMPARE(consumed.at(index).behavior, expected.at(index).behavior);
    }
    for (const QString &file : files) {
        QVERIFY(!QFile::exists(file));
        QVERIFY(relaunch::takeCarried(file).isEmpty());
    }
    server.keepLockUntilExit();
    QVERIFY(server.carriedFiles().isEmpty());
}

// A silent old primary still owns its carry file until it releases its lock.
void TstInstanceHandoff::windowsLegacyCarryWaitsForLock()
{
    LegacyTempFixture temp;
    QVERIFY(temp.create());
    const auto current = ipc::instancePaths();
    const auto legacy = ipc::legacyInstancePaths();
    QVERIFY(writeLegacyFixture(legacy.carriedFile,
                               {Message::open({pdf_}, QStringLiteral("new-tab"))}));
    QLockFile oldPrimary(legacy.lockFile);
    oldPrimary.setStaleLockTime(0);
    QVERIFY(oldPrimary.tryLock(0));
    QVERIFY(setLegacyFixtureOwner(legacy.lockFile));
    auto server = std::make_unique<SingleInstanceServer>();
    QCOMPARE(server->start(), Start::Started);
    QCOMPARE(server->carriedFiles(), QStringList{current.carriedFile});
    QVERIFY(QFile::exists(legacy.carriedFile));
    SingleInstanceServer secondary;
    QCOMPARE(secondary.start(), Start::Running);
    QVERIFY(secondary.carriedFiles().isEmpty());
    oldPrimary.unlock();
    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    QTRY_VERIFY_WITH_TIMEOUT(shows(manager, pdf_), 10000);
    QVERIFY(!QFile::exists(legacy.carriedFile));
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(manager.openTabPaths().isEmpty(), 10000);
}

void TstInstanceHandoff::windowsLegacyCarryFailures_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("malformed JSON") << QStringLiteral("format");
    QTest::newRow("read sharing denied") << QStringLiteral("read");
    QTest::newRow("delete sharing denied") << QStringLiteral("remove");
}

// Failed consumption must preserve bytes and permit a later primary to retry.
void TstInstanceHandoff::windowsLegacyCarryFailures()
{
    QFETCH(QString, failure);
    LegacyTempFixture temp;
    QVERIFY(temp.create());
    const auto legacy = ipc::legacyInstancePaths();
    const QList<Message> messages{Message::open({pdf_}, QStringLiteral("new-window"))};
    QVERIFY(writeLegacyFixture(legacy.carriedFile, messages));
    QFile file(legacy.carriedFile);
    if (failure == QLatin1String("format")) {
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("{\"written\":"), qint64(11));
        file.close();
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    HANDLE held = INVALID_HANDLE_VALUE;
    const auto close = qScopeGuard([&] {
        if (held != INVALID_HANDLE_VALUE)
            CloseHandle(held);
    });
    if (failure != QLatin1String("format")) {
        const std::wstring native = QDir::toNativeSeparators(legacy.carriedFile).toStdWString();
        held = CreateFileW(native.c_str(), GENERIC_READ,
                            failure == QLatin1String("read") ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
    }
    {
        SingleInstanceServer server;
        QCOMPARE(server.start(), Start::Started);
        for (const QString &path : server.carriedFiles())
            QVERIFY(relaunch::takeCarried(path).isEmpty());
        QVERIFY(QFile::exists(legacy.carriedFile));
    }
    if (held != INVALID_HANDLE_VALUE) {
        CloseHandle(held);
        held = INVALID_HANDLE_VALUE;
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    if (failure == QLatin1String("format"))
        QVERIFY(writeLegacyFixture(legacy.carriedFile, messages));
    SingleInstanceServer retry;
    QCOMPARE(retry.start(), Start::Started);
    QList<Message> consumed;
    for (const QString &path : retry.carriedFiles())
        consumed += relaunch::takeCarried(path);
    QCOMPARE(consumed.size(), 1);
    QCOMPARE(consumed.first().paths, messages.first().paths);
    QCOMPARE(consumed.first().behavior, messages.first().behavior);
    QVERIFY(!QFile::exists(legacy.carriedFile));
}

void TstInstanceHandoff::windowsLegacyCarryRejectsUnsafePaths_data()
{
    QTest::addColumn<QString>("unsafe");
    QTest::newRow("TEMP grants Everyone access") << QStringLiteral("temp-acl");
    QTest::newRow("carry grants Everyone access") << QStringLiteral("carry-acl");
    QTest::newRow("lock grants Everyone access") << QStringLiteral("lock-acl");
    QTest::newRow("TEMP is a junction") << QStringLiteral("temp-junction");
    QTest::newRow("carry is a symbolic link") << QStringLiteral("carry-link");
}

// Source validation must not follow redirects or trust other users' permissions.
void TstInstanceHandoff::windowsLegacyCarryRejectsUnsafePaths()
{
    QFETCH(QString, unsafe);
    LegacyTempFixture temp;
    QVERIFY(temp.create());
    const auto current = ipc::instancePaths();
    const auto legacy = ipc::legacyInstancePaths();
    const QList<Message> messages{Message::open({pdf_})};
    QVERIFY(writeLegacyFixture(legacy.carriedFile, messages));
    QString preserved = legacy.carriedFile;
    QString reparse;
    const auto removeReparse = qScopeGuard([&] {
        if (!reparse.isEmpty()) {
            const std::wstring native = QDir::toNativeSeparators(reparse).toStdWString();
            if (unsafe == QLatin1String("temp-junction"))
                RemoveDirectoryW(native.c_str());
            else
                DeleteFileW(native.c_str());
        }
    });
    if (unsafe == QLatin1String("temp-acl")) {
        QVERIFY(setLegacyFixtureAcl(temp.path, true));
    } else if (unsafe == QLatin1String("carry-acl")) {
        QVERIFY(setLegacyFixtureAcl(legacy.carriedFile, true));
    } else if (unsafe == QLatin1String("lock-acl")) {
        QFile lock(legacy.lockFile);
        QVERIFY(lock.open(QIODevice::WriteOnly));
        QCOMPARE(lock.write("untouched"), qint64(9));
        lock.close();
        QVERIFY(setLegacyFixtureAcl(legacy.lockFile, true));
    } else if (unsafe == QLatin1String("temp-junction")) {
        const QString moved = temp.root.filePath(QStringLiteral("moved"));
        QVERIFY(QDir().rename(temp.path, moved));
        preserved = QDir(moved).filePath(QFileInfo(legacy.carriedFile).fileName());
        reparse = temp.path;
        QVERIFY(makeLegacyFixtureJunction(reparse, moved));
    } else {
        preserved = temp.root.filePath(QStringLiteral("target.carried"));
        QVERIFY(QFile::rename(legacy.carriedFile, preserved));
        const std::wstring native = QDir::toNativeSeparators(legacy.carriedFile).toStdWString();
        const std::wstring target = QDir::toNativeSeparators(preserved).toStdWString();
        if (!CreateSymbolicLinkW(native.c_str(), target.c_str(),
                                 SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
                QSKIP("File symlink fixture needs Developer Mode or symbolic-link privilege.");
            QFAIL(qPrintable(QString::number(GetLastError())));
        }
        reparse = legacy.carriedFile;
    }
    QFile file(preserved);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    SingleInstanceServer server;
    QCOMPARE(server.start(), Start::Started);
    QCOMPARE(server.carriedFiles(), QStringList{current.carriedFile});
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    if (unsafe == QLatin1String("lock-acl")) {
        QFile lock(legacy.lockFile);
        QVERIFY(lock.open(QIODevice::ReadOnly));
        QCOMPARE(lock.readAll(), QByteArray("untouched"));
    } else {
        QVERIFY(!QFile::exists(legacy.lockFile));
    }
}
#endif

#ifdef Q_OS_UNIX
void TstInstanceHandoff::carryFileInAnotherFolder_data()
{
    QTest::addColumn<bool>("held");
    QTest::newRow("its lock is free") << false;
    QTest::newRow("a live process holds its lock") << true;
}

// A primary that ran in another folder, as one does that started in the profile's instance
// folder before /run/user/<uid> appeared, left its carry file there. The next primary
// takes it over, unless a live process holds that folder's lock.
void TstInstanceHandoff::carryFileInAnotherFolder()
{
    QFETCH(bool, held);
    ipc::InstancePaths other;
    {
        ipc::setUserRuntimeRootForTests(runtime_.path() + QStringLiteral("/none"));
        const auto restoreRoot =
            qScopeGuard([&] { ipc::setUserRuntimeRootForTests(runtime_.path()); });
        other = ipc::instancePaths(kGroup);
    }
    QVERIFY(other.carriedFile.startsWith(profile_->path() + QStringLiteral("/instance-")));
    relaunch::carry(Message::open({pdf_}, QStringLiteral("new-tab")));
    QVERIFY(relaunch::writeCarried(other.carriedFile));
    relaunch::nextCopyStarted();
    const auto removeFile = qScopeGuard([&] { QFile::remove(other.carriedFile); });
    QLockFile holder(other.lockFile);
    if (held)
        QVERIFY(holder.tryLock(0));

    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    QVERIFY(server->carriedFile() != other.carriedFile);
    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    if (held) {
        QTest::qWait(300);
        QVERIFY(!shows(manager, pdf_));
        QVERIFY(QFile::exists(other.carriedFile));
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(shows(manager, pdf_), 10000);
    QVERIFY(!QFile::exists(other.carriedFile));
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(manager.openTabPaths().isEmpty(), 10000);
}
#endif

void TstInstanceHandoff::carryFileDateBounds_data()
{
    QTest::addColumn<qint64>("writtenMs"); // when the file says it was written, from now
    QTest::addColumn<bool>("opens");
    QTest::newRow("older than the limit") << -(relaunch::kCarriedMaxAgeMs + 1000) << false;
    QTest::newRow("a minute within the limit")
        << -(relaunch::kCarriedMaxAgeMs - 60 * 1000) << true;
    QTest::newRow("59 s ahead") << relaunch::kCarriedMaxAheadMs - 1000 << true;
    QTest::newRow("more than a minute ahead") << relaunch::kCarriedMaxAheadMs + 1000 << false;
}

// A carry file opens if it was written within kCarriedMaxAgeMs, or at most
// kCarriedMaxAheadMs ahead, as after a clock was set back a little. One outside, as a
// copy that crashed leaves it, opens nothing. Either way it goes.
void TstInstanceHandoff::carryFileDateBounds()
{
    QFETCH(qint64, writtenMs);
    QFETCH(bool, opens);
    const QString carried = ipc::instancePaths(kGroup).carriedFile;
    QJsonObject root;
    root.insert(QStringLiteral("written"),
                double(QDateTime::currentMSecsSinceEpoch() + writtenMs));
    root.insert(QStringLiteral("launches"),
                QJsonArray{Message::open({pdf_}, QStringLiteral("new-tab")).toJson()});
    QFile file(carried);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(root).toJson()) > 0);
    file.close();

    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    if (opens) {
        QTRY_VERIFY_WITH_TIMEOUT(shows(manager, pdf_), 10000);
    } else {
        QTest::qWait(300);
        QVERIFY(!shows(manager, pdf_));
    }
    QVERIFY(!QFile::exists(carried));
    manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(manager.openTabPaths().isEmpty(), 10000);
}

void TstInstanceHandoff::cancelledQuitOpensHeldLaunch_data()
{
    QTest::addColumn<int>("closePath");
    QTest::addColumn<int>("answer");
    QTest::addColumn<bool>("createSurvivor");
    QTest::newRow("Quit, cancel") << 0 << int(QMessageBox::Cancel) << false;
    QTest::newRow("update, cancel") << 1 << int(QMessageBox::Cancel) << false;
    QTest::newRow("last window, cancel") << 2 << int(QMessageBox::Cancel) << false;
    QTest::newRow("last window, discard") << 2 << int(QMessageBox::Discard) << false;
    QTest::newRow("last window, save") << 2 << int(QMessageBox::Save) << false;
    QTest::newRow("window added during discard prompt") << 2 << int(QMessageBox::Discard) << true;
    QTest::newRow("window added during save prompt") << 2 << int(QMessageBox::Save) << true;
}

// A final close holds launches through its prompt. Cancel opens them here; an
// approved close carries them or opens them in a window added during the prompt.
void TstInstanceHandoff::cancelledQuitOpensHeldLaunch()
{
    QFETCH(int, closePath);
    QFETCH(int, answer);
    QFETCH(bool, createSurvivor);
    std::unique_ptr<SingleInstanceServer> server = startedServer();
    QVERIFY(server);
    WindowManager manager;
    manager.adoptInstanceServer(std::move(server));
    const QPointer<MainWindow> window = manager.createWindow();
    QVERIFY(testing::openAndWait(*window, pdf_));
    auto *tabs = window->findChild<QTabWidget *>();
    const QPointer<TabPage> tab = qobject_cast<TabPage *>(tabs->currentWidget());
    QVERIFY(tab);
    QTRY_VERIFY(tab->isLoaded());
    QVERIFY(tab->viewer()->annotModel()->addTextNote(0, {40, 40}, Qt::yellow, {},
                                                     QStringLiteral("Unsaved")) >= 0);
    const QString later = QDir(profile_->path()).filePath(QStringLiteral("later.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), later));

    std::optional<Handoff> offered;
    std::optional<bool> shownWhileAsking;
    std::optional<int> tabsWhileAsking;
    QPointer<MainWindow> survivor;
    QTimer dialogs;
    connect(&dialogs, &QTimer::timeout, this, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box || shownWhileAsking)
            return;
        if (!offered) {
            offered = offer(later);
            // Dispatch the acknowledged batch while the close question is still
            // open, without depending on an arbitrary delay before Cancel.
            QMetaObject::invokeMethod(&manager, "takeInstanceOpens", Qt::DirectConnection);
            QMetaObject::invokeMethod(&manager, "openNextInstanceOpen", Qt::DirectConnection);
        }
        shownWhileAsking = shows(manager, later);
        tabsWhileAsking = tabs->count();
        if (createSurvivor)
            survivor = manager.createWindow();
        box->button(QMessageBox::StandardButton(answer))->click();
    });
    dialogs.start(5);
    if (closePath == 1)
        QCOMPARE(manager.tryCloseAllForUpdate(), Updater::CloseResult::Declined);
    else if (closePath == 2)
        window->close();
    else
        manager.quitAll();
    QTRY_VERIFY_WITH_TIMEOUT(shownWhileAsking.has_value(), 10000);
    dialogs.stop();
    QCOMPARE(offered, std::optional(Handoff::Delivered));
    QCOMPARE(shownWhileAsking, std::optional(false));
    QCOMPARE(tabsWhileAsking, std::optional(1));
    if (answer == QMessageBox::Cancel) {
        QTRY_VERIFY_WITH_TIMEOUT(shows(manager, later), 10000);
        QVERIFY(window);
        QVERIFY(!relaunch::carrying());
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(!window, 10000);
        if (createSurvivor) {
            QVERIFY(survivor);
            QTRY_VERIFY_WITH_TIMEOUT(shows(manager, later), 10000);
            QVERIFY(!relaunch::carrying());
        } else {
            QVERIFY(!shows(manager, later));
            QVERIFY(relaunch::carrying());
        }
    }
}

QTEST_MAIN(TstInstanceHandoff)
#include "tst_instance_handoff.moc"
