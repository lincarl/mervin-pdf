#include "app/Relaunch.h"

#include "update/Installer.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin::relaunch {

namespace {
bool g_requested = false;
} // namespace

void request()
{
    g_requested = true;
}

bool requested()
{
    return g_requested;
}

QString program(const QString &appImage, const QString &applicationFile)
{
    if (!appImage.isEmpty() && QFileInfo(appImage).isFile())
        return appImage;
    return applicationFile;
}

bool start()
{
    const QString exe =
        program(qEnvironmentVariable("APPIMAGE"), QCoreApplication::applicationFilePath());
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
