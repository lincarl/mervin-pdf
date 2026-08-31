#include "config/ConfigPaths.h"

#include <QDir>
#include <QStandardPaths>

namespace mervin {

namespace {

// Set by --profile (see main.cpp); redirects the whole state directory.
QString g_overrideDir;

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
    if (!g_overrideDir.isEmpty()) {
        QDir().mkpath(g_overrideDir);
        return g_overrideDir;
    }
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
    QDir().mkpath(dir);
    return dir;
}

QString ConfigPaths::configFile()
{
    return QDir(configDir()).filePath(QStringLiteral("config.toml"));
}

} // namespace mervin
