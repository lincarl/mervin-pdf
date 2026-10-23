#include "config/ConfigPaths.h"
#include "ipc/Handoff.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMutex>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>

#ifdef Q_OS_UNIX
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

using mervin::ipc::Handoff;
using mervin::ipc::Launch;
using mervin::ipc::Message;
using mervin::ipc::SingleInstanceServer;
using Start = SingleInstanceServer::Start;

// A unique-ish name so the test never collides with a real running host.
static const QString kTestPipe = QStringLiteral("MervinPDF-unittest-si");

#ifdef Q_OS_UNIX
// A template for folders with a short path of their own, so that the socket path lengths
// in a test do not depend on the temp folder the suite runs in.
static const QString kShortTemp = QStringLiteral("/tmp/msi-XXXXXX");
#endif

namespace {
bool packaged = false;

// Where a client finds the test group's server.
QString testEndpoint()
{
    return mervin::ipc::instancePaths(kTestPipe).server;
}

// Every open `server` acknowledged within `ms`, after the event loop has run.
QList<Message> opensWithin(SingleInstanceServer &server, int ms)
{
    QList<Message> opens;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        opens += server.takeOpens();
        QThread::msleep(10);
    }
    return opens;
}

#ifdef Q_OS_UNIX
// Sets an environment variable for the rest of the scope.
class EnvOverride
{
public:
    EnvOverride(const char *name, const QByteArray &value)
        : name_(name)
        , previous_(qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt)
    {
        if (value.isNull())
            qunsetenv(name);
        else
            qputenv(name, value);
    }
    ~EnvOverride()
    {
        if (previous_)
            qputenv(name_, *previous_);
        else
            qunsetenv(name_);
    }

private:
    const char *name_;
    std::optional<QByteArray> previous_;
};
#endif
} // namespace

// Substitute process identity so the real lock and IPC implementation can be
// exercised for both installation types on each test platform.
namespace mervin::PlatformIntegration {
bool hasPackageIdentity() { return packaged; }
}

class TstSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void secondListenFails();
    void relistenAfterClose();
    void twoClientsDeliverMessages();
    void storeAndMsiHostsStaySeparate();
    void acksWhileMainThreadBlocked_data();
    void acksWhileMainThreadBlocked();
    void stoppedServerKeepsLockWithoutAck();
    void closedWithoutAckIsNoPrimary();
    void launchDeliversToRunningPrimary();
    void launchWaitsForQuittingPrimary();
    void busyPrimaryGetsOpenOnce();
    void launchThatGaveUpIsNotOpened();
    void peerCheck_data();
    void peerCheck();
    void lockLeftWithoutContent_data();
    void lockLeftWithoutContent();
#ifdef Q_OS_UNIX
    void filesLiveInRuntimeDir();
    void launchesWithAndWithoutXdgRuntimeDirShareOnePrimary();
    void primaryInAnotherFolderIsFound();
    void launchWaitsForQuittingPrimaryInAnotherFolder_data();
    void launchWaitsForQuittingPrimaryInAnotherFolder();
    void lockCheckOnlyReadsTheLockFile();
    void lockHolder_data();
    void lockHolder();
    void noRuntimeDirUsesProfileInstanceFolder();
    void launchesWithDifferentCacheAndTempShareOnePrimary();
    void instanceFolderMustBePrivate_data();
    void instanceFolderMustBePrivate();
    void tooLongPathFallsBack_data();
    void tooLongPathFallsBack();
    void socketFolderMustBePrivate_data();
    void socketFolderMustBePrivate();
    void launchFindsPrimaryThroughAnotherPath();
    void profileFolderWithoutSockets_data();
    void profileFolderWithoutSockets();
    void deletedLockKeepsLivePrimary();
    void deadSocketIsReplaced();
#endif
#ifdef Q_OS_LINUX
    void legacyNameMatchesOlderCopies();
    void legacyCopyGetsTheOpen();
    void legacyCopyOfAnotherUserGetsNothing();
    void quittingLegacyCopyIsSkipped();
    void silentLegacyCopyMeansStandalone();
    void newerPrimaryGoesBeforeLegacyCopy();
    void deadLegacySocketIsIgnored();
#endif

private:
#ifdef Q_OS_UNIX
    // Keeps the tests out of the user's real runtime folder, profile and cache folder, out
    // of /tmp, and out of the temp folder, where builds from before the move listen under
    // the same group names. Short paths of their own keep every socket path within Qt's
    // limit, whatever the temp folder the suite runs with.
    QTemporaryDir runtimeRoot_{kShortTemp};
    QTemporaryDir tmp_{kShortTemp};
    QTemporaryDir cache_{kShortTemp};
    QTemporaryDir profile_{kShortTemp};
    QTemporaryDir socketRoot_{kShortTemp};
    QString previousProfile_;
    std::optional<EnvOverride> runtimeEnv_;
    std::optional<EnvOverride> tmpEnv_;
    std::optional<EnvOverride> cacheEnv_;
#endif
};

#ifdef Q_OS_UNIX
namespace {
// The users' runtime root the suite uses, in place of /run/user.
QString g_runtimeRoot;

// Makes `root`/<uid> this user's 0700 folder, as systemd-logind does, and returns it.
QString makeUserRuntimeDir(const QString &root)
{
    const QString dir = QDir(root).filePath(QString::number(uint(::geteuid())));
    if (!QDir().mkpath(dir)
        || !QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return {};
    return dir;
}

// Points the users' runtime root at `root` for the rest of the scope.
class RuntimeRootOverride
{
public:
    explicit RuntimeRootOverride(const QString &root)
    {
        mervin::ipc::setUserRuntimeRootForTests(root);
    }
    ~RuntimeRootOverride() { mervin::ipc::setUserRuntimeRootForTests(g_runtimeRoot); }
};

// Leaves no usable /run/user/<uid> for the rest of the scope.
class NoRuntimeDir : public RuntimeRootOverride
{
public:
    NoRuntimeDir()
        : RuntimeRootOverride(g_runtimeRoot + QStringLiteral("/none"))
    {
    }
};

// Uses `dir` as the profile folder for the rest of the scope.
class ProfileOverride
{
public:
    explicit ProfileOverride(const QString &dir)
        : previous_(mervin::ConfigPaths::overrideDir())
    {
        mervin::ConfigPaths::setOverrideDir(dir);
    }
    ~ProfileOverride() { mervin::ConfigPaths::setOverrideDir(previous_); }

private:
    QString previous_;
};

// The instance folders in the profile folder `profile`.
QStringList instanceFolders(const QString &profile)
{
    return QDir(profile).entryList({QStringLiteral("instance-*")},
                                   QDir::Dirs | QDir::Hidden | QDir::System);
}
} // namespace
#endif

void TstSingleInstance::initTestCase()
{
#ifdef Q_OS_UNIX
    QVERIFY(runtimeRoot_.isValid() && tmp_.isValid() && cache_.isValid() && profile_.isValid()
            && socketRoot_.isValid());
    g_runtimeRoot = runtimeRoot_.path();
    const QString runtimeDir = makeUserRuntimeDir(g_runtimeRoot);
    QVERIFY(!runtimeDir.isEmpty());
    mervin::ipc::setUserRuntimeRootForTests(g_runtimeRoot);
    mervin::ipc::setSocketRootForTests(socketRoot_.path());
    previousProfile_ = mervin::ConfigPaths::overrideDir();
    mervin::ConfigPaths::setOverrideDir(profile_.path());
    // Nothing reads these for the lock any more, which the tests that set them show.
    runtimeEnv_.emplace("XDG_RUNTIME_DIR", QByteArray());
    tmpEnv_.emplace("TMPDIR", QFile::encodeName(tmp_.path()));
    cacheEnv_.emplace("XDG_CACHE_HOME", QFile::encodeName(cache_.path()));
    // A fallback to another folder fails here, not later in a test that cannot tell why.
    const QString lockFile = mervin::ipc::instancePaths(kTestPipe).lockFile;
    QVERIFY2(lockFile.startsWith(runtimeDir + QLatin1Char('/')),
             qPrintable(QStringLiteral("the lock is at %1, outside the suite's folder")
                            .arg(lockFile)));
#endif
}

void TstSingleInstance::cleanupTestCase()
{
#ifdef Q_OS_UNIX
    cacheEnv_.reset();
    tmpEnv_.reset();
    runtimeEnv_.reset();
    mervin::ConfigPaths::setOverrideDir(previousProfile_);
    mervin::ipc::setSocketRootForTests({});
    mervin::ipc::setUserRuntimeRootForTests({});
#endif
}

void TstSingleInstance::storeAndMsiHostsStaySeparate()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString previousProfile = mervin::ConfigPaths::overrideDir();
    const auto reset = qScopeGuard([&] {
        packaged = false;
        mervin::ConfigPaths::setOverrideDir(previousProfile);
    });
    mervin::ConfigPaths::setOverrideDir(profile.path());

