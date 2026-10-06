#pragma once

#include <QString>

namespace mervin {

// Persisted per-file view state, used to resume a document where the user left
// off (last page + zoom + rotation). Plain value type shared across the host
// store, the IPC layer, and the viewer; intentionally free of any logic.
struct ViewState
{
    int page = 0;                                   // 0-based page index
    QString zoomMode = QStringLiteral("fit-width"); // "fit-width" | "fit-page" | "custom"
    double scale = 1.0;                             // used only when zoomMode == "custom"
    int rotation = 0;                               // 0 / 90 / 180 / 270 degrees
    // Viewport top-left within page as a fraction of displayed page size, independent of scale.
    // Zero fractions place the page corner at the viewport corner.
    double offsetX = 0.0;
    double offsetY = 0.0;
};

} // namespace mervin
