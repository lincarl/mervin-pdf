#include "session/SessionStore.h"

#include "config/ConfigPaths.h"
#include "recent/PathKey.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>

#include <utility>

namespace mervin {

bool SessionStore::load(const QString &file)
{
    clear();
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    f.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    const QJsonArray arr = root.value(QStringLiteral("open")).toArray();
    for (const QJsonValue &v : arr)
        if (v.isString() && !v.toString().isEmpty())
            paths_.append(v.toString());

    // Absent in files written before the field existed, and only meaningful when
    // it names one of the open documents - anything else is stale and ignored.
    const QString active = root.value(QStringLiteral("active")).toString();
    if (paths_.contains(active))
        activePath_ = active;

    // Absent in files written before the field existed. A path also listed as
    // open is a live document, not a kept one.
    const QJsonArray kept = root.value(QStringLiteral("unavailable")).toArray();
    for (const QJsonValue &v : kept) {
        const QJsonObject entry = v.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || paths_.contains(path))
            continue;
        unavailable_.append(
            {path, QDate::fromString(entry.value(QStringLiteral("since")).toString(), Qt::ISODate)});
    }

    // Absent in files written before the field existed.
    const QJsonObject mounts = root.value(QStringLiteral("mounts")).toObject();
    for (auto it = mounts.constBegin(); it != mounts.constEnd(); ++it)
        setMount(it.key(), it.value().toString());
    keepListedMounts();
    return true;
}

bool SessionStore::save(const QString &file) const
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(toJson());
    return f.commit();
}

QByteArray SessionStore::toJson() const
{
    QJsonArray arr;
    for (const QString &p : paths_)
        arr.append(p);
    QJsonObject root;
    root.insert(QStringLiteral("open"), arr);
    if (!activePath_.isEmpty())
        root.insert(QStringLiteral("active"), activePath_);
    if (!unavailable_.isEmpty()) {
        QJsonArray kept;
        for (const Unavailable &entry : unavailable_) {
            QJsonObject o;
            o.insert(QStringLiteral("path"), entry.path);
            o.insert(QStringLiteral("since"), entry.since.toString(Qt::ISODate));
            kept.append(o);
        }
        root.insert(QStringLiteral("unavailable"), kept);
    }
    QJsonObject mounts;
    const auto addMount = [&](const QString &path) {
        const QString m = mount(path);
        if (!m.isEmpty())
            mounts.insert(path, m);
    };
    for (const QString &p : paths_)
        addMount(p);
    for (const Unavailable &entry : unavailable_)
        addMount(entry.path);
    if (!mounts.isEmpty())
        root.insert(QStringLiteral("mounts"), mounts);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void SessionStore::setPaths(const QStringList &paths)
{
    paths_ = paths;
    if (!unavailable_.isEmpty()) {
        QSet<QString> open;
        for (const QString &p : paths_)
            open.insert(normalizePathKey(p));
        unavailable_.removeIf([&open](const Unavailable &entry) {
            return open.contains(normalizePathKey(entry.path));
        });
    }
    keepListedMounts();
}

void SessionStore::forgetUnavailable(const QString &path)
{
    const QString key = normalizePathKey(path);
    if (unavailable_.removeIf([&key](const Unavailable &entry) {
            return normalizePathKey(entry.path) == key;
        }) > 0)
        keepListedMounts();
}

void SessionStore::setMount(const QString &path, const QString &mount)
{
    if (mount.isEmpty())
        mounts_.remove(normalizePathKey(path));
    else
        mounts_.insert(normalizePathKey(path), mount);
}

QString SessionStore::mount(const QString &path) const
{
    return mounts_.value(normalizePathKey(path));
}

void SessionStore::keepListedMounts()
{
    if (mounts_.isEmpty())
        return;
    QSet<QString> listed;
    for (const QString &p : std::as_const(paths_))
        listed.insert(normalizePathKey(p));
    for (const Unavailable &entry : std::as_const(unavailable_))
        listed.insert(normalizePathKey(entry.path));
    mounts_.removeIf([&listed](const auto &it) { return !listed.contains(it.key()); });
}

QList<FileCheck> SessionStore::filesToCheck(const QDate &today,
                                            const std::function<QString(const QString &)> &recordedMount)
{
    QList<FileCheck> files;
    const auto add = [&](const QString &path) {
        QString m = mount(path);
        if (m.isEmpty() && recordedMount) {
            m = recordedMount(path);
            setMount(path, m);
        }
        files.append({path, m});
    };
    for (const QString &path : std::as_const(paths_))
        add(path);
    for (const Unavailable &entry : std::as_const(unavailable_))
        if (!expired(entry, today) && !paths_.contains(entry.path))
            add(entry.path);
    return files;
}

bool SessionStore::expired(const Unavailable &entry, const QDate &today)
{
    return keptSince(entry, today).daysTo(today) >= kKeepUnavailableDays;
}

QDate SessionStore::keptSince(const Unavailable &entry, const QDate &today)
{
    // A date that is unreadable or in the future (a wrong clock) counts from today.
    return entry.since.isValid() && entry.since <= today ? entry.since : today;
}

QStringList SessionStore::prepareRestore(
    const QDate &today, const std::function<std::optional<FileFacts>(const QString &)> &found)
{
    enum class Found { File, Gone, Away };
    const auto find = [&](const QString &path) {
        const std::optional<FileFacts> facts = found ? found(path) : std::nullopt;
        if (!facts)
            return Found::Away;
        if (facts->exists)
            return Found::File;
        return facts->goneFromFolder() ? Found::Gone : Found::Away;
    };

    QStringList restore;
    QList<Unavailable> kept;
    for (const QString &path : std::as_const(paths_)) {
        const Found f = find(path);
        if (f == Found::File)
            restore.append(path);
        else if (f == Found::Away)
            kept.append({path, today});
    }
    for (const Unavailable &entry : std::as_const(unavailable_)) {
        if (expired(entry, today) || paths_.contains(entry.path))
            continue;
        const Found f = find(entry.path);
        if (f == Found::File)
            restore.append(entry.path);
        else if (f == Found::Away)
            kept.append({entry.path, keptSince(entry, today)});
    }

    paths_ = restore;
    unavailable_ = kept;
    keepListedMounts();
    if (!paths_.contains(activePath_))
        activePath_.clear();
    return paths_;
}

QString SessionStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("session.json"));
}

} // namespace mervin
