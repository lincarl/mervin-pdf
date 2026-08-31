#pragma once

#include <QDir>
#include <QString>
#include <QtGlobal>

namespace mervin {

// A normalized comparison key for a filesystem path. On Windows paths are
// case-insensitive and separator-agnostic, so two spellings of the same file
// ("C:\\A.pdf" vs "c:/a.pdf") must collapse to a single history entry and a
// single view-state record. The original spelling is still stored for display;
// only matching/dedup uses this key.
inline QString normalizePathKey(const QString &path)
{
    QString key = QDir::cleanPath(path); // normalize separators to '/', resolve . / ..
#ifdef Q_OS_WIN
    key = key.toLower();
#endif
    return key;
}

} // namespace mervin
