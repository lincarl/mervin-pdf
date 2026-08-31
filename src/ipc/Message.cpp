#include "ipc/Message.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace mervin::ipc {

QString Message::cmdToString(Cmd c)
{
    switch (c) {
    case Cmd::Open:
        return QStringLiteral("open");
    case Cmd::Ack:
        return QStringLiteral("ack");
    case Cmd::Unknown:
        break;
    }
    return QString();
}

Message::Cmd Message::cmdFromString(const QString &s)
{
    if (s == QLatin1String("open"))
        return Cmd::Open;
    if (s == QLatin1String("ack"))
        return Cmd::Ack;
    return Cmd::Unknown;
}

Message Message::open(const QStringList &paths, const QString &behavior)
{
    Message m;
    m.cmd = Cmd::Open;
    m.paths = paths;
    m.behavior = behavior;
    return m;
}

Message Message::ack(const QString &ref)
{
    Message m;
    m.cmd = Cmd::Ack;
    m.ref = ref;
    return m;
}

QJsonObject Message::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("v"), kVersion);
    obj.insert(QStringLiteral("cmd"), cmdToString(cmd));

    switch (cmd) {
    case Cmd::Open: {
        QJsonArray arr;
        for (const QString &p : paths)
            arr.append(p);
        obj.insert(QStringLiteral("paths"), arr);
        if (!behavior.isEmpty())
            obj.insert(QStringLiteral("behavior"), behavior);
        break;
    }
    case Cmd::Ack:
        obj.insert(QStringLiteral("ref"), ref);
        break;
    case Cmd::Unknown:
        break;
    }
    return obj;
}


Message Message::fromJson(const QJsonObject &obj)
{
    Message m;
    m.rawCmd = obj.value(QStringLiteral("cmd")).toString();
    m.cmd = cmdFromString(m.rawCmd);

    // Fields are read leniently regardless of cmd so partial/older senders are
    // tolerated; callers act on m.cmd.
    const QJsonArray arr = obj.value(QStringLiteral("paths")).toArray();
    m.paths.reserve(arr.size());
    for (const QJsonValue &v : arr)
        if (v.isString())
            m.paths.append(v.toString());
    m.behavior = obj.value(QStringLiteral("behavior")).toString();
    m.ref = obj.value(QStringLiteral("ref")).toString();
    return m;
}

QByteArray Message::encode() const
{
    return QJsonDocument(toJson()).toJson(QJsonDocument::Compact) + '\n';
}

QList<Message> MessageDecoder::feed(const QByteArray &chunk, bool *overflow)
{
    if (overflow)
        *overflow = false;

    buffer_.append(chunk);

    QList<Message> out;
    int nl;
    while ((nl = buffer_.indexOf('\n')) >= 0) {
        const QByteArray frame = buffer_.left(nl);
        buffer_.remove(0, nl + 1);
        if (frame.trimmed().isEmpty())
            continue; // tolerate blank lines / bare '\n' keep-alives

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(frame, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue; // skip one malformed frame, keep the stream
        out.append(Message::fromJson(doc.object()));
    }

    // A frame that never terminates and exceeds the cap is treated as hostile.
    if (buffer_.size() > kMaxBufferBytes) {
        buffer_.clear();
        if (overflow)
            *overflow = true;
    }
    return out;
}

} // namespace mervin::ipc
