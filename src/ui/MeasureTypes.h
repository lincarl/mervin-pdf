#pragma once

#include <QPointF>

#include <vector>

namespace mervin {

// The kind of measurement the tool is currently drawing.
enum class MeasureKind { Distance, Polyline, Area, Angle };

// A committed, ephemeral measurement. Points are in PAGE-POINT space (unrotated,
// 72 dpi), so they survive zoom/rotation and are transformed at paint time.
//   Distance : 2 points
//   Polyline : N points (open)
//   Area     : N points (auto-closed polygon)
//   Angle    : 3 points (p0, vertex, p2) - the vertex is the middle point
struct Measurement
{
    int page = -1;
    MeasureKind kind = MeasureKind::Distance;
    std::vector<QPointF> pts;

    // Optional user-pinned value-label position in PAGE-POINT space. When
    // hasLabelPos is false the label auto-anchors to the geometry; dragging the
    // label pins it here. Like pts, it survives zoom/rotation.
    QPointF labelPos;
    bool hasLabelPos = false;
};

} // namespace mervin
