#include "app/Relaunch.h"

#include "update/Installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QSaveFile>

#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin::relaunch {

namespace {
bool g_requested = false;
bool g_due = false;              // a copy should start for carried launches
QList<ipc::Message> g_carried;   // not written to the carry file yet
std::function<bool()> g_starter;

// The launches in a carry file, oldest first: {"written": msecs since the epoch,
// "launches": [Open messages]}. Empty when the file is missing, unreadable or stale.
QList<ipc::Message> readCarried(const QString &file)
{
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    const qint64 written = qint64(root.value(QStringLiteral("written")).toDouble());
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - written;
    if (age < -60 * 1000 || age > kCarriedMaxAgeMs) // a clock that moved back a little is fine
        return {};
    QList<ipc::Message> launches;
    for (const QJsonValue &value : root.value(QStringLiteral("launches")).toArray()) {
        const ipc::Message open = ipc::Message::fromJson(value.toObject());
        if (open.cmd == ipc::Message::Cmd::Open)
            launches.append(open);
    }
    return launches;
}
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

void carry(const ipc::Message &open)
{
    g_due = true;
    g_carried.append(open);
}

bool carrying()
{
    return g_due;
}

void nextCopyStarted()
{
    g_due = false;
}

bool writeCarried(const QString &file)
{
    if (g_carried.isEmpty())
        return true;
    if (file.isEmpty())
        return false;
    // Launches carried by a primary whose next copy has not started yet go first.
    QJsonArray launches;
    for (const ipc::Message &open : readCarried(file) + std::exchange(g_carried, {}))
        launches.append(open.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("written"), double(QDateTime::currentMSecsSinceEpoch()));
    root.insert(QStringLiteral("launches"), launches);
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    // The folder is this user's own already. The file's mode says so as well, from before
    // it takes its name.
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return out.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0 && out.commit();
}

QList<ipc::Message> takeCarried(const QString &file)
{
    if (file.isEmpty() || !QFile::exists(file))
        return {};
    const QList<ipc::Message> launches = readCarried(file);
    QFile::remove(file);
    return launches;
}

bool startIfDue(bool restart)
{
    if (!(restart && g_requested) && !g_due)
        return true;
    g_due = false;
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
