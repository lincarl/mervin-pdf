#include "ipc/Message.h"

#include <QTest>

using mervin::ipc::Message;
using mervin::ipc::MessageDecoder;

class TstMessage : public QObject
{
    Q_OBJECT

private slots:
    void roundTripOpen();
    void roundTripAck();
    void partialFrameByteByByte();
    void coalescedFrames();
    void splitAcrossBoundary();
    void malformedFrameSkipped();
    void unknownCommandTolerated();
    void extraFieldsTolerated();
    void blankLinesIgnored();
    void oversizeBufferOverflows();
};

void TstMessage::roundTripOpen()
{
    const QStringList paths{QStringLiteral("C:/docs/a.pdf"),
                            QStringLiteral("C:/Users/Łukasz/д.pdf")};
    MessageDecoder dec;
    const auto msgs = dec.feed(Message::open(paths, QStringLiteral("new-window")).encode());
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
    QCOMPARE(msgs[0].paths, paths);
    QCOMPARE(msgs[0].behavior, QStringLiteral("new-window"));
}

void TstMessage::roundTripAck()
{
    MessageDecoder dec;
    const auto msgs = dec.feed(Message::ack(QStringLiteral("open")).encode());
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Ack);
    QCOMPARE(msgs[0].ref, QStringLiteral("open"));
}

void TstMessage::partialFrameByteByByte()
{
    const QByteArray wire = Message::open({QStringLiteral("a.pdf")}).encode();
    MessageDecoder dec;
    // Feeding every byte but the last yields nothing; the trailing '\n' completes it.
    for (int i = 0; i < wire.size() - 1; ++i)
        QVERIFY(dec.feed(wire.mid(i, 1)).isEmpty());
    const auto msgs = dec.feed(wire.right(1));
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
}

void TstMessage::coalescedFrames()
{
    QByteArray wire;
    wire += Message::open({QStringLiteral("a.pdf")}).encode();
    wire += Message::ack(QStringLiteral("open")).encode();
    wire += Message::open({QStringLiteral("b.pdf")}).encode();

    MessageDecoder dec;
    const auto msgs = dec.feed(wire);
    QCOMPARE(msgs.size(), 3);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
    QCOMPARE(msgs[1].cmd, Message::Cmd::Ack);
    QCOMPARE(msgs[2].cmd, Message::Cmd::Open);
}

void TstMessage::splitAcrossBoundary()
{
    const QByteArray wire = Message::open({QStringLiteral("a.pdf")}).encode();
    const int mid = wire.size() / 2;
    MessageDecoder dec;
    QVERIFY(dec.feed(wire.left(mid)).isEmpty());
    const auto msgs = dec.feed(wire.mid(mid));
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
}

void TstMessage::malformedFrameSkipped()
{
    QByteArray wire = "this is not json\n";
    wire += Message::ack(QStringLiteral("open")).encode();
    MessageDecoder dec;
    const auto msgs = dec.feed(wire);
    QCOMPARE(msgs.size(), 1); // malformed line dropped, valid one survives
    QCOMPARE(msgs[0].cmd, Message::Cmd::Ack);
}

void TstMessage::unknownCommandTolerated()
{
    MessageDecoder dec;
    const auto msgs = dec.feed(QByteArray("{\"v\":1,\"cmd\":\"frobnicate\",\"x\":5}\n"));
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Unknown);
    QCOMPARE(msgs[0].rawCmd, QStringLiteral("frobnicate"));
}

void TstMessage::extraFieldsTolerated()
{
    MessageDecoder dec;
    const auto msgs = dec.feed(
        QByteArray("{\"v\":1,\"cmd\":\"open\",\"paths\":[\"a.pdf\"],\"future\":true}\n"));
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
    QCOMPARE(msgs[0].paths, QStringList{QStringLiteral("a.pdf")});
}

void TstMessage::blankLinesIgnored()
{
    MessageDecoder dec;
    const auto msgs = dec.feed(QByteArray("\n\n") + Message::open({QStringLiteral("a.pdf")}).encode() + "\n");
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs[0].cmd, Message::Cmd::Open);
}

void TstMessage::oversizeBufferOverflows()
{
    MessageDecoder dec;
    bool overflow = false;
    // > 1 MiB without a newline must trip the overflow guard.
    dec.feed(QByteArray(MessageDecoder::kMaxBufferBytes + 16, 'x'), &overflow);
    QVERIFY(overflow);
    // After overflow the buffer is reset; a subsequent valid frame still parses.
    overflow = false;
    const auto msgs = dec.feed(Message::open({QStringLiteral("a.pdf")}).encode(), &overflow);
    QVERIFY(!overflow);
    QCOMPARE(msgs.size(), 1);
}

QTEST_GUILESS_MAIN(TstMessage)
#include "tst_message.moc"