    const QString msiName = mervin::ipc::hostPipeName();
    SingleInstanceServer msi;
    QCOMPARE(msi.start(), Start::Started);
    packaged = true;
    const QString storeName = mervin::ipc::hostPipeName();
    QVERIFY(storeName != msiName);
    SingleInstanceServer store;
    QCOMPARE(store.start(), Start::Started);
#ifdef Q_OS_UNIX
    QVERIFY2(mervin::ipc::instancePaths().lockFile.startsWith(g_runtimeRoot + QLatin1Char('/')),
             qPrintable(mervin::ipc::instancePaths().lockFile));
#endif
    SingleInstanceServer duplicateStore;
    QCOMPARE(duplicateStore.start(), Start::Running);

    QCOMPARE(mervin::ipc::handOff(mervin::ipc::instancePaths().server,
                                  Message::open({QStringLiteral("store.pdf")},
                                                QStringLiteral("new-tab")),
                                  QDeadlineTimer(1000)),
             Handoff::Delivered);
    QCOMPARE(store.takeOpens().size(), 1);
    QCOMPARE(msi.takeOpens().size(), 0);
}

void TstSingleInstance::secondListenFails()
{
    SingleInstanceServer s1;
    QCOMPARE(s1.start(kTestPipe), Start::Started);
    QVERIFY(s1.isListening());

    // A second server on the same name must be rejected - the single-instance
    // guarantee. It reports the live holder, so the launch hands off.
    SingleInstanceServer s2;
    QCOMPARE(s2.start(kTestPipe), Start::Running);
    QVERIFY(!s2.isListening());
}

void TstSingleInstance::relistenAfterClose()
{
    {
        SingleInstanceServer s1;
        QCOMPARE(s1.start(kTestPipe), Start::Started);
    } // s1 destroyed -> pipe released

    // No stale-pipe block: a fresh server can immediately take the name.
    SingleInstanceServer s2;
    QCOMPARE(s2.start(kTestPipe), Start::Started);
    QVERIFY(s2.isListening());
}

void TstSingleInstance::twoClientsDeliverMessages()
{
    SingleInstanceServer server;
    QCOMPARE(server.start(kTestPipe), Start::Started);

    QLocalSocket a;
    a.connectToServer(testEndpoint());
    QVERIFY(a.waitForConnected(1000));
    a.write(Message::open({QStringLiteral("y.pdf")}, QStringLiteral("new-window")).encode());
    a.flush();

    QLocalSocket b;
    b.connectToServer(testEndpoint());
    QVERIFY(b.waitForConnected(1000));
    b.write(Message::open({QStringLiteral("x.pdf")}, QStringLiteral("new-tab")).encode());
    b.flush();

    // Each client gets its acknowledgement from the server thread.
    for (QLocalSocket *client : {&a, &b}) {
        mervin::ipc::MessageDecoder decoder;
        bool acked = false;
        while (!acked && client->waitForReadyRead(1000))
            for (const Message &m : decoder.feed(client->readAll()))
                acked = acked || m.cmd == Message::Cmd::Ack;
        QVERIFY(acked);
    }

    // Order across two sockets isn't guaranteed; assert by content.
    const QList<Message> received = server.takeOpens();
    QCOMPARE(received.size(), 2);
    bool sawY = false, sawX = false;
    for (const Message &m : received) {
        if (m.paths == QStringList{QStringLiteral("y.pdf")})
            sawY = m.behavior == QLatin1String("new-window");
        if (m.paths == QStringList{QStringLiteral("x.pdf")})
            sawX = m.behavior == QLatin1String("new-tab");
    }
    QVERIFY(sawY);
    QVERIFY(sawX);
}

void TstSingleInstance::acksWhileMainThreadBlocked_data()
{
    QTest::addColumn<bool>("lateReceiver");
    QTest::newRow("receiver connected first") << false;
    QTest::newRow("receiver connects after the frame") << true;
}

// The GUI thread can be busy for seconds, as in a settings save on a profile folder that
// stopped answering. The launch must still get its acknowledgement at once, and the open
// must reach the GUI thread exactly once when it resumes.
void TstSingleInstance::acksWhileMainThreadBlocked()
{
    QFETCH(bool, lateReceiver);
    SingleInstanceServer server;
    QCOMPARE(server.start(kTestPipe), Start::Started);

    QList<Message> received;
    const auto take = [&] { received += server.takeOpens(); };
    QMetaObject::Connection receiver;
    if (!lateReceiver)
        receiver = connect(&server, &SingleInstanceServer::opensReceived, this, take);

    std::optional<Handoff> result;
    qint64 elapsed = -1;
    const std::unique_ptr<QThread> launch(QThread::create([&] {
        QElapsedTimer timer;
        timer.start();
        result = mervin::ipc::handOff(testEndpoint(),
                                      Message::open({QStringLiteral("busy.pdf")}),
                                      QDeadlineTimer(1000));
        elapsed = timer.elapsed();
    }));
    launch->start();
    // Joining blocks this thread without dispatching GUI events. The handoff
    // must finish on the server thread within its own deadline.
    QVERIFY(launch->wait(5000));
    QCOMPARE(result, std::optional(Handoff::Delivered));
    QVERIFY2(elapsed < 1000, qPrintable(QString::number(elapsed)));

    if (lateReceiver) {
        // The notification has gone out with nobody listening; the open waits.
        QCoreApplication::processEvents();
        receiver = connect(&server, &SingleInstanceServer::opensReceived, this, take);
        take();
    }
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().paths, QStringList{QStringLiteral("busy.pdf")});
    QTest::qWait(200);
    QCOMPARE(received.size(), 1);
    disconnect(receiver);
}

// A quitting primary closes its endpoint and keeps the lock through its exit wait.
void TstSingleInstance::stoppedServerKeepsLockWithoutAck()
{
    auto s1 = std::make_unique<SingleInstanceServer>();
    QCOMPARE(s1->start(kTestPipe), Start::Started);

    // A connection made before the stop closes without an acknowledgement.
    QLocalSocket early;
    QByteArray answer;
    connect(&early, &QLocalSocket::readyRead, this, [&] { answer += early.readAll(); });
    early.connectToServer(testEndpoint());
    QVERIFY(early.waitForConnected(1000));
    s1->stopListening();
    QVERIFY(!s1->isListening());
    early.write(Message::open({QStringLiteral("early.pdf")}).encode());
    early.flush();
    QTRY_VERIFY(early.state() != QLocalSocket::ConnectedState);
    QVERIFY(answer.isEmpty());

    QCOMPARE(mervin::ipc::handOff(testEndpoint(), Message::open({QStringLiteral("late.pdf")}),
                                  QDeadlineTimer(500)),
             Handoff::NoPrimary);
    QCOMPARE(opensWithin(*s1, 100).size(), 0);

    SingleInstanceServer s2;
    QCOMPARE(s2.start(kTestPipe), Start::Running);
    s1.reset();
    QCOMPARE(s2.start(kTestPipe), Start::Started);
}

