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

// The program that starts a copy of this one: the AppImage file when running
// from one (the binary inside its mount disappears with this process),
// otherwise this executable.
QString program(const QString &appImage, const QString &applicationFile);

// Starts the new copy with this run's --profile. Linux waits for this process
// to exit first; Windows starts it at once, so call this after the instance
// server is gone. False if it could not be started.
bool start();

} // namespace mervin::relaunch
