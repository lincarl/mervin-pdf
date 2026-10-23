#pragma once

#include "update/UpdatePlan.h"

#include <QString>
#include <QStringList>

class QProcess;

// Platform steps of an update: recognising how this copy was installed and
// putting a downloaded release in its place. No UI here; the Updater decides
// when to run each step and reports failures.
namespace mervin::update {

// How this running copy was installed (see PackageKind). A few file and
// registry reads, no processes.
PackageKind installedPackageKind();

// Launch the MSI detached; the caller must then quit to release binaries. Show
// installation progress and relaunch on any outcome, preserving --profile. The copy it
// starts opens the launches this one carries (relaunch::carry). Return false if launch
// fails. `file` must be the sole package in an updater-owned private staging
// directory. The helper removes that file and directory after MSI exits.
bool startWindowsInstaller(const QString &file);

#ifdef Q_OS_WIN
// Configure the detached helper without starting it. Supplying the programs
// separately lets native process tests exercise quoting and cleanup without MSI.
// Reject paths containing characters that Windows filenames cannot contain but
// cmd would interpret as syntax. The profile comes from relaunchArguments().
bool configureWindowsInstaller(QProcess &process, const QString &file,
                               const QString &executable, const QString &msiexec);
#endif

// AppImage: copies `file` beside the running image ($APPIMAGE), verifies that
// staging file against `sha256`, then atomically replaces the image. The
// running process keeps the image it already mounted, so this is safe mid-run.
// On failure returns false and describes why in `error`.
bool replaceAppImage(const QString &file, const QString &sha256, QString *error);

// deb / rpm: the command (program first) that installs `file` as root through
// pkexec and the distribution's package manager, or empty when pkexec or a
// package manager is missing.
QStringList packageInstallCommand(PackageKind kind, const QString &file);

// Arguments that bring a relaunched copy back as this one: a --profile instance
// (dev and test runs) returns to its profile rather than the user's real state.
QStringList relaunchArguments();

// Linux: starts `exe` (keeping --profile) with `paths` as soon as this process has
// exited, so the new copy becomes the single-instance primary instead of handing its
// launch to this one while it shuts down. It also opens the launches this copy carries
// (relaunch::carry). False if the helper shell could not be started.
bool relaunchAfterExit(const QString &exe, const QStringList &paths = {});

} // namespace mervin::update