// A server that reads the open and closes the connection without an acknowledgement did
// not take it, so the launch may offer it again rather than wait for the deadline.
void TstSingleInstance::closedWithoutAckIsNoPrimary()
{
    const QString endpoint = testEndpoint();
    QLocalServer::removeServer(endpoint);
    std::atomic<bool> listening = false;
    std::atomic<int> received = 0;
    const std::unique_ptr<QThread> server(QThread::create([&] {
        QLocalServer refusing;
        connect(&refusing, &QLocalServer::newConnection, &refusing, [&] {
            while (QLocalSocket *socket = refusing.nextPendingConnection()) {
                if (socket->waitForReadyRead(1000))
                    received += int(socket->readAll().count('\n'));
                socket->abort();
                socket->deleteLater();
            }
        });
        listening = refusing.listen(endpoint);
        QEventLoop().exec();
    }));
    server->start();
    QTRY_VERIFY(listening);

    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::handOff(endpoint, Message::open({QStringLiteral("x.pdf")}),
                                  QDeadlineTimer(3000)),
             Handoff::NoPrimary);
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(received.load(), 1);
    server->quit();
    QVERIFY(server->wait(5000));
}

void TstSingleInstance::launchDeliversToRunningPrimary()
{
    SingleInstanceServer primary;
    QCOMPARE(primary.start(kTestPipe), Start::Started);

    // This thread is blocked in the launch, as the primary's GUI thread can be.
    SingleInstanceServer launch;
    const Message open = Message::open({QStringLiteral("a.pdf")}, QStringLiteral("new-tab"));
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 1000, kTestPipe), Launch::Delivered);
    QVERIFY(!launch.isListening());
    const QList<Message> opens = primary.takeOpens();
    QCOMPARE(opens.size(), 1);
    QCOMPARE(opens.first().paths, open.paths);
    QCOMPARE(opens.first().behavior, open.behavior);
}

// A launch during a primary's exit wait waits for the lock and becomes the next
// primary, instead of opening a second window beside it.
void TstSingleInstance::launchWaitsForQuittingPrimary()
{
    auto quitting = std::make_unique<SingleInstanceServer>();
    QCOMPARE(quitting->start(kTestPipe), Start::Started);
    quitting->stopListening();

    std::atomic<bool> released = false;
    std::optional<Launch> result;
    bool afterRelease = false;
    qint64 elapsed = -1;
    const std::unique_ptr<QThread> launch(QThread::create([&] {
        SingleInstanceServer next;
        QElapsedTimer timer;
        timer.start();
        result = mervin::ipc::claimOrHandOff(next, Message::open({QStringLiteral("b.pdf")}),
                                             5000, kTestPipe);
        afterRelease = released;
        elapsed = timer.elapsed();
    }));
    launch->start();
    QThread::msleep(1000);
    released = true;
    quitting.reset(); // the exit wait is over
    QVERIFY(launch->wait(10000));
    QCOMPARE(result, std::optional(Launch::Primary));
    QVERIFY2(afterRelease, "became the primary while the old one still held the lock");
    QVERIFY2(elapsed < 5000, qPrintable(QString::number(elapsed)));
}

namespace {
// A server on its own thread that accepts connections and counts the frames they bring,
// but never acknowledges one, as a primary does whose GUI and server threads both hang.
class SilentPeer
{
public:
    explicit SilentPeer(const QString &endpoint)
        : thread_(QThread::create([this, endpoint] {
            QLocalServer server;
            QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
                while (QLocalSocket *socket = server.nextPendingConnection()) {
                    ++connections;
                    QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                        frames += int(socket->readAll().count('\n'));
                    });
                }
            });
            listening = server.listen(endpoint);
            QEventLoop().exec();
        }))
    {
        thread_->start();
    }
    ~SilentPeer()
    {
        thread_->quit();
        thread_->wait();
    }

    std::atomic<bool> listening = false;
    std::atomic<int> connections = 0;
    std::atomic<int> frames = 0;

private:
    std::unique_ptr<QThread> thread_;
};
} // namespace

// A primary that took the connection but never answers gets the open once, however
// long the launch waits, and the launch then opens standalone.
void TstSingleInstance::busyPrimaryGetsOpenOnce()
{
    const mervin::ipc::InstancePaths paths = mervin::ipc::instancePaths(kTestPipe);
    QLockFile lock(paths.lockFile);
    QVERIFY(lock.tryLock(0));
    QLocalServer::removeServer(paths.server);
    SilentPeer silent(paths.server);
    QTRY_VERIFY(silent.listening);

    SingleInstanceServer launch;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("c.pdf")}), 1500,
                                         kTestPipe),
             Launch::Standalone);
    QVERIFY(timer.elapsed() >= 1400);

    // Every connection the launch made counts, not only the first.
    QTRY_COMPARE(silent.frames.load(), 1);
    QTest::qWait(200);
    QCOMPARE(silent.connections.load(), 1);
    QCOMPARE(silent.frames.load(), 1);
}

// A launch that gave up waiting opened its files in a copy of its own, so a primary that
// reads its open only afterwards must not open the files as well.
void TstSingleInstance::launchThatGaveUpIsNotOpened()
{
    SingleInstanceServer server;
    QCOMPARE(server.start(kTestPipe), Start::Started);

    // The server thread hangs, as in a stopped process.
    QSemaphore held;
    QSemaphore release;
    server.runOnServerThreadForTests([&] {
        held.release();
        release.acquire();
    });
    QVERIFY(held.tryAcquire(1, 5000));
    const Handoff gaveUp = mervin::ipc::handOff(
        testEndpoint(), Message::open({QStringLiteral("gave-up.pdf")}), QDeadlineTimer(500));
    release.release();
    QCOMPARE(gaveUp, Handoff::Busy);
    QCOMPARE(opensWithin(server, 500).size(), 0);

    // A launch that still waits is served as before.
    QCOMPARE(mervin::ipc::handOff(testEndpoint(), Message::open({QStringLiteral("waits.pdf")}),
                                  QDeadlineTimer(2000)),
             Handoff::Delivered);
    const QList<Message> opens = opensWithin(server, 100);
    QCOMPARE(opens.size(), 1);
    QCOMPARE(opens.first().paths, QStringList{QStringLiteral("waits.pdf")});
}

void TstSingleInstance::peerCheck_data()
{
    QTest::addColumn<QString>("peer");
    QTest::newRow("same user") << QStringLiteral("same");
    QTest::newRow("another user") << QStringLiteral("other");
    // As on Windows, where the token of another account's process cannot be read.
    QTest::newRow("user unknown") << QStringLiteral("unknown");
    QTest::newRow("empty identity") << QStringLiteral("empty");
}

// A launch checks who runs the listening process, through the same lookup a real launch
// uses, and sends no path to a process that is not known to be this user's.
void TstSingleInstance::peerCheck()
{
    QFETCH(QString, peer);
    SingleInstanceServer server;
    QCOMPARE(server.start(kTestPipe), Start::Started);

    QLocalSocket probe;
    probe.connectToServer(testEndpoint());
    QVERIFY(probe.waitForConnected(1000));
    QCOMPARE(mervin::ipc::peerUser(probe), std::optional(mervin::ipc::currentUser()));
#ifdef Q_OS_UNIX
    QCOMPARE(mervin::ipc::currentUser(), QByteArray::number(uint(::getuid())));
#endif
    probe.abort();

    // What the system would report for the process at the endpoint.
    mervin::ipc::UserLookup lookup = mervin::ipc::peerUser;
    if (peer == QLatin1String("other")) {
        lookup = [](const QLocalSocket &s) {
            const std::optional<QByteArray> user = mervin::ipc::peerUser(s);
            return user ? std::optional(*user + QByteArray("-other")) : user;
        };
    } else if (peer == QLatin1String("unknown")) {
        lookup = [](const QLocalSocket &) { return std::optional<QByteArray>(); };
    } else if (peer == QLatin1String("empty")) {
        lookup = [](const QLocalSocket &) { return std::optional(QByteArray()); };
    }
    const bool accepted = peer == QLatin1String("same");
    const Message open = Message::open({QStringLiteral("/home/a/private.pdf")});
    const Handoff result = accepted
        ? mervin::ipc::handOff(testEndpoint(), open, QDeadlineTimer(500))
        : mervin::ipc::handOff(testEndpoint(), open, QDeadlineTimer(500), lookup);
    QCOMPARE(result, accepted ? Handoff::Delivered : Handoff::NoPrimary);
    QCOMPARE(opensWithin(server, 100).size(), accepted ? 1 : 0);
    if (accepted)
        return;

    // A whole launch never hands its files to that process. The lock is held, so it
    // keeps trying, then opens standalone.
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 600, kTestPipe, lookup), Launch::Standalone);
    QCOMPARE(opensWithin(server, 200).size(), 0);
}

