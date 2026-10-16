#include "recent/RecentStore.h"

#include "config/ConfigPaths.h"
#include "recent/MountTable.h"
#include "recent/PathKey.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>

#ifndef Q_OS_WIN
#include <sys/statvfs.h>

#include <cerrno>
#endif

namespace mervin {

RecentStore::RecentStore(int retention)
    : retention_(retention)
{
}

void RecentStore::setRetention(int retention)
{
    retention_ = retention;
    trim();
}

bool RecentStore::add(const QString &path, qint64 whenMs, int pageCount, const QString &mount)
{
    if (path.isEmpty())
        return false;

    const QString key = normalizePathKey(path);
    bool wasFavorite = false;
    int knownPages = 0;
    for (int i = 0; i < entries_.size(); ++i) {
        if (normalizePathKey(entries_.at(i).path) == key) {
            wasFavorite = entries_.at(i).favorite; // preserve the flag on re-open
            knownPages = entries_.at(i).pageCount;
            entries_.removeAt(i);
            break; // keys are unique, so at most one match
        }
    }

    RecentEntry e;
    e.path = path;
    e.lastOpened = whenMs;
    e.pageCount = pageCount > 0 ? pageCount : knownPages;
    e.favorite = wasFavorite;
    e.mount = mount;
    entries_.prepend(e);
    trim();
    return true;
}

bool RecentStore::setFavorite(const QString &path, bool favorite)
{
    if (path.isEmpty())
        return false;
    const QString key = normalizePathKey(path);
    for (RecentEntry &e : entries_) {
        if (normalizePathKey(e.path) == key) {
            if (e.favorite == favorite)
                return false;
            e.favorite = favorite;
            return true;
        }
    }
    return false;
}

bool RecentStore::remove(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QString key = normalizePathKey(path);
    for (int i = 0; i < entries_.size(); ++i) {
        if (normalizePathKey(entries_.at(i).path) == key) {
            entries_.removeAt(i);
            return true;
        }
    }
    return false;
}

QStringList RecentStore::removeEntries(const QStringList &paths)
{
    QSet<QString> keys;
    keys.reserve(paths.size());
    for (const QString &path : paths) {
        if (!path.isEmpty())
            keys.insert(normalizePathKey(path));
    }
    if (keys.isEmpty())
        return {};

    QStringList removed;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (!keys.contains(normalizePathKey(it->path))) {
            ++it;
            continue;
        }
        removed.append(it->path);
        it = entries_.erase(it);
    }
    return removed;
}

bool RecentStore::setMount(const QString &path, const QString &mount)
{
    const QString key = normalizePathKey(path);
    for (RecentEntry &e : entries_) {
        if (normalizePathKey(e.path) == key) {
            if (e.mount == mount)
                return false;
            e.mount = mount;
            return true;
        }
    }
    return false;
}

bool RecentStore::setMissingMounts(const QHash<QString, QString> &mounts)
{
    QHash<QString, QString> byKey;
    byKey.reserve(mounts.size());
    for (auto it = mounts.cbegin(); it != mounts.cend(); ++it) {
        if (!it.key().isEmpty() && !it.value().isEmpty())
            byKey.insert(normalizePathKey(it.key()), it.value());
    }
    if (byKey.isEmpty())
        return false;

    bool changed = false;
    for (RecentEntry &e : entries_) {
        if (!e.mount.isEmpty())
            continue;
        const auto found = byKey.constFind(normalizePathKey(e.path));
        if (found == byKey.cend())
            continue;
        e.mount = *found;
        changed = true;
    }
    return changed;
}

namespace {

#ifdef Q_OS_WIN
// The share (//server/share) or drive (D:) holding `p`, an absolute path with
// forward slashes, or "" when it has neither.
QString windowsVolume(const QString &p)
{
    if (p.startsWith(QLatin1String("//"))) {
        const QStringList parts = p.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        return parts.size() >= 2 ? QStringLiteral("//%1/%2").arg(parts.at(0), parts.at(1))
                                 : QString();
    }
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':'))
        return p.left(2);
    return {};
}
#else
// The file systems Linux lists in /proc/self/mountinfo, which is read without
// touching any volume. Empty when it cannot be read.
QList<mounts::Mount> kernelMounts()
{
    QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
    return mountInfo.open(QIODevice::ReadOnly) ? mounts::parseMountInfo(mountInfo.readAll())
                                               : QList<mounts::Mount>();
}

// The file systems mounted now. QStorageInfo::mountedVolumes(), used only without
// mountinfo, queries each one, which stalls on an offline share and mounts every
// automount point.
QList<mounts::Mount> mountedNow()
{
    const QList<mounts::Mount> kernel = kernelMounts();
    if (!kernel.isEmpty())
        return kernel;
    QList<mounts::Mount> listed;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes())
        listed.append({volume.rootPath(), volume.fileSystemType()});
    return listed;
}

// The folders /etc/fstab mounts volumes on, mounted or not, as written. Resolving
// links in them would look inside other volumes and could mount an unrelated
// automount point or wait on an offline share.
QStringList configuredMounts()
{
    QFile fstab(QStringLiteral("/etc/fstab"));
    return fstab.open(QIODevice::ReadOnly) ? mounts::parseFstab(fstab.readAll()) : QStringList();
}

