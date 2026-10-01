#pragma once

#include <QPointF>

#include <utility>
#include <vector>

namespace mervin {

// Flattened page-content segments for CAD snapping. Coordinates are unrotated page points at 72
// dpi with a zero origin. Segments reference deduplicated vertices. truncated marks a hard
// segment-cap hit; partial geometry remains usable.
struct PageGeometry
{
    std::vector<QPointF> vertices;            // deduped endpoints, page-point space
    std::vector<std::pair<int, int>> segments; // index pairs into `vertices`
    bool truncated = false;

    bool empty() const { return segments.empty() && vertices.empty(); }
};

} // namespace mervin
