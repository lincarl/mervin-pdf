#include "config/ConfigPaths.h"

#include <QDir>
#include <QSet>
#include <QStandardPaths>

#include <mutex>

namespace mervin {

namespace {

// Set by --profile (see main.cpp); redirects the whole state directory.
QString g_overrideDir;

// Creates `dir` the first time it is asked for, at startup, so later calls from the
// UI thread do not touch the disk. The background writer creates a folder that
// went missing meanwhile (see appfiles::write).
const QString &made(const QString &dir)
{
    static std::mutex mutex;
    static QSet<QString> done;
    std::lock_guard<std::mutex> lock(mutex);
    if (!done.contains(dir)) {
        QDir().mkpath(dir);
        done.insert(dir);
    }
    return dir;
}

} // namespace

void ConfigPaths::setOverrideDir(const QString &dir)
{
    g_overrideDir = dir.isEmpty() ? QString() : QDir(dir).absolutePath();
}

QString ConfigPaths::overrideDir()
{
    return g_overrideDir;
}

QString ConfigPaths::configDir()
{
    if (!g_overrideDir.isEmpty())
        return made(g_overrideDir);
#ifdef Q_OS_WIN
    // The spec calls for %APPDATA%/MervinPDF/ exactly (not Org/App nested).
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString dir = QDir(base).filePath(QStringLiteral("MervinPDF"));
#else
    // Linux/XDG: a flat ~/.config/mervin-pdf. GenericConfigLocation is
    // $XDG_CONFIG_HOME itself (no org/app suffix that AppConfigLocation would add,
    // which - with our org "Mervin" + app "Mervin PDF" - would triple-nest).
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    const QString dir = QDir(base).filePath(QStringLiteral("mervin-pdf"));
#endif
    return made(dir);
}

QString ConfigPaths::configFile()
{
    return QDir(configDir()).filePath(QStringLiteral("config.toml"));
}

QString ConfigPaths::updatesDir()
{
    QString dir;
    if (!g_overrideDir.isEmpty()) {
        dir = QDir(g_overrideDir).filePath(QStringLiteral("updates"));
    } else {
#ifdef Q_OS_WIN
        // %LOCALAPPDATA%, not %APPDATA%: installers must not roam with the profile.
        QString base = qEnvironmentVariable("LOCALAPPDATA");
        if (base.isEmpty())
            base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        dir = QDir(base).filePath(QStringLiteral("MervinPDF/updates"));
#else
        QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/.cache");
        dir = QDir(base).filePath(QStringLiteral("mervin-pdf/updates"));
#endif
    }
    return made(dir);
}

} // namespace mervin
