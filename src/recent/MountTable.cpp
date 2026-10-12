#include "recent/MountTable.h"

#include <QByteArrayView>
#include <QDir>
#include <QFile>

namespace mervin::mounts {

namespace {

// A folder as mountinfo and fstab write it, with octal escapes such as \040 for
// a space.
QString folder(QByteArrayView field)
{
    const auto octal = [field](qsizetype at) {
        return at < field.size() && field[at] >= '0' && field[at] <= '7';
    };
    QByteArray bytes;
    bytes.reserve(field.size());
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            bytes.append(char(((field[i + 1] - '0') << 6) | ((field[i + 2] - '0') << 3)
                              | (field[i + 3] - '0')));
            i += 3;
        } else {
            bytes.append(field[i]);
        }
    }
    return QDir::cleanPath(QFile::decodeName(bytes));
}

// Whether `path` is `folder` or lies inside it.
bool holds(const QString &folder, const QString &path)
{
    if (folder == QLatin1String("/"))
        return path.startsWith(QLatin1Char('/'));
    return path.startsWith(folder)
        && (path.size() == folder.size() || path.at(folder.size()) == QLatin1Char('/'));
}

// An automount (autofs) point mounts the volume on demand, on itself or on a
// folder below it, so it holds no files of its own.
bool automount(const Mount &m)
{
    return m.type == "autofs";
}

} // namespace

QList<Mount> parseMountInfo(const QByteArray &text)
{
    // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
    // The mount point is the fifth field and the type follows the "-", after any
    // optional fields.
    QList<Mount> mounts;
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype separator = fields.indexOf(QByteArrayLiteral("-"), 6);
        if (separator < 0 || separator + 1 >= fields.size() || !fields.at(4).startsWith('/'))
            continue;
        mounts.append({folder(fields.at(4)), fields.at(separator + 1)});
    }
    return mounts;
}

QStringList parseFstab(const QByteArray &text)
{
    // <device> <folder> <type> <options> <dump> <pass>, separated by spaces or
    // tabs. Swap lists "none" or "swap" as its folder.
    QStringList folders;
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 2 || fields.at(0).startsWith('#') || !fields.at(1).startsWith('/'))
            continue;
        folders.append(folder(fields.at(1)));
    }
    return folders;
}

QString deepestMount(const QString &path, const QList<Mount> &mounted)
{
    QString holder = QStringLiteral("/");
    for (const Mount &m : mounted) {
        if (!automount(m) && holds(m.point, path) && m.point.size() > holder.size())
            holder = m.point;
    }
    return holder;
}

QString holdingMount(const QString &path, const QList<Mount> &mounted,
                     const QStringList &configured)
{
    const QString holder = deepestMount(path, mounted);
    QString required;
    for (const Mount &m : mounted) {
        if (automount(m) && holds(m.point, path) && m.point.size() > required.size())
            required = m.point;
    }
    for (const QString &point : configured) {
        if (holds(point, path) && point.size() > required.size())
            required = point;
    }
    // The volume that belongs on the configured or automount folder is not
    // mounted, so the path falls through to the volume below, often the
    // system disk with an empty folder.
    if (holder.size() < required.size())
        return {};

    // Removable and network volumes are mounted below these folders. An
    // unmounted mount point is often still there as an empty folder, and the
    // folders themselves may sit on a mount of their own (/mnt on some hosts),
    // so only a volume mounted below the folder counts.
    static const char *const kDetachable[] = {"/media", "/run/media", "/mnt", "/net"};
    for (const char *name : kDetachable) {
        const QString detachable = QLatin1String(name);
        if (holds(detachable, path) && path.size() > detachable.size()
            && holder.size() <= detachable.size())
            return {};
    }
    return holder;
}

} // namespace mervin::mounts