void TstSingleInstance::lockLeftWithoutContent_data()
{
    QTest::addColumn<bool>("old");
    QTest::newRow("left by a process that ended") << true;
    QTest::newRow("being written") << false;
}

// A process that ended between making its lock file and writing it leaves the file empty.
// QLockFile never counts that as stale, so start() replaces it itself once it is older than
// QLockFile needs to write it. One that is being written still holds the lock.
void TstSingleInstance::lockLeftWithoutContent()
{
    QFETCH(bool, old);
    const QString lockFile = mervin::ipc::instancePaths(kTestPipe).lockFile;
    {
        QFile file(lockFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        if (old)
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                                     QFileDevice::FileModificationTime));
    }
    const auto removeLock = qScopeGuard([&] { QFile::remove(lockFile); });
    QCOMPARE(mervin::ipc::lockLeftUnwritten(lockFile), old);
    SingleInstanceServer server;
    QCOMPARE(server.start(kTestPipe), old ? Start::Started : Start::Running);
    if (old)
        QVERIFY(QFileInfo(lockFile).size() > 0); // this process's lock now
}

#ifdef Q_OS_UNIX
namespace {
// Whether `dir` is this user's folder with mode 0700.
bool ownPrivateDir(const QString &dir)
{
    return mervin::ipc::isPrivateDir(dir, uint(::geteuid()));
}

// The longest socket path Qt takes. It keeps a byte of sockaddr_un for the terminating
// zero, and on Linux one more for abstract names.
#  ifdef Q_OS_LINUX
constexpr int kSocketPathLimit = int(sizeof(sockaddr_un::sun_path)) - 2;
#  else
constexpr int kSocketPathLimit = int(sizeof(sockaddr_un::sun_path)) - 1;
#  endif

// This user's socket folder in the socket root `root`.
QString socketFolder(const QString &root)
{
    return QDir(root).filePath(QStringLiteral("mervin-pdf-%1").arg(uint(::geteuid())));
}

// The mode bits of `path`, without following a link.
uint modeOf(const QString &path)
{
    struct stat st{};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? uint(st.st_mode & 07777) : 0;
}
} // namespace

void TstSingleInstance::filesLiveInRuntimeDir()
{
    QTemporaryDir root(kShortTemp);
    QTemporaryDir tmp(kShortTemp);
    QVERIFY(root.isValid() && tmp.isValid());
    const QString runtimeDir = makeUserRuntimeDir(root.path());
    QVERIFY(!runtimeDir.isEmpty());
    const RuntimeRootOverride rootOverride(root.path());
    const EnvOverride tmpEnv("TMPDIR", QFile::encodeName(tmp.path()));
    // The group name comes from the uid, not from $USER.
    const QString name = mervin::ipc::hostPipeName();
    {
        const EnvOverride user("USER", "someone-else");
        const EnvOverride userName("USERNAME", "someone-else");
        QCOMPARE(mervin::ipc::hostPipeName(), name);
    }

    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Started);
    const QString lock = QDir(runtimeDir).filePath(kTestPipe + QStringLiteral(".lock"));
    const QString socket = QDir(runtimeDir).filePath(kTestPipe);
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile, lock);
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).server, socket);
    // $XDG_RUNTIME_DIR, which a launch from ssh, cron or `sudo -u` may lack or have
    // elsewhere, does not move them.
    {
        QTemporaryDir elsewhere(kShortTemp);
        QVERIFY(QFile::setPermissions(elsewhere.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const EnvOverride xdg("XDG_RUNTIME_DIR", QFile::encodeName(elsewhere.path()));
        QCOMPARE(mervin::ipc::instancePaths(kTestPipe).server, socket);
    }
    QVERIFY(QFile::exists(lock));
    QVERIFY(QFile::exists(socket));
    // Nothing goes to the temp folder.
    QVERIFY(QDir(tmp.path()).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System |
                                     QDir::NoDotAndDotDot));

    // Sticky, so a cleaner that follows the XDG spec leaves them alone.
    for (const QString &path : {lock, socket}) {
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
        QVERIFY2(st.st_mode & S_ISVTX, qPrintable(path));
    }
}

