#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace mervin::ipc {

// Control message exchanged over the single-instance QLocalSocket: a secondary
// launch sends Open to the running primary, which replies Ack. Wire format: one
// compact JSON object per line (newline-delimited). Every message carries a
// version field; unknown commands and extra fields are tolerated so instances
// of different versions interoperate.
struct Message
{
    // Protocol version stamped into / read from the "v" field.
    static constexpr int kVersion = 1;

    enum class Cmd {
        Unknown, // unrecognized "cmd" - kept so callers can ignore politely
        Open,    // {paths, behavior?}     secondary -> primary
        Ack,     // {ref}                  primary -> secondary
    };

    Cmd cmd = Cmd::Unknown;

    // open
    QStringList paths;
    QString behavior; // "new-tab" | "new-window" (empty = use default)
    // ack
    QString ref; // command being acknowledged, e.g. "open"

    QString rawCmd; // original "cmd" string (diagnostics; set for Unknown too)

    // Convenience constructors for the messages this app sends.
    static Message open(const QStringList &paths, const QString &behavior = QString());
    static Message ack(const QString &ref);

    QJsonObject toJson() const;
    static Message fromJson(const QJsonObject &obj);

    // Compact JSON serialization terminated with '\n' (a full wire frame).
    QByteArray encode() const;

    static QString cmdToString(Cmd c);
    static Cmd cmdFromString(const QString &s);
};

// Incremental frame decoder: one instance per connection. Accumulates bytes and
// yields whole messages as '\n'-terminated frames arrive. Handles partial reads
// (a frame split across chunks) and coalesced reads (several frames in one
// chunk). If the internal buffer grows past the frame-size cap without a
// newline, the stream is considered malformed: feed() sets *overflow and the
// caller should drop the connection.
class MessageDecoder
{
public:
    static constexpr int kMaxBufferBytes = 1 << 20; // 1 MiB

    QList<Message> feed(const QByteArray &chunk, bool *overflow = nullptr);
    void reset() { buffer_.clear(); }

private:
    QByteArray buffer_;
};

} // namespace mervin::ipc

Q_DECLARE_METATYPE(mervin::ipc::Message)
