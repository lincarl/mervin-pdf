#include "ipc/Message.h"

#include <QJsonDocument>
#include <QTest>

using mervin::ipc::Message;
using mervin::ipc::MessageDecoder;

namespace {

// Independent wire input pins compatibility with already-running app versions.
const QByteArray kOpen = R"({"v":1,"cmd":"open","paths":["C:/docs/a.pdf","C:/Users/Łukasz/д.pdf","line\nbreak\".pdf"],"behavior":"new-window"})" "\n";
const QByteArray kAck = R"({"v":1,"cmd":"ack","ref":"open"})" "\n";
const QStringList kPaths{QStringLiteral("C:/docs/a.pdf"),
                         QStringLiteral("C:/Users/Łukasz/д.pdf"),
                         QStringLiteral("line\nbreak\".pdf")};

// Check the payload as well as the command, including frame ordering.
void compareOpenAndAck(const QList<Message> &messages)
{
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[0].cmd, Message::Cmd::Open);
    QCOMPARE(messages[0].paths, kPaths);
    QCOMPARE(messages[0].behavior, QStringLiteral("new-window"));
    QCOMPARE(messages[1].cmd, Message::Cmd::Ack);
    QCOMPARE(messages[1].ref, QStringLiteral("open"));
}

} // namespace

class TstMessage : public QObject
{
    Q_OBJECT

private slots:
    void encodesCompatibleFrame_data();
    void encodesCompatibleFrame();
    void partialFrameByteByByte();
    void coalescedFramesAtEverySplit_data();
    void coalescedFramesAtEverySplit();
    void malformedFrameSkipped_data();
    void malformedFrameSkipped();
    void unknownCommandTolerated();
    void extraFieldsTolerated();
    void blankLinesIgnored();
    void oversizeBufferOverflows();
    void resetDropsAnUnfinishedFrame();
};

void TstMessage::encodesCompatibleFrame_data()
{
    QTest::addColumn<Message>("message");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("open-with-behavior") << Message::open(kPaths, QStringLiteral("new-window")) << kOpen;
    QTest::newRow("open-with-default-behavior")
        << Message::open({QStringLiteral("a.pdf")})
        << QByteArray(R"({"v":1,"cmd":"open","paths":["a.pdf"]})");
    QTest::newRow("ack") << Message::ack(QStringLiteral("open")) << kAck;
}

void TstMessage::encodesCompatibleFrame()
{
    QFETCH(Message, message);
    QFETCH(QByteArray, expected);
    const QByteArray wire = message.encode();
    QVERIFY(wire.endsWith('\n'));
    QCOMPARE(wire.count('\n'), 1); // path newlines must be escaped, not frame separators
    QJsonParseError error;
    const auto actual = QJsonDocument::fromJson(wire, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    // JSON member order is irrelevant. Field names, values and types are the contract.
    QCOMPARE(actual, QJsonDocument::fromJson(expected));
}

void TstMessage::partialFrameByteByByte()
{
    MessageDecoder dec;
    for (qsizetype i = 0; i < kOpen.size() - 1; ++i)
        QVERIFY(dec.feed(kOpen.mid(i, 1)).isEmpty());
    const auto messages = dec.feed(kOpen.right(1));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages[0].cmd, Message::Cmd::Open);
    QCOMPARE(messages[0].paths, kPaths);
    QCOMPARE(messages[0].behavior, QStringLiteral("new-window"));
    QVERIFY(dec.feed({}).isEmpty());
}

void TstMessage::coalescedFramesAtEverySplit_data()
{
    QTest::addColumn<int>("split");
    // Includes a whole chunk, the newline boundary, and splits inside UTF-8 and escapes.
    for (int split = 0; split <= kOpen.size() + kAck.size(); ++split)
        QTest::newRow(qPrintable(QStringLiteral("split-%1").arg(split))) << split;
}

void TstMessage::coalescedFramesAtEverySplit()
{
    QFETCH(int, split);
    const QByteArray wire = kOpen + kAck;
    MessageDecoder dec;
    auto messages = dec.feed(wire.left(split));
    const int complete = split >= wire.size() ? 2 : split >= kOpen.size() ? 1 : 0;
    QCOMPARE(messages.size(), complete);
    messages += dec.feed(wire.mid(split));
    compareOpenAndAck(messages);
    QVERIFY(dec.feed({}).isEmpty());
}

void TstMessage::malformedFrameSkipped_data()
{
    QTest::addColumn<QByteArray>("frame");
    QTest::newRow("not-json") << QByteArray("this is not json");
    QTest::newRow("truncated-object") << QByteArray(R"({"cmd":"open")");
    QTest::newRow("array") << QByteArray(R"([{"cmd":"open"}])");
}

void TstMessage::malformedFrameSkipped()
{
    QFETCH(QByteArray, frame);
    MessageDecoder dec;
    compareOpenAndAck(dec.feed(kOpen + frame + '\n' + kAck));
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
    QVERIFY(msgs[0].behavior.isEmpty());
}

void TstMessage::blankLinesIgnored()
{
    MessageDecoder dec;
    compareOpenAndAck(dec.feed(QByteArray("\n \r\n") + kOpen + "\t\n" + kAck + "\n"));
}

void TstMessage::oversizeBufferOverflows()
{
    MessageDecoder dec;
    bool overflow = true;
    QVERIFY(dec.feed(QByteArray(MessageDecoder::kMaxBufferBytes, 'x'), &overflow).isEmpty());
    QVERIFY(!overflow); // exactly the cap is allowed while waiting for the newline
    QVERIFY(dec.feed("x", &overflow).isEmpty());
    QVERIFY(overflow); // the limit applies across reads, not just to one chunk
    compareOpenAndAck(dec.feed(kOpen + kAck, &overflow));
    QVERIFY(!overflow); // overflow discards the unfinished frame and resets the flag

    QVERIFY(dec.feed(QByteArray(MessageDecoder::kMaxBufferBytes + 1, 'x'), &overflow).isEmpty());
    QVERIFY(overflow); // a single oversized read is rejected too
}

void TstMessage::resetDropsAnUnfinishedFrame()
{
    MessageDecoder dec;
    QVERIFY(dec.feed(kOpen.left(kOpen.size() / 2)).isEmpty());
    dec.reset();
    compareOpenAndAck(dec.feed(kOpen + kAck));
}

QTEST_GUILESS_MAIN(TstMessage)
#include "tst_message.moc"
