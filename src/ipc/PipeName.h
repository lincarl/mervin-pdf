#pragma once

#include <QString>

namespace mervin::ipc {

// Stable per-user pipe name hashed to avoid Windows charset/length issues.
// Windows package identity and --profile isolate their own instance groups.
QString hostPipeName();

} // namespace mervin::ipc
