#pragma once

#include <QString>

namespace mervin {

// Locates Mervin PDF's per-user config/data directory (Windows:
// %APPDATA%/MervinPDF; Linux: $XDG_CONFIG_HOME/mervin-pdf, i.e. ~/.config/mervin-pdf).
namespace ConfigPaths {

// Directory holding the config file and tessdata/ (created if missing).
QString configDir();

// Full path to config.toml.
QString configFile();

// Redirect configDir() - and everything built on it: config.toml, recent.json,
// viewstate.json, session.json, tessdata/ - to `dir`. Backs the --profile
// command-line argument, so a dev/test instance keeps its state separate from
// the installed app's. Must be called before anything resolves configDir()
// (i.e. right after argument parsing in main). Empty resets to the default.
void setOverrideDir(const QString &dir);

// The active override directory (absolute), or empty when using the default.
QString overrideDir();

} // namespace ConfigPaths

} // namespace mervin
