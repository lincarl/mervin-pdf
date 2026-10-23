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

#include <cstdio>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin::relaunch {

namespace {
bool g_requested = false;
bool g_due = false;            // a copy should start for carried launches
QList<ipc::Message> g_carried; // not in a carry file yet
std::function<bool(const QStringList &)> g_starter;

// The launches in a carry file, oldest first: {"written": msecs since the epoch,
// "launches": [Open messages]}. A missing or expired file holds no launches.
// Read or format failures are distinct so callers do not discard unread data.
std::optional<QList<ipc::Message>> readCarried(const QString &file)
{
    QFile in(file);
    if (!in.exists())
        return QList<ipc::Message>{};
    if (!in.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray bytes = in.readAll();
    if (in.error() != QFileDevice::NoError)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("written")).isDouble()
        || !root.value(QStringLiteral("launches")).isArray())
        return std::nullopt;
    const qint64 written = root.value(QStringLiteral("written")).toInteger();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (written < now - kCarriedMaxAgeMs || written > now + kCarriedMaxAheadMs)
        return QList<ipc::Message>{};
    QList<ipc::Message> launches;
    for (const QJsonValue &value : root.value(QStringLiteral("launches")).toArray()) {
        const ipc::Message open = ipc::Message::fromJson(value.toObject());
        if (open.cmd == ipc::Message::Cmd::Open)
            launches.append(open);
    }
    return launches;
}

// The files of the carried launches that no carry file holds.
QStringList unwrittenPaths()
{
    QStringList paths;
    for (const ipc::Message &open : std::as_const(g_carried))
        paths += open.paths;
    return paths;
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
    const auto previous = readCarried(file);
    if (!previous)
        return false;
    // Launches carried by a primary whose next copy has not started yet go first.
    QJsonArray launches;
    for (const ipc::Message &open : *previous + g_carried)
        launches.append(open.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("written"), double(QDateTime::currentMSecsSinceEpoch()));
    root.insert(QStringLiteral("launches"), launches);
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    // The folder is this user's own already. The file's mode says so as well, from before
    // it takes its name.
    if (!out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (out.write(bytes) != bytes.size() || !out.commit())
        return false;
    // Only now does the file hold them. Until then they stay for startIfDue.
    g_carried.clear();
    return true;
}

QList<ipc::Message> takeCarried(const QString &file)
{
    if (file.isEmpty() || !QFile::exists(file))
        return {};
    const auto launches = readCarried(file);
    if (!launches || !QFile::remove(file)) {
        std::fprintf(stderr, "MervinPDF: could not consume carried launches from %s\n",
                     qUtf8Printable(QDir::toNativeSeparators(file)));
        return {};
    }
    return *launches;
}

bool startIfDue(bool restart)
{
    // Carried launches that no carry file holds, because writing it failed.
    const QStringList paths = unwrittenPaths();
    if (!(restart && g_requested) && !g_due) {
        // An update started the next copy before the carry file failed.
        for (const QString &path : paths)
            std::fprintf(stderr, "MervinPDF: could not pass on %s\n",
                         qUtf8Printable(QDir::toNativeSeparators(path)));
        return g_carried.isEmpty();
    }
    const bool due = g_due;
    if (!paths.isEmpty())
        std::fprintf(stderr, "MervinPDF: passing the files opened during the quit to the "
                             "next copy as arguments\n");
    if (!(g_starter ? g_starter(paths) : start(paths))) {
        g_due = due;
        return false;
    }
    g_carried.clear();
    g_due = false;
    g_requested = false;
    return true;
}

void setStarterForTests(std::function<bool(const QStringList &paths)> starter)
{
    g_starter = std::move(starter);
}

bool start(const QStringList &paths)
{
    const QString exe = program(qEnvironmentVariable("APPIMAGE"), qEnvironmentVariable("APPDIR"),
                                QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    // The new copy's window may take the foreground; this process can still grant that.
    AllowSetForegroundWindow(ASFW_ANY);
    return QProcess::startDetached(exe, update::relaunchArguments() + paths);
#elif defined(Q_OS_LINUX)
    return update::relaunchAfterExit(exe, paths);
#else
    return QProcess::startDetached(exe, update::relaunchArguments() + paths);
#endif
}

} // namespace mervin::relaunch
