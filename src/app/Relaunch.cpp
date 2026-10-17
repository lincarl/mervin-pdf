#include "app/Relaunch.h"

#include "update/Installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>

#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin::relaunch {

namespace {
bool g_requested = false;
bool g_carrying = false;
QStringList g_carried;
std::function<bool()> g_starter;

// Holds the carried files on their way to the next copy, as a JSON array of paths.
constexpr char kPassedVariable[] = "MERVIN_PASSED_OPENS";
} // namespace

void request()
{
    g_requested = true;
}

bool requested()
{
    return g_requested;
}

QString program(const QString &appImage, const QString &appDir, const QString &applicationFile)
{
    // The same rule as update::linuxPackageKind.
    if (!appImage.isEmpty() && !appDir.isEmpty() && QFileInfo(appImage).isFile()
        && applicationFile.startsWith(QDir::cleanPath(appDir) + QLatin1Char('/')))
        return appImage;
    return applicationFile;
}

void carry(const QStringList &paths)
{
    g_carrying = true;
    g_carried += paths;
}

bool carrying()
{
    return g_carrying;
}

void passCarried()
{
    if (!g_carrying)
        return;
    const QJsonDocument files(QJsonArray::fromStringList(std::exchange(g_carried, {})));
    qputenv(kPassedVariable, files.toJson(QJsonDocument::Compact));
    g_carrying = false;
}

QStringList takePassed()
{
    if (!qEnvironmentVariableIsSet(kPassedVariable))
        return {};
    const QJsonDocument files = QJsonDocument::fromJson(qgetenv(kPassedVariable));
    qunsetenv(kPassedVariable);
    QStringList paths;
    for (const QJsonValue &file : files.array())
        if (const QString path = file.toString(); !path.isEmpty())
            paths.append(path);
    return paths;
}

bool startIfDue(bool restart)
{
    if (!(restart && g_requested) && !g_carrying)
        return true;
    passCarried();
    return g_starter ? g_starter() : start();
}

void setStarterForTests(std::function<bool()> starter)
{
    g_starter = std::move(starter);
}

bool start()
{
    const QString exe = program(qEnvironmentVariable("APPIMAGE"), qEnvironmentVariable("APPDIR"),
                                QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    // The new copy's window may take the foreground; this process can still grant that.
    AllowSetForegroundWindow(ASFW_ANY);
    return QProcess::startDetached(exe, update::relaunchArguments());
#elif defined(Q_OS_LINUX)
    return update::relaunchAfterExit(exe);
#else
    return QProcess::startDetached(exe, update::relaunchArguments());
#endif
}

} // namespace mervin::relaunch
