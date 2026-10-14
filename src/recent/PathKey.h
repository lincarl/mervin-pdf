#pragma once

#include <QDir>
#include <QFileInfo>
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

// A comparison key for a file that may not exist, with links resolved as far as
// the disk allows: the whole path, or else the folder the file would be in. It
// reads the disk, so a folder that stopped answering blocks it.
inline QString fileKey(const QFileInfo &file)
{
    if (file.exists())
        return normalizePathKey(file.canonicalFilePath());
    const QString folder = QFileInfo(file.absolutePath()).canonicalFilePath();
    return normalizePathKey(folder.isEmpty() ? file.absoluteFilePath()
                                             : folder + QLatin1Char('/') + file.fileName());
}

} // namespace mervin
