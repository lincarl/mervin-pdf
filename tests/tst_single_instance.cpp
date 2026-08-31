#include "ipc/Message.h"
#include "ipc/SingleInstanceServer.h"

#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>

using mervin::ipc::Message;
using mervin::ipc::SingleInstanceServer;

// A unique-ish name so the test never collides with a real running host.
static const QString kTestPipe = QStringLiteral("MervinPDF-unittest-si");

class TstSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondListenFails();
    void relistenAfterClose();
    void twoClientsDeliverMessages();
};

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
