#include "recent/RecentStore.h"

#include "config/ConfigPaths.h"
#include "recent/PathKey.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

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

bool RecentStore::add(const QString &path, qint64 whenMs, int pageCount)
{
    if (path.isEmpty())
        return false;

    const QString key = normalizePathKey(path);
    bool wasFavorite = false;
    for (int i = 0; i < entries_.size(); ++i) {
        if (normalizePathKey(entries_.at(i).path) == key) {
            wasFavorite = entries_.at(i).favorite; // preserve the flag on re-open
            entries_.removeAt(i);
            break; // keys are unique, so at most one match
        }
    }

    RecentEntry e;
    e.path = path;
    e.lastOpened = whenMs;
    e.pageCount = pageCount;
    e.favorite = wasFavorite;
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

QStringList RecentStore::removeMissingFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return {};

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
        if (!keys.contains(normalizePathKey(it->path)) || QFileInfo::exists(it->path)) {
            ++it;
            continue;
        }
        removed.append(it->path);
        it = entries_.erase(it);
    }
    return removed;
}

void RecentStore::clear()
{
    entries_.clear();
}

void RecentStore::trim()
{
    if (retention_ > 0 && entries_.size() > retention_)
        entries_.erase(entries_.begin() + retention_, entries_.end());
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
        if (!e.path.isEmpty())
            entries_.append(e);
    }
    trim();
    return true;
}

bool RecentStore::save(const QString &file) const
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
        arr.append(o);
    }

    QSaveFile f(file); // atomic write: a crash mid-save can't corrupt the file
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return f.commit();
}

QString RecentStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("recent.json"));
}

} // namespace mervin