// Whether the file system mounted on `folder` answers. A network share can stay
// mounted after its server went away.
bool answers(const QString &folder)
{
    const QByteArray name = QFile::encodeName(folder);
    struct statvfs info;
    int result;
    do {
        result = ::statvfs(name.constData(), &info);
    } while (result == -1 && errno == EINTR);
    return result == 0;
}
#endif

// Whether the volume holding `absPath` is attached, given the mount recorded
// for the file, if any (see isVolumeAttached).
bool volumeAttached(const QString &absPath, const QString &mount)
{
#ifdef Q_OS_WIN
    Q_UNUSED(mount);
    const QString volume = windowsVolume(QDir::fromNativeSeparators(absPath));
    return !volume.isEmpty() && QDir(volume + QLatin1Char('/')).exists();
#else
    // Recent and the session store canonical paths, without links, as the
    // mount list does.
    const QString p = QDir::fromNativeSeparators(absPath);
    // GVFS (GNOME's network and phone mounts) gives each share its own folder
    // under /run/user/<uid>/gvfs, which exists only while the share is mounted.
    const int gvfs = p.indexOf(QLatin1String("/gvfs/"));
    if (p.startsWith(QLatin1String("/run/user/")) && gvfs > 0) {
        const int shareEnd = p.indexOf(QLatin1Char('/'), gvfs + 6);
        return shareEnd > 0 && QDir(p.left(shareEnd)).exists();
    }
    const QList<mounts::Mount> mounted = mountedNow();
    // A share mounted by hand outside fstab leaves an empty folder once it is
    // unmounted, which the mount list cannot tell from a folder whose file was
    // deleted. While the recorded mount's folder is still there and
    // another file system holds the path, the volume is away. Otherwise the mount
    // list decides, so a recorded mount never makes a volume look attached. That
    // covers a folder that went with its volume, as udisks and per-login mounts
    // such as kio-fuse leave it.
    if (!mount.isEmpty() && mounts::deepestMount(p, mounted) != mount && QFileInfo(mount).isDir())
        return false;
    const QString holder = mounts::holdingMount(p, mounted, configuredMounts());
    return !holder.isEmpty() && answers(holder);
#endif
}

} // namespace

bool RecentStore::isRemovedFromDisk(const QString &path, const QString &mount)
{
    if (path.isEmpty())
        return false;
    const QFileInfo fi(path);
    return !fi.exists() && volumeAttached(fi.absoluteFilePath(), mount);
}

bool RecentStore::isVolumeAttached(const QString &path, const QString &mount)
{
    return !path.isEmpty() && volumeAttached(QFileInfo(path).absoluteFilePath(), mount);
}

QString RecentStore::mountOf(const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path);
    return {};
#else
    // Only the kernel's list says which file system holds the path. Without it
    // nothing is recorded, and the entry keeps the mount list rules.
    const QList<mounts::Mount> mounted = kernelMounts();
    if (path.isEmpty() || mounted.isEmpty())
        return {};
    return mounts::deepestMount(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()),
                                mounted);
#endif
}

QString RecentStore::recordedMount(const QString &path) const
{
    const QString key = normalizePathKey(path);
    for (const RecentEntry &e : entries_) {
        if (normalizePathKey(e.path) == key)
            return e.mount;
    }
    return {};
}

QString RecentStore::volumeKey(const QString &path)
{
    const QString p = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    const QString volume = windowsVolume(p);
    return (volume.isEmpty() ? p : volume).toLower(); // Windows paths ignore case
#else
    return QFileInfo(p).path();
#endif
}

void RecentStore::clear()
{
    entries_.clear();
}

void RecentStore::trim()
{
    if (retention_ <= 0)
        return;
    // Newest first, so the entries past the first `retention_` others are the
    // oldest. Favourites never count.
    int others = 0;
    entries_.removeIf([this, &others](const RecentEntry &e) {
        return !e.favorite && ++others > retention_;
    });
}

bool RecentStore::load(const QString &file)
{
    entries_.clear();

    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false; // no history yet is normal, not an error to report
    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return false;

    const QJsonArray arr = doc.array();
    entries_.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        RecentEntry e;
        e.path = o.value(QStringLiteral("p")).toString();
        e.lastOpened = static_cast<qint64>(o.value(QStringLiteral("t")).toDouble());
        e.pageCount = o.value(QStringLiteral("n")).toInt(0);
        e.favorite = o.value(QStringLiteral("f")).toBool(false);
        e.mount = o.value(QStringLiteral("m")).toString();
        if (!e.path.isEmpty())
            entries_.append(e);
    }
    trim();
    return true;
}

bool RecentStore::save(const QString &file) const
{
    QSaveFile f(file); // atomic write: a crash mid-save can't corrupt the file
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(toJson());
    return f.commit();
}

QByteArray RecentStore::toJson() const
{
    QJsonArray arr;
    for (const RecentEntry &e : entries_) {
        QJsonObject o;
        o.insert(QStringLiteral("p"), e.path);
        o.insert(QStringLiteral("t"), e.lastOpened);
        if (e.pageCount > 0)
            o.insert(QStringLiteral("n"), e.pageCount);
        if (e.favorite)
            o.insert(QStringLiteral("f"), true);
        if (!e.mount.isEmpty())
            o.insert(QStringLiteral("m"), e.mount);
        arr.append(o);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QString RecentStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recent.json"));
}

} // namespace mervin
