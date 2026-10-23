#include "update/Installer.h"

#include "app/Relaunch.h"

#include "config/ConfigPaths.h"
#include "platform/PlatformIntegration.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <cstdio>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

// Set by CMake for the release .deb and .rpm builds (release.yml); empty for
// every other build.
#ifndef MERVIN_PACKAGE_FORMAT
#  define MERVIN_PACKAGE_FORMAT ""
#endif

namespace mervin::update {

QStringList relaunchArguments()
{
    const QString profile = ConfigPaths::overrideDir();
    if (profile.isEmpty())
        return {};
    return {QStringLiteral("--profile"), profile};
}

namespace {

#ifdef Q_OS_WIN
// Windows' System32, resolved directly: a bare "cmd.exe" or "msiexec.exe" would
// also be looked up in the current directory (often the folder of the PDF that
// launched Mervin).
QString systemDir()
{
    wchar_t buf[MAX_PATH];
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return QStringLiteral("C:\\Windows\\System32");
    return QString::fromWCharArray(buf, int(n));
}
#endif

} // namespace

PackageKind installedPackageKind()
{
#if defined(Q_OS_WIN)
    // A Store copy can coexist with MSI. Package identity wins even when the
    // user's MSI install directory or pending update still exists.
    if (PlatformIntegration::hasPackageIdentity())
        return PackageKind::Msix;
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Mervin PDF"),
                        QSettings::NativeFormat);
    return windowsPackageKind(exeDir, reg.value(QStringLiteral("InstallDir")).toString());
#elif defined(Q_OS_LINUX)
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    return linuxPackageKind(QCoreApplication::applicationFilePath(),
                            QFileInfo(appImage).isFile() ? appImage : QString(),
                            qEnvironmentVariable("APPDIR"), QStringLiteral(MERVIN_PACKAGE_FORMAT));
#else
    return PackageKind::None;
#endif
}

#ifdef Q_OS_WIN
bool configureWindowsInstaller(QProcess &process, const QString &file,
                               const QString &executable, const QString &msiexec)
{
    const QString profile = ConfigPaths::overrideDir();
    for (const QString &path : {file, executable, msiexec, profile}) {
        if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\r'))
            || path.contains(QLatin1Char('\n')) || path.contains(QChar::Null))
            return false;
    }
    const QString workDir = QFileInfo(file).absolutePath();
    const QDir system(systemDir());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("MERVIN_UPDATE_INSTALLER"), QDir::toNativeSeparators(msiexec));
    environment.insert(QStringLiteral("MERVIN_UPDATE_PACKAGE"), QDir::toNativeSeparators(file));
    environment.insert(QStringLiteral("MERVIN_UPDATE_EXECUTABLE"), QDir::toNativeSeparators(executable));
    environment.insert(QStringLiteral("MERVIN_UPDATE_STAGING"), QDir::toNativeSeparators(workDir));
    environment.insert(QStringLiteral("MERVIN_UPDATE_SYSTEM"), QDir::toNativeSeparators(system.absolutePath()));
    QString profileArgument = QDir::toNativeSeparators(profile);
    // Windows argv parsing treats a backslash before a closing quote as an
    // escaped quote. A drive-root profile needs the trailing backslash doubled.
    if (profileArgument.endsWith(QLatin1Char('\\')))
        profileArgument += QLatin1Char('\\');
    environment.insert(QStringLiteral("MERVIN_UPDATE_PROFILE"), profileArgument);
    process.setProcessEnvironment(environment);
    process.setProgram(system.filePath(QStringLiteral("cmd.exe")));
    process.setWorkingDirectory(workDir);
    // Expand only these fixed variable references. Their values are not scanned
    // again for percent expansion, and /v:off preserves literal exclamation marks.
    // & relaunches after successful, failed or cancelled installation alike.
    QString command = QStringLiteral(
        "/d /v:off /s /c \"start \"\" /wait \"%MERVIN_UPDATE_INSTALLER%\" "
        "/i \"%MERVIN_UPDATE_PACKAGE%\" /passive /norestart "
        "& del /q \"%MERVIN_UPDATE_PACKAGE%\" "
        "& cd /d \"%MERVIN_UPDATE_SYSTEM%\" & rd \"%MERVIN_UPDATE_STAGING%\" "
        "& start \"\" \"%MERVIN_UPDATE_EXECUTABLE%\"");
    if (!profile.isEmpty())
        command += QStringLiteral(" --profile \"%MERVIN_UPDATE_PROFILE%\"");
    command += QLatin1Char('"');
    process.setNativeArguments(command);
    return true;
}
#endif

bool startWindowsInstaller(const QString &file)
{
#ifdef Q_OS_WIN
    if (PlatformIntegration::hasPackageIdentity())
        return false;
    QProcess cmd;
    if (!configureWindowsInstaller(cmd, file, QCoreApplication::applicationFilePath(),
                                   QDir(systemDir()).filePath(QStringLiteral("msiexec.exe"))))
        return false;
    if (!cmd.startDetached())
        return false; // startIfDue then starts a copy for the launches this one carries
    // The copy the installer starts afterwards opens the launches this one carries,
    // from the carry file this process writes before it exits.
    relaunch::nextCopyStarted();
    return true;
#else
    Q_UNUSED(file);
#endif
    return false;
}

