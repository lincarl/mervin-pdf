#include "config/ConfigPaths.h"
#include "ipc/Handoff.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QScopeGuard>
#include <QSemaphore>
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
#ifdef Q_OS_UNIX
    void filesLiveInRuntimeDir();
    void noRuntimeDirUsesPrivateCacheFolder();
    void tooLongPathFallsBack();
    void tempFolderMustBePrivate();
    void deletedLockKeepsLivePrimary();
    void deadSocketIsReplaced();
#endif

private:
#ifdef Q_OS_UNIX
    // Keeps the tests out of the user's real runtime directory.
    QTemporaryDir runtime_;
    std::optional<EnvOverride> runtimeEnv_;
#endif
};

void TstSingleInstance::initTestCase()
{
#ifdef Q_OS_UNIX
    QVERIFY(runtime_.isValid());
    runtimeEnv_.emplace("XDG_RUNTIME_DIR", QFile::encodeName(runtime_.path()));
#endif
}

void TstSingleInstance::cleanupTestCase()
{
#ifdef Q_OS_UNIX
    runtimeEnv_.reset();
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
    QThread::sleep(3); // this thread runs no events meanwhile
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
    QTest::addColumn<bool>("otherUser");
    QTest::newRow("same user") << false;
    QTest::newRow("another user") << true;
}

// A launch checks who runs the listening process, through the same lookup a real launch
// uses, and sends no path to another user's process.
void TstSingleInstance::peerCheck()
{
    QFETCH(bool, otherUser);
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

    // Another user's process as the kernel would report it.
    const mervin::ipc::UserLookup other = [](const QLocalSocket &s) {
        const std::optional<QByteArray> user = mervin::ipc::peerUser(s);
        return user ? std::optional(*user + QByteArray("-other")) : user;
    };
    const Message open = Message::open({QStringLiteral("/home/a/private.pdf")});
    const Handoff result = otherUser
        ? mervin::ipc::handOff(testEndpoint(), open, QDeadlineTimer(500), other)
        : mervin::ipc::handOff(testEndpoint(), open, QDeadlineTimer(500));
    QCOMPARE(result, otherUser ? Handoff::NoPrimary : Handoff::Delivered);
    QCOMPARE(opensWithin(server, 100).size(), otherUser ? 0 : 1);
    if (!otherUser)
        return;

    // A whole launch never hands its files to that process. The lock is held, so it
    // keeps trying, then opens standalone.
    SingleInstanceServer launch;
    QCOMPARE(mervin::ipc::claimOrHandOff(launch, open, 600, kTestPipe, other), Launch::Standalone);
    QCOMPARE(opensWithin(server, 200).size(), 0);
}

#ifdef Q_OS_UNIX
namespace {
// Whether `dir` is this user's folder with mode 0700.
bool ownPrivateDir(const QString &dir)
{
    return mervin::ipc::isPrivateDir(dir, uint(::geteuid()));
}

// The temp-folder fallback for this user.
QString tempFallback(const QString &tmp)
{
    return QDir(tmp).filePath(QStringLiteral("mervin-pdf-%1").arg(uint(::geteuid())));
}
} // namespace

void TstSingleInstance::filesLiveInRuntimeDir()
{
    QTemporaryDir runtime;
    QTemporaryDir tmp;
    QVERIFY(runtime.isValid() && tmp.isValid());
    const EnvOverride env("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
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
    const QString lock = QDir(runtime.path()).filePath(kTestPipe + QStringLiteral(".lock"));
    const QString socket = QDir(runtime.path()).filePath(kTestPipe);
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile, lock);
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).server, socket);
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

void TstSingleInstance::noRuntimeDirUsesPrivateCacheFolder()
{
    QTemporaryDir cache;
    QTemporaryDir tmp;
    QVERIFY(cache.isValid() && tmp.isValid());
    const EnvOverride runtime("XDG_RUNTIME_DIR", QByteArray());
    const EnvOverride cacheHome("XDG_CACHE_HOME", QFile::encodeName(cache.path()));
    const EnvOverride tmpEnv("TMPDIR", QFile::encodeName(tmp.path()));
    const QDir mervinCache(QDir(cache.path()).filePath(QStringLiteral("mervin-pdf")));

    QString dir;
    {
        SingleInstanceServer s;
        QCOMPARE(s.start(kTestPipe), Start::Started);
        // One folder per machine, as machines can share a home folder.
        const QStringList folders = mervinCache.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), 1);
        QVERIFY2(folders.first().startsWith(QStringLiteral("run-")) && folders.first().size() == 12,
                 qPrintable(folders.first()));
        dir = mervinCache.filePath(folders.first());
        QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile,
                 QDir(dir).filePath(kTestPipe + QStringLiteral(".lock")));
        QVERIFY(QFile::exists(QDir(dir).filePath(kTestPipe + QStringLiteral(".lock"))));
        QVERIFY(QFile::exists(QDir(dir).filePath(kTestPipe)));
        QVERIFY(ownPrivateDir(dir));
    }

    // A folder there that is not this user's own directory is not used. The lock goes to
    // this user's private folder in the temp folder instead.
    QVERIFY(QDir(dir).removeRecursively());
    QTemporaryDir elsewhere;
    QVERIFY(QFile::link(elsewhere.path(), dir));
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile,
             QDir(tempFallback(tmp.path())).filePath(kTestPipe + QStringLiteral(".lock")));
    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Started);
    QVERIFY(QDir(elsewhere.path()).isEmpty());
    QVERIFY(ownPrivateDir(tempFallback(tmp.path())));
}

