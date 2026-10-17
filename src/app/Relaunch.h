#pragma once

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

// Starts the new copy with this run's --profile. Linux waits for this process
// to exit first; Windows starts it at once, so call this after the instance
// server is gone. False if it could not be started.
bool start();

// Adds the files of a launch that this copy acknowledged but will not open, because it
// quits first. The next copy opens them (see startIfDue and passCarried). Even a launch
// without files asks for a next copy, as it asks for a window.
void carry(const QStringList &paths);
bool carrying();

// Hands the carried files to every process this one starts from now on, through the
// environment, and forgets them. Whatever starts the next copy calls this first.
void passCarried();

// In the next copy, the files the previous copy passed on. Removes them from the
// environment, so they reach no process this copy starts.
QStringList takePassed();

// Starts the next copy if one is due. It is due if a restart was asked for and `restart`
// allows it, or if launches are carried. False if a copy was due and did not start.
bool startIfDue(bool restart);

// For tests only. Replaces start() in startIfDue with `starter`, or restores it when
// `starter` is empty.
void setStarterForTests(std::function<bool()> starter);

} // namespace mervin::relaunch
