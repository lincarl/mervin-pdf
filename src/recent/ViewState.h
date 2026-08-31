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
    // Scroll position within `page`, as a fraction of the page's displayed size:
    // the canvas point at the viewport's top-left, minus the page's top-left,
    // over the page size. Scale-independent (numerator and denominator both scale
    // with zoom), so the exact spot is restored at any zoom level / window size.
    // 0,0 means "page top-left at the viewport corner" - the legacy page-only
    // behaviour, and the default for entries written before this field existed.
    double offsetX = 0.0;
    double offsetY = 0.0;
};

} // namespace mervin
