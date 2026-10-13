#include "io/MemoryFiles.h"

#include <QLocale>
#include <QResource>
#include <QtEndian>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace mervin::memoryfiles {

namespace {

void append32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

void append16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToBigEndian(value, bytes);
    out.append(bytes, 2);
}

// One registered root: its data must stay valid while it is registered.
struct Published
{
    QString root; // without the leading ':'
    std::unique_ptr<QByteArray> data;
};

} // namespace

QByteArray resourceData(QList<QPair<QString, QByteArray>> files)
{
    // Qt finds a name by binary search over the children's name hashes.
    std::sort(files.begin(), files.end(),
              [](const auto &a, const auto &b) { return qt_hash(a.first) < qt_hash(b.first); });

    // A tree of 14-byte nodes, the root folder first, then the data and the names.
    QByteArray tree;
    QByteArray data;
    QByteArray names;
    append32(tree, 0);      // root: no name
    append16(tree, 0x02);   // a folder
    append32(tree, quint32(files.size()));
    append32(tree, 1);      // its first child
    for (const auto &[name, contents] : std::as_const(files)) {
        append32(tree, quint32(names.size()));
        append16(names, quint16(name.size()));
        append32(names, qt_hash(name));
        for (const QChar c : name)
            append16(names, c.unicode());
        append16(tree, 0);                         // a file, uncompressed
        append16(tree, 0);                         // any territory
        append16(tree, quint16(QLocale::C));       // the locale-neutral file
        append32(tree, quint32(data.size()));
        append32(data, quint32(contents.size()));
        data.append(contents);
    }

    constexpr quint32 kHeader = 20; // "qres", version, and three offsets
    QByteArray out("qres");
    append32(out, 1);
    append32(out, kHeader);
    append32(out, kHeader + quint32(tree.size()));
    append32(out, kHeader + quint32(tree.size() + data.size()));
    out += tree;
    out += data;
    out += names;
    return out;
}

QString publish(const QString &prefix, const QList<QPair<QString, QByteArray>> &files)
{
    static std::map<QString, Published> current; // by prefix
    static quint64 serial = 0;

    Published next;
    next.root = QStringLiteral("/%1-%2").arg(prefix).arg(++serial);
    next.data = std::make_unique<QByteArray>(resourceData(files));
    QResource::registerResource(reinterpret_cast<const uchar *>(next.data->constData()), next.root);

    Published &slot = current[prefix];
    // Qt keeps reading a root it could not unregister, so its data stays.
    static std::vector<std::unique_ptr<QByteArray>> retired;
    if (slot.data
        && !QResource::unregisterResource(reinterpret_cast<const uchar *>(slot.data->constData()),
                                          slot.root))
        retired.push_back(std::move(slot.data));
    slot = std::move(next);
    return QLatin1Char(':') + slot.root;
}

} // namespace mervin::memoryfiles