// A launch from the desktop session has $XDG_RUNTIME_DIR, one from ssh, cron or `sudo -u`
// may not, or may have it elsewhere. Both take the same lock, so the second hands off.
void TstSingleInstance::launchesWithAndWithoutXdgRuntimeDirShareOnePrimary()
{
    SingleInstanceServer primary;
    QCOMPARE(primary.start(kTestPipe), Start::Started); // without the variable

    QTemporaryDir elsewhere(kShortTemp);
    QVERIFY(QFile::setPermissions(elsewhere.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const EnvOverride xdg("XDG_RUNTIME_DIR", QFile::encodeName(elsewhere.path()));
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("x.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QVERIFY(!launch.isListening());
    QCOMPARE(primary.takeOpens().size(), 1);

    // A launch makes no folder the variable names.
    const QString missing = QDir(elsewhere.path()).filePath(QStringLiteral("missing"));
    const EnvOverride missingXdg("XDG_RUNTIME_DIR", QFile::encodeName(missing));
    SingleInstanceServer another;
    QCOMPARE(mervin::ipc::claimOrHandOff(another, Message::open({QStringLiteral("y.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(primary.takeOpens().size(), 1);
    QVERIFY(!QFileInfo::exists(missing));
}

// The folders available can change while a primary runs, as when /run/user/<uid> appears
// with a desktop login after the primary started from ssh, in the profile's instance
// folder. A launch that would choose /run/user/<uid> still finds that primary and hands
// off to it.
void TstSingleInstance::primaryInAnotherFolderIsFound()
{
    SingleInstanceServer primary;
    {
        const NoRuntimeDir noRuntimeDir;
        QCOMPARE(primary.start(kTestPipe), Start::Started);
        QVERIFY(mervin::ipc::instancePaths(kTestPipe).lockFile.startsWith(
            profile_.path() + QStringLiteral("/instance-")));
    }
    QVERIFY(mervin::ipc::instancePaths(kTestPipe).server.startsWith(g_runtimeRoot));

    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("y.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QVERIFY(!launch.isListening());
    QCOMPARE(primary.takeOpens().size(), 1);
}

void TstSingleInstance::launchWaitsForQuittingPrimaryInAnotherFolder_data()
{
    QTest::addColumn<QString>("other");
    QTest::newRow("the other copy exits") << QStringLiteral("exits");
    QTest::newRow("the other copy does not exit") << QStringLiteral("stays");
    QTest::newRow("a lock left with an ID another user has now") << QStringLiteral("leftover");
}

// A copy that holds the lock in another folder and has closed its endpoint is in its exit
// wait, or starting. A launch whose own folder's lock is free waits for it as for a holder
// of that lock, and becomes the primary only once it has let go. A lock left there by a
// process that ended, whose ID another user's process has now, does not hold it back.
void TstSingleInstance::launchWaitsForQuittingPrimaryInAnotherFolder()
{
    QFETCH(QString, other);
    const bool released = other == QLatin1String("exits");
    QString otherLock;
    {
        const NoRuntimeDir noRuntimeDir;
        otherLock = mervin::ipc::instancePaths(kTestPipe).lockFile;
    }
    QVERIFY(otherLock.startsWith(profile_.path() + QStringLiteral("/instance-")));
    QLockFile quitting(otherLock);
    if (other == QLatin1String("leftover")) {
        if (QCoreApplication::applicationPid() == 1)
            QSKIP("this test runs as process 1");
        QFile file(otherLock);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("1\nMervinPDF\n" + QSysInfo::machineHostName().toUtf8() + '\n'
                           + QSysInfo::machineUniqueId() + '\n' + QSysInfo::bootUniqueId()
                           + '\n')
                > 0);
    } else {
        QVERIFY(quitting.tryLock(0));
    }
    const auto removeLeftover = qScopeGuard([&] {
        if (other == QLatin1String("leftover"))
            QFile::remove(otherLock);
    });

    std::atomic<bool> unlocked = false;
    std::optional<Launch> result;
    bool afterRelease = false;
    bool listening = false;
    qint64 elapsed = -1;
    const int waitMs = other == QLatin1String("stays") ? 600 : 5000;
    const std::unique_ptr<QThread> launch(QThread::create([&] {
        SingleInstanceServer next;
        QElapsedTimer timer;
        timer.start();
        result = mervin::ipc::claimOrHandOff(next, Message::open({QStringLiteral("c.pdf")}),
                                             waitMs, kTestPipe);
        elapsed = timer.elapsed();
        afterRelease = unlocked;
        listening = next.isListening();
    }));
    launch->start();
    if (released) {
        QThread::msleep(500);
        unlocked = true;
        quitting.unlock(); // the exit wait is over
    }
    QVERIFY(launch->wait(10000));
    if (other == QLatin1String("leftover")) {
        QCOMPARE(result, std::optional(Launch::Primary));
        QVERIFY2(elapsed < 2000, qPrintable(QString::number(elapsed)));
        QVERIFY(listening);
    } else if (released) {
        QCOMPARE(result, std::optional(Launch::Primary));
        QVERIFY2(afterRelease, "became the primary while the other copy still held its lock");
        QVERIFY(listening);
    } else {
        QCOMPARE(result, std::optional(Launch::Standalone));
        QVERIFY(!listening);
    }
}

// Before it probes other folders, a launch checks whether a live process holds this
// group's lock. It only reads the lock file, so another launch never sees a lock that this
// check made. A lock file left by a process that has exited stays as it is, for start() to
// replace.
void TstSingleInstance::lockCheckOnlyReadsTheLockFile()
{
    SingleInstanceServer primary;
    {
        const NoRuntimeDir noRuntimeDir;
        QCOMPARE(primary.start(kTestPipe), Start::Started);
    }

    QProcess exited;
    exited.start(QStringLiteral("true"), {});
    QVERIFY(exited.waitForStarted());
    const qint64 deadPid = exited.processId();
    QVERIFY(exited.waitForFinished());
    const QString lockFile = mervin::ipc::instancePaths(kTestPipe).lockFile;
    QVERIFY(lockFile.startsWith(g_runtimeRoot));
    const QByteArray stale = QByteArray::number(deadPid) + "\nMervinPDF\n"
        + QSysInfo::machineHostName().toUtf8() + '\n';
    {
        QFile file(lockFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(stale), stale.size());
    }
    const auto removeLock = qScopeGuard([&] { QFile::remove(lockFile); });

    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("z.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(primary.takeOpens().size(), 1);
    QFile file(lockFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), stale);
}

void TstSingleInstance::lockHolder_data()
{
    QTest::addColumn<QString>("holder");
    QTest::addColumn<bool>("held");
    QTest::newRow("no lock file") << QStringLiteral("none") << false;
    QTest::newRow("this process") << QStringLiteral("self") << true;
    QTest::newRow("a process that ended") << QStringLiteral("ended") << false;
    QTest::newRow("a process of another user") << QStringLiteral("process 1") << false;
#  ifdef Q_OS_LINUX
    QTest::newRow("another program under that ID") << QStringLiteral("other program") << false;
#  endif
    QTest::newRow("an earlier boot") << QStringLiteral("earlier boot") << false;
    QTest::newRow("another machine") << QStringLiteral("other machine") << false;
    QTest::newRow("being written") << QStringLiteral("empty now") << true;
    QTest::newRow("left without content") << QStringLiteral("empty old") << false;
}

// When a lock file in one of this user's folders counts as held by a live process, read
// without taking the lock. The files are written as QLockFile writes them.
void TstSingleInstance::lockHolder()
{
    QFETCH(QString, holder);
    QFETCH(bool, held);
    QTemporaryDir dir(kShortTemp);
    QVERIFY(dir.isValid());
    const QString lockFile = QDir(dir.path()).filePath(QStringLiteral("group.lock"));
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const auto writeLock = [&](qint64 pid, const QString &app,
                               const QByteArray &machine = QSysInfo::machineUniqueId(),
                               const QByteArray &boot = QSysInfo::bootUniqueId()) {
        QFile file(lockFile);
        return file.open(QIODevice::WriteOnly)
            && file.write(QByteArray::number(pid) + '\n' + app.toUtf8() + '\n'
                          + QSysInfo::machineHostName().toUtf8() + '\n' + machine + '\n'
                          + boot + '\n')
            > 0;
    };
    QProcess other;
    const auto stopOther = qScopeGuard([&] {
        other.kill();
        other.waitForFinished();
    });
    if (holder == QLatin1String("self")) {
        QVERIFY(writeLock(QCoreApplication::applicationPid(), self));
    } else if (holder == QLatin1String("ended")) {
        other.start(QStringLiteral("true"), {});
        QVERIFY(other.waitForStarted());
        const qint64 pid = other.processId();
        QVERIFY(other.waitForFinished());
        QVERIFY(writeLock(pid, self));
    } else if (holder == QLatin1String("process 1")) {
        // The init process, root's, or this test's own in a container where it is process 1.
        if (QCoreApplication::applicationPid() == 1)
            QSKIP("this test runs as process 1");
        QVERIFY(writeLock(1, self));
    } else if (holder == QLatin1String("earlier boot")) {
        if (QSysInfo::bootUniqueId().isEmpty())
            QSKIP("this system has no boot ID");
        QVERIFY(writeLock(QCoreApplication::applicationPid(), self, QSysInfo::machineUniqueId(),
                          "00000000-0000-0000-0000-000000000000"));
    } else if (holder == QLatin1String("other machine")) {
        if (QSysInfo::machineUniqueId().isEmpty())
            QSKIP("this system has no machine ID");
        QVERIFY(writeLock(QCoreApplication::applicationPid(), self, "another-machine"));
    } else if (holder == QLatin1String("other program")) {
        other.start(QStringLiteral("sleep"), {QStringLiteral("30")});
        QVERIFY(other.waitForStarted());
        QVERIFY(writeLock(other.processId(), self));
    } else if (holder.startsWith(QLatin1String("empty"))) {
        QFile file(lockFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        if (holder == QLatin1String("empty old"))
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                                     QFileDevice::FileModificationTime));
    }
    QCOMPARE(mervin::ipc::lockHeldByLiveProcess(lockFile), held);
    QCOMPARE(QFile::exists(lockFile), holder != QLatin1String("none")); // made or removed none
}

// Without /run/user/<uid> the lock, the socket and the carry file are in a folder of
// this machine's own in the profile folder, made with mode 0700. The profile folder's own
// mode stays as it is, and nothing goes to the cache or temp folder.
void TstSingleInstance::noRuntimeDirUsesProfileInstanceFolder()
{
    QTemporaryDir profile(kShortTemp);
    QTemporaryDir cache(kShortTemp);
    QTemporaryDir tmp(kShortTemp);
    QVERIFY(profile.isValid() && cache.isValid() && tmp.isValid());
    QVERIFY(QFile::setPermissions(profile.path(), QFile::ReadOwner | QFile::WriteOwner
                                                      | QFile::ExeOwner | QFile::ReadOther
                                                      | QFile::ExeOther));
    const uint profileMode = modeOf(profile.path());
    const ProfileOverride profileOverride(profile.path());
    const NoRuntimeDir noRuntimeDir;
    const EnvOverride cacheHome("XDG_CACHE_HOME", QFile::encodeName(cache.path()));
    const EnvOverride tmpEnv("TMPDIR", QFile::encodeName(tmp.path()));

    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Started);
    // One folder per machine, as machines can share a profile folder.
    const QStringList folders = instanceFolders(profile.path());
    QCOMPARE(folders.size(), 1);
    QVERIFY2(folders.first().size() == int(QStringLiteral("instance-").size()) + 8,
             qPrintable(folders.first()));
    const QDir dir(QDir(profile.path()).filePath(folders.first()));
    const mervin::ipc::InstancePaths paths = mervin::ipc::instancePaths(kTestPipe);
    QCOMPARE(paths.lockFile, dir.filePath(kTestPipe + QStringLiteral(".lock")));
    QCOMPARE(paths.server, dir.filePath(kTestPipe));
    QCOMPARE(paths.carriedFile, dir.filePath(kTestPipe + QStringLiteral(".carried")));
    QVERIFY(QFile::exists(paths.lockFile));
    QVERIFY(QFile::exists(paths.server));
    QCOMPARE(mervin::ipc::endpointOf(paths), paths.server);
    QVERIFY(ownPrivateDir(dir.path()));
    QCOMPARE(modeOf(profile.path()), profileMode);
    for (const QString &other : {cache.path(), tmp.path(), socketRoot_.path()})
        QVERIFY2(QDir(other).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System
                                     | QDir::NoDotAndDotDot),
                 qPrintable(other));

    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("p.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(s.takeOpens().size(), 1);
}

// Without /run/user/<uid>, launches of one profile take the same lock whatever their
// $XDG_CACHE_HOME and $TMPDIR, as the lock lies with the profile's files, so one of them
// ends up the primary.
void TstSingleInstance::launchesWithDifferentCacheAndTempShareOnePrimary()
{
    QTemporaryDir profile(kShortTemp);
    QTemporaryDir cacheA(kShortTemp);
    QTemporaryDir cacheB(kShortTemp);
    QTemporaryDir tmpA(kShortTemp);
    QTemporaryDir tmpB(kShortTemp);
    QVERIFY(profile.isValid() && cacheA.isValid() && cacheB.isValid() && tmpA.isValid()
            && tmpB.isValid());
    const QString previous = mervin::ConfigPaths::overrideDir();
    const auto restore = qScopeGuard([&] { mervin::ConfigPaths::setOverrideDir(previous); });
    mervin::ConfigPaths::setOverrideDir(profile.path());
    const RuntimeRootOverride noRuntimeDir(profile.path() + QStringLiteral("/no-runtime"));

    SingleInstanceServer primary;
    {
        const EnvOverride cache("XDG_CACHE_HOME", QFile::encodeName(cacheA.path()));
        const EnvOverride tmp("TMPDIR", QFile::encodeName(tmpA.path()));
        QCOMPARE(primary.start(kTestPipe), Start::Started);
    }
    const EnvOverride cache("XDG_CACHE_HOME", QFile::encodeName(cacheB.path()));
    const EnvOverride tmp("TMPDIR", QFile::encodeName(tmpB.path()));
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("e.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(primary.takeOpens().size(), 1);
}

void TstSingleInstance::instanceFolderMustBePrivate_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("open to others") << QStringLiteral("open");
    QTest::newRow("a link to a private folder") << QStringLiteral("link");
}

// An instance folder that already exists and is not this user's own 0700 folder, or is a
// link, is refused, not repaired. Without a folder for the lock, a launch runs on its own
// at once, and nothing goes where a link points.
void TstSingleInstance::instanceFolderMustBePrivate()
{
    QFETCH(QString, kind);
    QTemporaryDir profile(kShortTemp);
    QTemporaryDir target(kShortTemp);
    QVERIFY(profile.isValid() && target.isValid());
    const ProfileOverride profileOverride(profile.path());
    const NoRuntimeDir noRuntimeDir;
    QString dir;
    {
        SingleInstanceServer first;
        QCOMPARE(first.start(kTestPipe), Start::Started);
        QCOMPARE(instanceFolders(profile.path()).size(), 1);
        dir = QDir(profile.path()).filePath(instanceFolders(profile.path()).first());
    }
    // Another user's folder does not count either.
    QVERIFY(ownPrivateDir(dir));
    QVERIFY(!mervin::ipc::isPrivateDir(dir, uint(::geteuid()) + 1));

    if (kind == QLatin1String("open")) {
        QVERIFY(QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                               | QFile::ReadOther | QFile::ExeOther));
    } else {
        QVERIFY(QDir(dir).removeRecursively());
        QVERIFY(QFile::link(target.path(), dir));
    }
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile, QString());
    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Failed);
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(s, Message::open({}), 5000, kTestPipe),
             Launch::Uncoordinated);
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(QDir(target.path()).isEmpty());
    if (kind == QLatin1String("open"))
        QVERIFY(!ownPrivateDir(dir));
}

void TstSingleInstance::tooLongPathFallsBack_data()
{
    QTest::addColumn<QString>("folder");   // the folder whose socket path is sized
    QTest::addColumn<int>("over");         // bytes over the longest socket path Qt takes
    QTest::addColumn<QString>("expected"); // where the lock and the socket go
    QTest::newRow("runtime folder far too deep") << QStringLiteral("runtime") << 100
                                                 << QStringLiteral("instance");
    QTest::newRow("runtime socket path one byte too long")
        << QStringLiteral("runtime") << 1 << QStringLiteral("instance");
    QTest::newRow("runtime socket path at the limit")
        << QStringLiteral("runtime") << 0 << QStringLiteral("runtime");
    QTest::newRow("profile folder far too deep") << QStringLiteral("instance") << 100
                                                 << QStringLiteral("socket folder");
    QTest::newRow("instance socket path one byte too long")
        << QStringLiteral("instance") << 1 << QStringLiteral("socket folder");
    QTest::newRow("instance socket path at the limit")
        << QStringLiteral("instance") << 0 << QStringLiteral("instance");
}

// A folder whose socket path Qt would refuse gives way. Without /run/user/<uid> the lock
// goes to the profile's instance folder. If the socket path is too long there, the lock
// stays and the socket goes to this user's folder in /tmp, which the primary records for
// launches. Qt keeps a byte of sockaddr_un for the terminating zero, and on Linux one more
// for abstract names.
void TstSingleInstance::tooLongPathFallsBack()
{
    QFETCH(QString, folder);
    QFETCH(int, over);
    QFETCH(QString, expected);
    QTemporaryDir root(kShortTemp);
    QVERIFY(root.isValid());
    const int limit = kSocketPathLimit;
    const QString uid = QString::number(uint(::geteuid()));
    const int runtimeSuffix = 1 + int(uid.size()) + 1 + int(kTestPipe.size());
    const int instanceSuffix = int(QStringLiteral("/instance-").size()) + 8 + 1
        + int(kTestPipe.size());
    // The name of a folder in `root` whose socket path, after `suffix` more bytes, is
    // `over` bytes over the limit.
    const int name = limit + over
        - (folder == QLatin1String("runtime") ? runtimeSuffix : instanceSuffix)
        - int(root.path().size()) - 1;
    QVERIFY2(name > 0, qPrintable(QStringLiteral("%1 is too deep for this test").arg(root.path())));
    const QString sized = root.path() + QLatin1Char('/') + QString(name, QLatin1Char('x'));
    // As /run/user, which holds this user's runtime folder <uid>.
    const QString runtimeRoot =
        folder == QLatin1String("runtime") ? sized : root.filePath(QStringLiteral("r"));
    const QString runtimeDir =
        folder == QLatin1String("runtime") ? makeUserRuntimeDir(runtimeRoot) : QString();
    QVERIFY(folder != QLatin1String("runtime") || !runtimeDir.isEmpty());
    const QString profile =
        folder == QLatin1String("instance") ? sized : root.filePath(QStringLiteral("p"));
    const RuntimeRootOverride rootOverride(runtimeRoot);
    const ProfileOverride profileOverride(profile);

    const mervin::ipc::InstancePaths paths = mervin::ipc::instancePaths(kTestPipe);
    if (expected == QLatin1String("runtime")) {
        QCOMPARE(paths.server, QDir(runtimeDir).filePath(kTestPipe));
        QCOMPARE(paths.lockFile, QDir(runtimeDir).filePath(kTestPipe + QStringLiteral(".lock")));
    } else {
        QVERIFY2(paths.lockFile.startsWith(profile + QStringLiteral("/instance-")),
                 qPrintable(paths.lockFile));
        if (expected == QLatin1String("instance"))
            QCOMPARE(QFileInfo(paths.server).path(), QFileInfo(paths.lockFile).path());
        else
            QCOMPARE(paths.server, QDir(socketFolder(socketRoot_.path())).filePath(kTestPipe));
    }
    if (over == 0)
        QCOMPARE(QFile::encodeName(paths.server).size(), qsizetype(limit));

    // Qt listens there, and a launch reaches it, through the primary's record when the
    // socket is not next to the lock.
    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Started);
    QCOMPARE(mervin::ipc::endpointOf(paths), paths.server);
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("a.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(s.takeOpens().size(), 1);
    if (expected == QLatin1String("socket folder"))
        QVERIFY(ownPrivateDir(socketFolder(socketRoot_.path())));
}

void TstSingleInstance::socketFolderMustBePrivate_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("open to others") << QStringLiteral("open");
    QTest::newRow("a link to a private folder") << QStringLiteral("link");
}

// /tmp is shared by every user, so the socket folder there must already be this user's
// own 0700 folder or be made one. Anything else is refused, not repaired. The lock in the
// profile's instance folder still keeps the profile's files to one copy, so a later launch
// runs standalone, not as a second owner.
void TstSingleInstance::socketFolderMustBePrivate()
{
    QFETCH(QString, kind);
    QTemporaryDir root(kShortTemp);
    QTemporaryDir socketRoot(kShortTemp);
    QTemporaryDir target(kShortTemp);
    QVERIFY(root.isValid() && socketRoot.isValid() && target.isValid());
    const QString deep = root.path() + QLatin1Char('/') + QString(110, QLatin1Char('d'));
    QVERIFY(QDir().mkpath(deep));
    const ProfileOverride profileOverride(deep);
    const NoRuntimeDir noRuntimeDir;
    mervin::ipc::setSocketRootForTests(socketRoot.path());
    const auto restoreRoot =
        qScopeGuard([&] { mervin::ipc::setSocketRootForTests(socketRoot_.path()); });
    const QString fallback = socketFolder(socketRoot.path());
    if (kind == QLatin1String("open")) {
        QVERIFY(QDir().mkdir(fallback));
        QVERIFY(QFile::setPermissions(fallback, QFile::ReadOwner | QFile::WriteOwner
                                                    | QFile::ExeOwner | QFile::ReadOther
                                                    | QFile::ExeOther));
    } else {
        QVERIFY(QFile::setPermissions(target.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::link(target.path(), fallback));
    }

    const mervin::ipc::InstancePaths paths = mervin::ipc::instancePaths(kTestPipe);
    QVERIFY2(paths.lockFile.startsWith(deep + QStringLiteral("/instance-")),
             qPrintable(paths.lockFile));
    QCOMPARE(paths.server, QString());
    SingleInstanceServer primary;
    QCOMPARE(primary.start(kTestPipe), Start::Started);
    QVERIFY(!primary.isListening());
    QVERIFY(QFile::exists(paths.lockFile));
    QVERIFY(QDir(target.path()).isEmpty());
    if (kind == QLatin1String("open"))
        QVERIFY(!ownPrivateDir(fallback));

    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("s.pdf")}), 600,
                                         kTestPipe),
             Launch::Standalone);
    QVERIFY(!launch.isListening());
}

