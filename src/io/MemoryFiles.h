#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

// Files kept in memory and reached through Qt's resource system, for code that
// takes a path but must not touch the disk, such as style sheet images
// (image:url(...)), which Qt loads on the UI thread when it paints.
namespace mervin::memoryfiles {

// Makes `files` (a name and its contents each) readable as "<root>/<name>" and
// returns the root, such as ":/mervin-theme-3". Each call gets a new root, so
// caches keyed by path, such as Qt's pixmap cache, never return the old contents.
// The files published under `prefix` before are removed, so publish the new ones
// before anything stops using the old ones. Call on the UI thread.
QString publish(const QString &prefix, const QList<QPair<QString, QByteArray>> &files);

// The contents of a resource file in Qt's binary format (rcc, version 1) holding
// `files` at its root, as publish() registers it.
QByteArray resourceData(QList<QPair<QString, QByteArray>> files);

} // namespace mervin::memoryfiles
