#include "update/Installer.h"

#include "config/ConfigPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

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
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Mervin PDF"),
                        QSettings::NativeFormat);
    return windowsPackageKind(exeDir, reg.value(QStringLiteral("InstallDir")).toString(),
                              QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("uninstall.exe"))));
#elif defined(Q_OS_LINUX)
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    return linuxPackageKind(QCoreApplication::applicationFilePath(),
                            QFileInfo(appImage).isFile() ? appImage : QString(),
                            qEnvironmentVariable("APPDIR"), QStringLiteral(MERVIN_PACKAGE_FORMAT));
#else
    return PackageKind::None;
#endif
}

bool startWindowsInstaller(PackageKind kind, const QString &file)
{
#ifdef Q_OS_WIN
    const QString installer = QDir::toNativeSeparators(file);
    const QString workDir = QFileInfo(file).absolutePath(); // not wherever Mervin was started
    if (kind == PackageKind::NsisSetup)
        return QProcess::startDetached(installer, {QStringLiteral("/S")}, workDir);
    if (kind == PackageKind::Msi) {
        // cmd's /s strips just the outer quotes, leaving every quoted path intact.
        // "&" (not "&&") relaunches even after a failed or cancelled install, so
        // the user always gets a running Mervin back.
        QString relaunch = QStringLiteral("start \"\" \"%1\"")
                               .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
        const QStringList args = relaunchArguments();
        if (!args.isEmpty())
            relaunch += QStringLiteral(" %1 \"%2\"").arg(args.at(0), QDir::toNativeSeparators(args.at(1)));
        const QDir system(systemDir());
        QProcess cmd;
        cmd.setProgram(system.filePath(QStringLiteral("cmd.exe")));
        cmd.setWorkingDirectory(workDir);
        cmd.setNativeArguments(
            QStringLiteral("/d /s /c \"start \"\" /wait \"%1\" /i \"%2\" /passive /norestart & %3\"")
                .arg(QDir::toNativeSeparators(system.filePath(QStringLiteral("msiexec.exe"))),
                     installer, relaunch));
        return cmd.startDetached();
    }
#else
    Q_UNUSED(kind);
    Q_UNUSED(file);
#endif
    return false;
}

bool replaceAppImage(const QString &file, QString *error)
{
    const QString target = qEnvironmentVariable("APPIMAGE");
    if (target.isEmpty()) {
        *error = QCoreApplication::translate("Updater", "This copy is not running as an AppImage.");
        return false;
    }
    // Stage beside the target so the final rename stays on one filesystem and
    // is atomic: a crash leaves either the old image or the new one, never half.
    const QString staged = target + QStringLiteral(".update");
    QFile::remove(staged);
    if (!QFile::copy(file, staged)) {
        *error = QCoreApplication::translate("Updater", "Couldn't write to %1.")
                     .arg(QFileInfo(target).absolutePath());
        return false;
    }
    QFile::setPermissions(staged, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                      | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                      | QFileDevice::ExeOther);
    // QFile::rename refuses to overwrite; POSIX rename() replaces in place.
    if (std::rename(QFile::encodeName(staged).constData(), QFile::encodeName(target).constData())
        != 0) {
        QFile::remove(staged);
        *error = QCoreApplication::translate("Updater", "Couldn't replace %1.").arg(target);
        return false;
    }
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

bool relaunchAfterExit(const QString &exe)
{
#ifdef Q_OS_LINUX
    QProcess sh;
    sh.setProgram(QStringLiteral("/bin/sh"));
    sh.setArguments(QStringList{QStringLiteral("-c"),
                                QStringLiteral("pid=$1; shift; while kill -0 \"$pid\" 2>/dev/null; do "
                                               "sleep 0.2; done; exec \"$@\""),
                                QStringLiteral("sh"),
                                QString::number(QCoreApplication::applicationPid()), exe}
                    + relaunchArguments());
    // Leaving an AppImage: drop what its runtime exported into the mount that
    // vanishes with this process. The new image's runtime sets its own.
    if (const QString appDir = qEnvironmentVariable("APPDIR"); !appDir.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &key : env.keys())
            if (env.value(key).contains(appDir))
                env.remove(key);
        sh.setProcessEnvironment(env);
    }
    return sh.startDetached();
#else
    Q_UNUSED(exe);
    return false;
#endif
}

} // namespace mervin::update
