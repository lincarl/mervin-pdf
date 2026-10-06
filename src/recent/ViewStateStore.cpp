#include "recent/ViewStateStore.h"

#include "config/ConfigPaths.h"
#include "recent/PathKey.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace mervin {

ViewStateStore::ViewStateStore(int retention)
    : retention_(retention)
{
}

void ViewStateStore::setRetention(int retention)
{
    retention_ = retention;
    prune();
}

void ViewStateStore::put(const QString &path, const ViewState &state, qint64 whenMs)
{
    if (path.isEmpty())
        return;
    states_.insert(normalizePathKey(path), Record{state, whenMs});
    prune();
}

std::optional<ViewState> ViewStateStore::get(const QString &path) const
{
    const auto it = states_.constFind(normalizePathKey(path));
    if (it == states_.constEnd())
        return std::nullopt;
    return it->state;
}

bool ViewStateStore::remove(const QString &path)
{
    return states_.remove(normalizePathKey(path));
}

void ViewStateStore::clear()
{
    states_.clear();
}

void ViewStateStore::prune()
{
    if (retention_ <= 0)
        return;
    // Evict the least-recently-updated entries until within the cap. Puts are
    // infrequent (a tab close), so a linear min-scan per eviction is fine.
    while (states_.size() > retention_) {
        auto victim = states_.begin();
        for (auto it = states_.begin(); it != states_.end(); ++it)
            if (it->updated < victim->updated)
                victim = it;
        states_.erase(victim);
    }
}

bool ViewStateStore::load(const QString &file)
{
    states_.clear();

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
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const QJsonObject o = it.value().toObject();
        ViewState s;
        s.page = o.value(QStringLiteral("pg")).toInt(s.page);
        s.zoomMode = o.value(QStringLiteral("zm")).toString(s.zoomMode);
        s.scale = o.value(QStringLiteral("sc")).toDouble(s.scale);
        s.rotation = o.value(QStringLiteral("rot")).toInt(s.rotation);
        s.offsetX = o.value(QStringLiteral("ox")).toDouble(s.offsetX);
        s.offsetY = o.value(QStringLiteral("oy")).toDouble(s.offsetY);
        const qint64 upd = static_cast<qint64>(o.value(QStringLiteral("upd")).toDouble());
        states_.insert(it.key(), Record{s, upd});
    }
    prune();
    return true;
}

bool ViewStateStore::save(const QString &file) const
{
    QJsonObject root;
    for (auto it = states_.constBegin(); it != states_.constEnd(); ++it) {
        const Record &r = it.value();
        QJsonObject o;
        o.insert(QStringLiteral("pg"), r.state.page);
        o.insert(QStringLiteral("zm"), r.state.zoomMode);
        o.insert(QStringLiteral("sc"), r.state.scale);
        o.insert(QStringLiteral("rot"), r.state.rotation);
        o.insert(QStringLiteral("ox"), r.state.offsetX);
        o.insert(QStringLiteral("oy"), r.state.offsetY);
        o.insert(QStringLiteral("upd"), r.updated);
        root.insert(it.key(), o);
    }

    QSaveFile f(file); // atomic write
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return f.commit();
}

QString ViewStateStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("viewstate.json"));
}

} // namespace mervin