// The messages share the Updater's context: it shows them, inside its own text.
bool replaceAppImage(const QString &file, const QString &sha256, QString *error)
{
    const QString target = qEnvironmentVariable("APPIMAGE");
    if (target.isEmpty()) {
        //: AppImage is a Linux package format; keep the name as it is.
        *error = QCoreApplication::translate("mervin::Updater",
                                             "This copy is not running as an AppImage.");
        return false;
    }
    // Stage beside the target so the final rename stays on one filesystem and
    // is atomic: a crash leaves either the old image or the new one, never half.
    QTemporaryFile staged(target + QStringLiteral(".update-XXXXXX"));
    QFile source(file);
    bool copied = staged.open() && source.open(QIODevice::ReadOnly);
    while (copied && !source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        copied = !bytes.isEmpty() && staged.write(bytes) == bytes.size();
    }
    copied = copied && source.error() == QFileDevice::NoError && staged.flush();
    if (!copied) {
        //: %1 is a folder path.
        *error = QCoreApplication::translate("mervin::Updater", "Couldn't write to %1.")
                     .arg(QFileInfo(target).absolutePath());
        return false;
    }
    // Check the destination bytes through the same open file before publishing.
    // A source pathname replaced during or after copying cannot change this file.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!isSha256(sha256) || !staged.seek(0) || !hash.addData(&staged)
        || QString::fromLatin1(hash.result().toHex()) != sha256.toLower()) {
        *error = QCoreApplication::translate("mervin::Updater",
            "The downloaded update failed its integrity check and was discarded.");
        return false;
    }
    if (!staged.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                              | QFileDevice::ExeGroup | QFileDevice::ReadOther
                              | QFileDevice::ExeOther)) {
        *error = QCoreApplication::translate("mervin::Updater", "Couldn't replace %1.").arg(target);
        return false;
    }
    // QFile::rename refuses to overwrite; POSIX rename() replaces in place.
    if (std::rename(QFile::encodeName(staged.fileName()).constData(), QFile::encodeName(target).constData())
        != 0) {
        //: %1 is the path of the running AppImage file.
        *error = QCoreApplication::translate("mervin::Updater", "Couldn't replace %1.").arg(target);
        return false;
    }
    staged.setAutoRemove(false);
    return true;
}

QStringList packageInstallCommand(PackageKind kind, const QString &file)
{
    const auto find = [](const char *name) {
        return QStandardPaths::findExecutable(QString::fromLatin1(name));
    };
    const QString pkexec = find("pkexec");
    if (pkexec.isEmpty())
        return {};
    // apt-get and dnf resolve any dependency the new version adds; the bare
    // dpkg / rpm fallbacks cannot, but still upgrade when nothing new is needed.
    if (kind == PackageKind::Deb) {
        if (const QString apt = find("apt-get"); !apt.isEmpty())
            return {pkexec, apt, QStringLiteral("install"), QStringLiteral("-y"), file};
        if (const QString dpkg = find("dpkg"); !dpkg.isEmpty())
            return {pkexec, dpkg, QStringLiteral("-i"), file};
    } else if (kind == PackageKind::Rpm) {
        if (const QString dnf = find("dnf"); !dnf.isEmpty())
            return {pkexec, dnf, QStringLiteral("install"), QStringLiteral("-y"), file};
        if (const QString rpm = find("rpm"); !rpm.isEmpty())
            return {pkexec, rpm, QStringLiteral("-U"), file};
    }
    return {};
}

bool relaunchAfterExit(const QString &exe, const QStringList &paths)
{
#ifdef Q_OS_LINUX
    QProcess sh;
    sh.setProgram(QStringLiteral("/bin/sh"));
    // The program, its arguments and any carried paths reach the script as positional
    // parameters. They must stay in the quoted "$@", so the shell never parses them.
    sh.setArguments(QStringList{QStringLiteral("-c"),
                                QStringLiteral("pid=$1; shift; while kill -0 \"$pid\" 2>/dev/null; do "
                                               "sleep 0.2; done; exec \"$@\""),
                                QStringLiteral("sh"),
                                QString::number(QCoreApplication::applicationPid()), exe}
                    + relaunchArguments() + paths);
    // Leaving an AppImage: drop what its runtime exported into the mount that
    // vanishes with this process. The new image's runtime sets its own.
    if (const QString appDir = qEnvironmentVariable("APPDIR"); !appDir.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &key : env.keys())
            if (env.value(key).contains(appDir))
                env.remove(key);
        sh.setProcessEnvironment(env);
    }
    if (!sh.startDetached())
        return false;
    relaunch::nextCopyStarted(); // it opens the launches this one carries
    return true;
#else
    Q_UNUSED(exe);
    Q_UNUSED(paths);
    return false;
#endif
}

} // namespace mervin::update