void TstSingleInstance::tooLongPathFallsBack()
{
    QTemporaryDir root;
    QTemporaryDir tmp;
    QVERIFY(root.isValid() && tmp.isValid());
    const QString deep = root.path() + QStringLiteral("/") + QString(110, QLatin1Char('d'));
    QVERIFY(QDir().mkpath(deep));
    QVERIFY(QFile::setPermissions(deep, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString shortCache = root.filePath(QStringLiteral("c"));
    const EnvOverride tmpEnv("TMPDIR", QFile::encodeName(tmp.path()));
    const QString lockName = kTestPipe + QStringLiteral(".lock");

    {
        // A runtime folder too deep for a socket path gives way to the cache folder.
        const EnvOverride runtime("XDG_RUNTIME_DIR", QFile::encodeName(deep));
        const EnvOverride cache("XDG_CACHE_HOME", QFile::encodeName(shortCache));
        const QString lock = mervin::ipc::instancePaths(kTestPipe).lockFile;
        QVERIFY2(lock.startsWith(shortCache + QStringLiteral("/mervin-pdf/run-")), qPrintable(lock));
        SingleInstanceServer s;
        QCOMPARE(s.start(kTestPipe), Start::Started);
    }
    {
        // A cache folder too deep as well gives way to the private temp folder.
        const EnvOverride runtime("XDG_RUNTIME_DIR", QByteArray());
        const EnvOverride cache("XDG_CACHE_HOME", QFile::encodeName(deep));
        QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile,
                 QDir(tempFallback(tmp.path())).filePath(lockName));
        SingleInstanceServer s;
        QCOMPARE(s.start(kTestPipe), Start::Started);
        QVERIFY(QFile::exists(QDir(tempFallback(tmp.path())).filePath(kTestPipe)));
        QVERIFY(ownPrivateDir(tempFallback(tmp.path())));
    }
}

// The temp folder is shared by every user, so its fallback folder must already be this
// user's own 0700 folder. Anything else is refused, not repaired, and without any usable
// folder a launch runs on its own at once.
void TstSingleInstance::tempFolderMustBePrivate()
{
    QTemporaryDir root;
    QTemporaryDir tmp;
    QVERIFY(root.isValid() && tmp.isValid());
    const QString deep = root.path() + QStringLiteral("/") + QString(110, QLatin1Char('d'));
    QVERIFY(QDir().mkpath(deep));
    const EnvOverride runtime("XDG_RUNTIME_DIR", QByteArray());
    const EnvOverride cache("XDG_CACHE_HOME", QFile::encodeName(deep));
    const EnvOverride tmpEnv("TMPDIR", QFile::encodeName(tmp.path()));
    const QString fallback = tempFallback(tmp.path());

    // Another user's folder.
    QVERIFY(QDir().mkdir(fallback));
    QVERIFY(QFile::setPermissions(fallback, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(ownPrivateDir(fallback));
    QVERIFY(!mervin::ipc::isPrivateDir(fallback, uint(::geteuid()) + 1));

    // A folder that others could open.
    QVERIFY(QFile::setPermissions(fallback, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                                | QFile::ReadOther | QFile::ExeOther));
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile, QString());
    QVERIFY(!ownPrivateDir(fallback));

    // A link to a private folder.
    QVERIFY(QDir(fallback).removeRecursively());
    QTemporaryDir target;
    QVERIFY(QFile::link(target.path(), fallback));
    QVERIFY(QFile::setPermissions(target.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QCOMPARE(mervin::ipc::instancePaths(kTestPipe).lockFile, QString());

    SingleInstanceServer s;
    QCOMPARE(s.start(kTestPipe), Start::Failed);
    QVERIFY(QDir(target.path()).isEmpty());
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(mervin::ipc::claimOrHandOff(s, Message::open({}), 5000, kTestPipe),
             Launch::Uncoordinated);
    QVERIFY(timer.elapsed() < 1000);
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

QTEST_GUILESS_MAIN(TstSingleInstance)
#include "tst_single_instance.moc"