// Launches can reach one profile folder through different paths, as through a link, and
// so compute different sockets when one path is too long for a socket. They take the same
// lock, and a launch finds the primary through the endpoint it recorded.
void TstSingleInstance::launchFindsPrimaryThroughAnotherPath()
{
    QTemporaryDir root(kShortTemp);
    QVERIFY(root.isValid());
    const QString profile = root.filePath(QStringLiteral("p"));
    const QString link = root.path() + QLatin1Char('/') + QString(95, QLatin1Char('l'));
    QVERIFY(QDir().mkpath(profile));
    QVERIFY(QFile::link(profile, link));
    const NoRuntimeDir noRuntimeDir;
    SingleInstanceServer primary;
    mervin::ipc::InstancePaths direct;
    {
        const ProfileOverride profileOverride(profile);
        QCOMPARE(primary.start(kTestPipe), Start::Started);
        direct = mervin::ipc::instancePaths(kTestPipe);
    }
    QCOMPARE(QFileInfo(direct.server).path(), QFileInfo(direct.lockFile).path());

    const ProfileOverride profileOverride(link);
    const mervin::ipc::InstancePaths linked = mervin::ipc::instancePaths(kTestPipe);
    QCOMPARE(QFileInfo(linked.lockFile).canonicalFilePath(),
             QFileInfo(direct.lockFile).canonicalFilePath());
    QVERIFY(linked.server != direct.server);
    QCOMPARE(mervin::ipc::endpointOf(linked), direct.server);
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("l.pdf")}), 1000,
                                         kTestPipe),
             Launch::Delivered);
    QCOMPARE(primary.takeOpens().size(), 1);
}

