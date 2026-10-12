#include "session/SessionStore.h"

#include "config/ConfigPaths.h"
#include "recent/PathKey.h"
#include "recent/RecentStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    paths_.clear();
    activePath_.clear();
    unavailable_.clear();
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
    return true;
}

bool SessionStore::save(const QString &file) const
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

    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return f.commit();
}

void SessionStore::setPaths(const QStringList &paths)
{
    paths_ = paths;
    if (unavailable_.isEmpty())
        return;
    QSet<QString> open;
    for (const QString &p : paths_)
        open.insert(normalizePathKey(p));
    unavailable_.removeIf([&open](const Unavailable &entry) {
        return open.contains(normalizePathKey(entry.path));
    });
}

QStringList SessionStore::prepareRestore(const QDate &today,
                                         const std::function<bool(const QString &)> &volumeAttached)
{
    enum class Found { File, Gone, Away };
    // Check the volume first. On an unreachable share each check can stall for
    // seconds, and a volume that is away already settles the answer.
    const std::function<bool(const QString &)> attached =
        volumeAttached ? volumeAttached : RecentStore::isVolumeAttached;
    const auto find = [&attached](const QString &path) {
        if (!attached(path))
            return Found::Away;
        return QFileInfo::exists(path) ? Found::File : Found::Gone;
    };

    QStringList restore;
    QList<Unavailable> kept;
    for (const QString &path : std::as_const(paths_)) {
        const Found found = find(path);
        if (found == Found::File)
            restore.append(path);
        else if (found == Found::Away)
            kept.append({path, today});
    }
    for (const Unavailable &entry : std::as_const(unavailable_)) {
        // A date that is unreadable or in the future (a wrong clock) counts from today.
        const QDate since = entry.since.isValid() && entry.since <= today ? entry.since : today;
        if (since.daysTo(today) >= kKeepUnavailableDays || paths_.contains(entry.path))
            continue;
        const Found found = find(entry.path);
        if (found == Found::File)
            restore.append(entry.path);
        else if (found == Found::Away)
            kept.append({entry.path, since});
    }

    paths_ = restore;
    unavailable_ = kept;
    if (!paths_.contains(activePath_))
        activePath_.clear();
    return paths_;
}

QString SessionStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("session.json"));
}

} // namespace mervin
