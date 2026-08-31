#pragma once

#include "config/ConfigPaths.h"

#include <QDir>
#include <QStandardPaths>

namespace mervin::urlopen {

inline QString downloadDirectory()
{
    // Internet documents belong somewhere visible and user-manageable. Respect
    // the OS-configured Downloads location (including Windows folder redirection
    // and OneDrive). A --profile run stays fully isolated as promised by that flag.
    if (!ConfigPaths::overrideDir().isEmpty()) {
        return QDir(ConfigPaths::configDir()).filePath(QStringLiteral("downloads"));
    }

    const QString standard = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return standard.isEmpty()
               ? QDir(ConfigPaths::configDir()).filePath(QStringLiteral("downloads"))
               : standard;
}

} // namespace mervin::urlopen