void TstSingleInstance::profileFolderWithoutSockets_data()
{
    QTest::addColumn<bool>("socketFolderUsable");
    QTest::newRow("the socket folder in /tmp takes it") << true;
    QTest::newRow("the socket folder is refused too") << false;
}

// Some file systems hold no sockets, as sshfs, vboxsf and some CIFS mounts. When listening
// next to the lock in the profile's instance folder fails, the primary listens in this
// user's socket folder in /tmp instead and records that, and a launch hands off there.
// Only if that fails too does the primary keep the lock without an endpoint, so a launch
// runs standalone.
void TstSingleInstance::profileFolderWithoutSockets()
{
    QFETCH(bool, socketFolderUsable);
    QTemporaryDir profile(kShortTemp);
    QTemporaryDir socketRoot(kShortTemp);
    QVERIFY(profile.isValid() && socketRoot.isValid());
    const ProfileOverride profileOverride(profile.path());
    const NoRuntimeDir noRuntimeDir;
    mervin::ipc::setSocketRootForTests(socketRoot.path());
    const auto restoreRoot =
        qScopeGuard([&] { mervin::ipc::setSocketRootForTests(socketRoot_.path()); });
    if (!socketFolderUsable) {
        QVERIFY(QDir().mkdir(socketFolder(socketRoot.path())));
        QVERIFY(QFile::setPermissions(socketFolder(socketRoot.path()),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                          | QFile::ReadOther | QFile::ExeOther));
    }
    const mervin::ipc::InstancePaths paths = mervin::ipc::instancePaths(kTestPipe);
    QCOMPARE(QFileInfo(paths.server).path(), QFileInfo(paths.lockFile).path());
    // A folder in the socket's place, which removeServer() cannot remove, makes bind()
    // fail there, as such a file system does.
    QVERIFY(QDir().mkdir(paths.server));

    SingleInstanceServer primary;
    QCOMPARE(primary.start(kTestPipe), Start::Started);
    QVERIFY(QFile::exists(paths.lockFile));
    SingleInstanceServer launch;
    if (socketFolderUsable) {
        QVERIFY(primary.isListening());
        const QString moved = QDir(socketFolder(socketRoot.path())).filePath(kTestPipe);
        QCOMPARE(mervin::ipc::endpointOf(paths), moved);
        QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("f.pdf")}),
                                             1000, kTestPipe),
                 Launch::Delivered);
        QCOMPARE(primary.takeOpens().size(), 1);
    } else {
        QVERIFY(!primary.isListening());
        QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("f.pdf")}),
                                             600, kTestPipe),
                 Launch::Standalone);
    }
    QVERIFY(!launch.isListening());
}

// MPDF-20: a cleaner that removes a live primary's lock file must not let the next
// launch take over its socket.
void TstSingleInstance::deletedLockKeepsLivePrimary()
{
    SingleInstanceServer s1;
    QCOMPARE(s1.start(kTestPipe), Start::Started);
    QVERIFY(QFile::remove(mervin::ipc::instancePaths(kTestPipe).lockFile));

    SingleInstanceServer s2;
    QCOMPARE(s2.start(kTestPipe), Start::Running);
    QVERIFY(!s2.isListening());
    QCOMPARE(mervin::ipc::handOff(testEndpoint(), Message::open({QStringLiteral("a.pdf")}),
                                  QDeadlineTimer(1000)),
             Handoff::Delivered);
    QCOMPARE(s1.takeOpens().size(), 1);
}

