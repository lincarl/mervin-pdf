#pragma once

#include "ipc/Message.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Restarting Mervin from inside, for a change that only takes effect at startup
// (the UI language). The running copy closes its windows as Quit does, and
// main() starts the new copy after runUi() has returned, when this process no
// longer holds the single-instance lock, so the new copy becomes the primary at
// once instead of waiting for this one to exit.
namespace mervin::relaunch {

// Ask main() to start a new copy once this one has shut down.
void request();
bool requested();

// The program that starts a copy of this one: the AppImage file when this
// executable runs from that image's mount, `appDir` (the binary inside the mount
// disappears with this process), otherwise this executable. A program started
// from inside any AppImage inherits APPIMAGE and APPDIR, so the variables alone
// don't mean this copy is an AppImage.
QString program(const QString &appImage, const QString &appDir, const QString &applicationFile);

// Starts the new copy with this run's --profile, and `paths` for it to open. They are
// absolute, so the copy cannot read one as an option. Each is a separate argument of the
// process, which goes through no shell or cmd. Linux waits for this process to exit
// first; Windows starts it at once, so call this after the instance server is gone.
// False if it could not be started.
bool start(const QStringList &paths = {});

// Adds a launch that this copy acknowledged but will not open, because it quits first.
// WindowManager writes the launches carried to the carry file (writeCarried) before it
// lets go of the single-instance lock, and the copy that next becomes the primary opens
// them (takeCarried). If that write fails, startIfDue gives their files to the copy it
// starts. Even a launch without files asks for a next copy, as it asks for a window.
void carry(const ipc::Message &open);

// Whether a copy should start to open the carried launches.
bool carrying();

// Records that whatever started the next copy, as an update does, so startIfDue need not
// start one for the carried launches.
void nextCopyStarted();

// Writes the carried launches to `file`, after those a previous primary left there, as
// UTF-8 JSON readable only by this user, and forgets them once the file is complete.
// Call it while this process holds the lock that guards `file`. False if they could not
// be written. They then stay for startIfDue.
bool writeCarried(const QString &file);

// How old a carry file may be when the next primary finds it. Long enough for an
// installer to run between the two copies, short enough that a file left by a copy that
// crashed does not open files days later.
constexpr qint64 kCarriedMaxAgeMs = 10 * 60 * 1000;

// How far ahead of this copy's clock a carry file may be dated, for a clock that was set
// back a little between the two copies.
constexpr qint64 kCarriedMaxAheadMs = 60 * 1000;

// Reads and removes the launches a previous primary left in `file`. Call it while
// holding the lock that guards `file`. Empty if there are none, if the file cannot be
// read, or if it is older than kCarriedMaxAgeMs or dated more than kCarriedMaxAheadMs
// ahead.
QList<ipc::Message> takeCarried(const QString &file);

// Starts the next copy if one is due. It is due if a restart was asked for and `restart`
// allows it, or if launches are carried and no copy was started for them yet. Files of
// carried launches that no carry file holds go to that copy as arguments (see start).
// If an update started the next copy instead, those files cannot reach it and are
// logged. False if a copy was due and did not start.
bool startIfDue(bool restart);

// For tests only. Replaces start() in startIfDue with `starter`, which gets the paths
// start() would get, or restores start() when `starter` is empty.
void setStarterForTests(std::function<bool(const QStringList &paths)> starter);

} // namespace mervin::relaunch
