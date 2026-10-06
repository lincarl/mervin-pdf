#pragma once

#include <QPointF>

#include <vector>

namespace mervin {

// The kind of measurement the tool is currently drawing.
enum class MeasureKind { Distance, Polyline, Area, Angle };

// Measurement geometry in unrotated 72-dpi page points. Distance uses two points; Polyline is
// open; Area closes automatically; Angle uses (start, vertex, end). Persistence is handled by
// MeasureDoc.
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