// A socket file nothing listens on, as a crashed primary leaves, is replaced.
void TstSingleInstance::deadSocketIsReplaced()
{
    const QByteArray path = QFile::encodeName(testEndpoint());
    QFile::remove(QString::fromLocal8Bit(path));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    QVERIFY(size_t(path.size()) < sizeof(addr.sun_path));
    ::memcpy(addr.sun_path, path.constData(), path.size() + 1);
    QCOMPARE(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr), 0);
    ::close(fd);
    QVERIFY(QFile::exists(QString::fromLocal8Bit(path)));

    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Started);
    QCOMPARE(mervin::ipc::handOff(testEndpoint(), Message::open({QStringLiteral("a.pdf")}),
                                  QDeadlineTimer(1000)),
             Handoff::Delivered);
}
#endif

#ifdef Q_OS_LINUX
namespace {
// What a copy from before the move does with an Open.
enum class Legacy {
    Answers, // acknowledges it
    Closes,  // closes the connection without an answer, as one that is quitting
    Silent,  // reads it and never answers, as one whose GUI thread hangs
};

// A copy from before the single-instance files left the temp folder, listening under its
// old name and speaking the same protocol.
class LegacyCopy
{
public:
    LegacyCopy(const QString &endpoint, Legacy mode)
        : thread_(QThread::create([this, endpoint, mode] {
            QLocalServer server;
            QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
                while (QLocalSocket *socket = server.nextPendingConnection()) {
                    auto read = [this, socket, mode,
                                 decoder = mervin::ipc::MessageDecoder()]() mutable {
                        const QByteArray bytes = socket->readAll();
                        {
                            const QMutexLocker locker(&mutex_);
                            received_ += bytes;
                        }
                        for (const Message &m : decoder.feed(bytes)) {
                            if (m.cmd != Message::Cmd::Open || mode == Legacy::Silent)
                                continue;
                            if (mode == Legacy::Closes) {
                                socket->abort();
                                return;
                            }
                            socket->write(Message::ack(QStringLiteral("open")).encode());
                            socket->flush();
                        }
                    };
                    QObject::connect(socket, &QLocalSocket::readyRead, socket, std::move(read));
                }
            });
            listening = server.listen(endpoint);
            QEventLoop().exec();
        }))
    {
        thread_->start();
    }
    ~LegacyCopy()
    {
        thread_->quit();
        thread_->wait();
    }

    // Everything the launches wrote to this copy.
    QByteArray received() const
    {
        const QMutexLocker locker(&mutex_);
        return received_;
    }

    std::atomic<bool> listening = false;

private:
    mutable QMutex mutex_;
    QByteArray received_;
    std::unique_ptr<QThread> thread_;
};
} // namespace

// Older copies named the group from $USERNAME or $USER and listened in the temp folder.
void TstSingleInstance::legacyNameMatchesOlderCopies()
{
    const ProfileOverride defaultProfile{QString()}; // the installed app's group
    const EnvOverride userName("USERNAME", QByteArray());
    const EnvOverride user("USER", "someone");
    const QByteArray digest =
        QCryptographicHash::hash("someone", QCryptographicHash::Sha1).toHex().left(16);
    QCOMPARE(mervin::ipc::legacyEndpoint(),
             QDir(QDir::tempPath()).filePath(QStringLiteral("MervinPDF-")
                                             + QString::fromLatin1(digest)));
    QCOMPARE(mervin::ipc::legacyEndpoint(kTestPipe), QDir(QDir::tempPath()).filePath(kTestPipe));
}

// After an upgrade while an older copy runs, a launch hands its files to that copy, in the
// bytes an older launch sent, instead of starting a second primary beside it.
void TstSingleInstance::legacyCopyGetsTheOpen()
{
    LegacyCopy legacy(mervin::ipc::legacyEndpoint(kTestPipe), Legacy::Answers);
    QTRY_VERIFY(legacy.listening);
    const Message open =
        Message::open({QStringLiteral("/home/a/old.pdf")}, QStringLiteral("new-window"));
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 3000, kTestPipe), Launch::Delivered);
    QVERIFY(!launch.isListening());
    QTRY_COMPARE(legacy.received(), open.encode());

    // The launch took no lock of its own.
    SingleInstanceServer next;
    QCOMPARE(next.start(kTestPipe), Start::Started);
}

// An older copy's name in the temp folder can be anyone's socket. Another user's process
// gets no paths, and the launch becomes the primary.
void TstSingleInstance::legacyCopyOfAnotherUserGetsNothing()
{
    LegacyCopy legacy(mervin::ipc::legacyEndpoint(kTestPipe), Legacy::Answers);
    QTRY_VERIFY(legacy.listening);
    const mervin::ipc::UserLookup other = [](const QLocalSocket &s) {
        const std::optional<QByteArray> user = mervin::ipc::peerUser(s);
        return user ? std::optional(*user + QByteArray("-other")) : user;
    };
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("/home/a/p.pdf")}),
                                         3000, kTestPipe, other),
             Launch::Primary);
    QVERIFY(launch.isListening());
    QTest::qWait(200);
    QVERIFY(legacy.received().isEmpty());
}

// An older copy that closes the connection without an answer is quitting. The launch then
// becomes the primary.
void TstSingleInstance::quittingLegacyCopyIsSkipped()
{
    LegacyCopy legacy(mervin::ipc::legacyEndpoint(kTestPipe), Legacy::Closes);
    QTRY_VERIFY(legacy.listening);
    SingleInstanceServer launch;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("q.pdf")}), 3000,
                                         kTestPipe),
             Launch::Primary);
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QTRY_VERIFY(!legacy.received().isEmpty());
}

// An older copy that takes the open and never answers keeps it, and opens the files once
// it runs again. With no copy of this version holding the lock, the launch opens them
// standalone, as beside a primary of this version that does not answer.
void TstSingleInstance::silentLegacyCopyMeansStandalone()
{
    LegacyCopy legacy(mervin::ipc::legacyEndpoint(kTestPipe), Legacy::Silent);
    QTRY_VERIFY(legacy.listening);
    const Message open = Message::open({QStringLiteral("/home/a/s.pdf")});
    SingleInstanceServer launch;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 600, kTestPipe), Launch::Standalone);
    QVERIFY2(timer.elapsed() >= 500, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(!launch.isListening());
    QTRY_COMPARE(legacy.received(), open.encode()); // once
    QTest::qWait(100);
    QCOMPARE(legacy.received(), open.encode());

    // The launch took no lock of its own.
    SingleInstanceServer next;
    QCOMPARE(next.start(kTestPipe), Start::Started);
}

// A copy of this version that holds the lock takes the launches, with the whole wait, and
// an older copy beside it is not asked, even if it hangs.
void TstSingleInstance::newerPrimaryGoesBeforeLegacyCopy()
{
    SingleInstanceServer primary;
    QCOMPARE(primary.start(kTestPipe), Start::Started);
    LegacyCopy legacy(mervin::ipc::legacyEndpoint(kTestPipe), Legacy::Silent);
    QTRY_VERIFY(legacy.listening);
    const Message open = Message::open({QStringLiteral("/home/a/n.pdf")});
    SingleInstanceServer launch;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 600, kTestPipe), Launch::Delivered);
    QVERIFY2(timer.elapsed() < 400, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(primary.takeOpens().size(), 1);
    QTest::qWait(100);
    QVERIFY(legacy.received().isEmpty());
}

// A socket file an older copy left behind when it crashed is passed over at once.
void TstSingleInstance::deadLegacySocketIsIgnored()
{
    const QByteArray path = QFile::encodeName(mervin::ipc::legacyEndpoint(kTestPipe));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    QVERIFY(size_t(path.size()) < sizeof(addr.sun_path));
    ::memcpy(addr.sun_path, path.constData(), path.size() + 1);
    QCOMPARE(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr), 0);
    ::close(fd);
    const auto remove = qScopeGuard([&] { QFile::remove(QString::fromLocal8Bit(path)); });

    SingleInstanceServer launch;
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, Message::open({QStringLiteral("d.pdf")}), 3000,
                                         kTestPipe),
             Launch::Primary);
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
}
#endif

QTEST_GUILESS_MAIN(TstSingleInstance)
#include "tst_single_instance.moc"
