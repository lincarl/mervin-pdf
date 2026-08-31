#pragma once

#include <QPointF>

#include <utility>
#include <vector>

namespace mervin {

// Flattened vector geometry harvested from a page's content stream (straight
// line segments and their endpoints), used by the measuring tool to snap the
// cursor to precise vertices/edges on CAD line drawings.
//
// Points are in PAGE-POINT space (unrotated, 72 dpi, (0,0)-based - the same
// space as TextIndex rects and ViewerWidget::canvasToPagePoint), so they survive
// zoom/rotation and map cleanly to/from the widget. Curves are pre-flattened to
// short segments; segments index into `vertices` (deduplicated) so shared
// endpoints are stored once. `truncated` is set when a hard segment cap was hit
// on a pathological page (the data is still usable, just incomplete).
struct PageGeometry
{
    std::vector<QPointF> vertices;            // deduped endpoints, page-point space
    std::vector<std::pair<int, int>> segments; // index pairs into `vertices`
    bool truncated = false;

    bool empty() const { return segments.empty() && vertices.empty(); }
};

} // namespace mervin
