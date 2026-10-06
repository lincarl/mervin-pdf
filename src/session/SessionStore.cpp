#include "session/SessionStore.h"

#include "config/ConfigPaths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

namespace mervin {

bool SessionStore::load(const QString &file)
{
    paths_.clear();
    activePath_.clear();
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

    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return f.commit();
}

QString SessionStore::defaultFile()
{
    return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("session.json"));
}

} // namespace mervin
