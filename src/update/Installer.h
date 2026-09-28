#pragma once

#include "update/UpdatePlan.h"

#include <QString>
#include <QStringList>

// Platform steps of an update: recognising how this copy was installed and
// putting a downloaded release in its place. No UI here; the Updater decides
// when to run each step and reports failures.
namespace mervin::update {

// How this running copy was installed (see PackageKind). A few file and
// registry reads, no processes.
PackageKind installedPackageKind();

// Windows: starts the downloaded installer detached so it outlives this
// process, which must quit straight after (the installer replaces files it
// holds open). NSIS runs silently and relaunches Mervin itself (mervin.nsi); the
// MSI shows only a progress bar, and a cmd.exe wrapper starts Mervin again once
// msiexec returns, whatever the outcome, keeping --profile so a test instance
// comes back in its profile. False if nothing could be started.
bool startWindowsInstaller(PackageKind kind, const QString &file);

// AppImage: moves a copy of `file` over the running image ($APPIMAGE). The
// running process keeps the image it already mounted, so this is safe mid-run.
// On failure returns false and describes why in `error`.
bool replaceAppImage(const QString &file, QString *error);

// deb / rpm: the command (program first) that installs `file` as root through
// pkexec and the distribution's package manager, or empty when pkexec or a
// package manager is missing.
QStringList packageInstallCommand(PackageKind kind, const QString &file);

// Linux: starts `exe` (keeping --profile) as soon as this process has exited,
// so the new copy becomes the single-instance primary instead of handing its
// launch to this one while it shuts down. False if the helper shell could not
// be started.
bool relaunchAfterExit(const QString &exe);

} // namespace mervin::update
