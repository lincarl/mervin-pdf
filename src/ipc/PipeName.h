#pragma once

#include <QString>

namespace mervin::ipc {

// Stable per-user pipe name hashed to avoid Windows charset/length issues. An active --profile
// appends a directory hash to isolate its instance group.
QString hostPipeName();

} // namespace mervin::ipc
