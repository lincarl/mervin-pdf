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

// Launch the MSI detached; the caller must then quit to release binaries. Show
// installation progress and relaunch on any outcome, preserving --profile.
// Return false if launch fails.
bool startWindowsInstaller(const QString &file);

// AppImage: moves a copy of `file` over the running image ($APPIMAGE). The
// running process keeps the image it already mounted, so this is safe mid-run.
// On failure returns false and describes why in `error`.
bool replaceAppImage(const QString &file, QString *error);

// deb / rpm: the command (program first) that installs `file` as root through
// pkexec and the distribution's package manager, or empty when pkexec or a
// package manager is missing.
QStringList packageInstallCommand(PackageKind kind, const QString &file);

// Arguments that bring a relaunched copy back as this one: a --profile instance
// (dev and test runs) returns to its profile rather than the user's real state.
QStringList relaunchArguments();

// Linux: starts `exe` (keeping --profile) as soon as this process has exited,
// so the new copy becomes the single-instance primary instead of handing its
// launch to this one while it shuts down. False if the helper shell could not
// be started.
bool relaunchAfterExit(const QString &exe);

} // namespace mervin::update
