#pragma once

#include <QString>

namespace mervin::ipc {

// The per-user QLocalServer name used for the single-instance pipe. Derived
// from a hash of the current user so two users on one machine get distinct
// pipes, while avoiding any charset/length/unicode issues that raw usernames
// would bring into a Windows named-pipe name. Stable across launches for the
// same user. When a --profile state directory is active (ConfigPaths
// override), a hash of that directory is appended, so profile instances are
// isolated from the default instance and from other profiles.
QString hostPipeName();

} // namespace mervin::ipc
