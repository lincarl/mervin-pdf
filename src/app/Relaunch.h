#pragma once

#include <QString>

// Restarting Mervin from inside, for a change that only takes effect at startup
// (the UI language). The running copy closes its windows as Quit does, and
// main() starts the new copy after runUi() has returned, when this process no
// longer holds the single-instance pipe. Otherwise the new copy would hand its
// launch to this one while it shuts down.
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

} // namespace mervin::relaunch
