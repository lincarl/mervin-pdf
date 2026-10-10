#include "config/ConfigPaths.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"

#include <QLocalSocket>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using mervin::ipc::Message;
using mervin::ipc::SingleInstanceServer;

// A unique-ish name so the test never collides with a real running host.
static const QString kTestPipe = QStringLiteral("MervinPDF-unittest-si");

namespace {
bool packaged = false;
}

// Substitute process identity so the real lock and IPC implementation can be
// exercised for both installation types on each test platform.
namespace mervin::PlatformIntegration {
bool hasPackageIdentity() { return packaged; }
}

class TstSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondListenFails();
    void relistenAfterClose();
    void twoClientsDeliverMessages();
    void storeAndMsiHostsStaySeparate();
};

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
    QVERIFY(msi.start());
    packaged = true;
    const QString storeName = mervin::ipc::hostPipeName();
    QVERIFY(storeName != msiName);
    SingleInstanceServer store;
    QVERIFY(store.start());
    SingleInstanceServer duplicateStore;
    QVERIFY(!duplicateStore.start());

    QSignalSpy msiMessages(&msi, &SingleInstanceServer::messageReceived);
    QSignalSpy storeMessages(&store, &SingleInstanceServer::messageReceived);
    QLocalSocket client;
    client.connectToServer(storeName);
    QVERIFY(client.waitForConnected(1000));
    client.write(Message::open({QStringLiteral("store.pdf")}, QStringLiteral("new-tab")).encode());
    client.flush();
    QTRY_COMPARE(storeMessages.size(), 1);
    QCOMPARE(msiMessages.size(), 0);
}

void TstSingleInstance::secondListenFails()
{
    SingleInstanceServer s1;
    QVERIFY(s1.start(kTestPipe));
    QVERIFY(s1.isListening());

    // A second server on the same name must be rejected - the single-instance
    // guarantee (AddressInUseError under the hood).
    SingleInstanceServer s2;
    QVERIFY(!s2.start(kTestPipe));
    QVERIFY(!s2.isListening());
}

void TstSingleInstance::relistenAfterClose()
{
    {
        SingleInstanceServer s1;
        QVERIFY(s1.start(kTestPipe));
    } // s1 destroyed -> pipe released

    // No stale-pipe block: a fresh server can immediately take the name.
    SingleInstanceServer s2;
    QVERIFY(s2.start(kTestPipe));
    QVERIFY(s2.isListening());
}

void TstSingleInstance::twoClientsDeliverMessages()
{
    SingleInstanceServer server;
    QVERIFY(server.start(kTestPipe));

    QList<Message> received;
    connect(&server, &SingleInstanceServer::messageReceived, this,
            [&](QLocalSocket *, const Message &m) { received.append(m); });

    QLocalSocket a;
    a.connectToServer(kTestPipe);
    QVERIFY(a.waitForConnected(1000));
    a.write(Message::open({QStringLiteral("y.pdf")}, QStringLiteral("new-window")).encode());
    a.flush();

    QLocalSocket b;
    b.connectToServer(kTestPipe);
    QVERIFY(b.waitForConnected(1000));
    b.write(Message::open({QStringLiteral("x.pdf")}, QStringLiteral("new-tab")).encode());
    b.flush();

    // Delivery is awaited via the event loop (QTRY), not waitForBytesWritten,
    // which can false-negative on Windows when the write already flushed.
    QTRY_COMPARE(received.size(), 2);

    // Order across two sockets isn't guaranteed; assert by content.
    bool sawY = false, sawX = false;
    for (const Message &m : received) {
        if (m.cmd == Message::Cmd::Open && m.paths == QStringList{QStringLiteral("y.pdf")})
            sawY = true;
        if (m.cmd == Message::Cmd::Open && m.paths == QStringList{QStringLiteral("x.pdf")})
            sawX = true;
    }
    QVERIFY(sawY);
    QVERIFY(sawX);
}

QTEST_GUILESS_MAIN(TstSingleInstance)
#include "tst_single_instance.moc"
