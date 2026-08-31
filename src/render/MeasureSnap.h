#pragma once

// Pure, header-only snapping math for the measuring tool: given a page's
// vector geometry (see GeometryTypes.h) and a cursor position in page-point
// space, find the nearest snap target within a radius. Endpoints (vertices)
// win over edges within the radius - the CAD-conventional "endpoint first"
// behaviour - so a precise corner is preferred even when a passing edge is
// marginally closer. No Qt widgets, no MuPDF: trivially unit-testable.

#include "render/GeometryTypes.h"

#include <QPointF>

#include <cstddef>

namespace mervin::snap {

enum class SnapType { None, Vertex, Edge };

struct SnapResult
{
    SnapType type = SnapType::None;
    QPointF point;     // snapped page point (valid only when type != None)
    double dist2 = 0.0; // squared distance from the query to `point`

    bool snapped() const { return type != SnapType::None; }
};

// Closest point to `p` on the segment [a, b], clamped to the segment ends.
inline QPointF closestOnSegment(QPointF a, QPointF b, QPointF p)
{
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0.0)
        return a; // degenerate segment
    double t = ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return QPointF(a.x() + t * dx, a.y() + t * dy);
}

// Snap `p` to the nearest vertex (preferred) or edge in `g` within `radius`
// (page-point units). Returns {None} when nothing is in range. A brute-force
// scan: bounded by the geometry's segment cap, it stays well under a
// millisecond for realistic pages.
inline SnapResult snap(const PageGeometry &g, QPointF p, double radius,
                       bool useVertices = true, bool useEdges = true)
{
    SnapResult best;
    if (radius <= 0.0)
        return best;
    const double r2 = radius * radius;

    if (useVertices) {
        double bestD = r2;
        int bestI = -1;
        for (std::size_t i = 0; i < g.vertices.size(); ++i) {
            const QPointF &v = g.vertices[i];
            const double dx = v.x() - p.x();
            const double dy = v.y() - p.y();
            const double d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                bestI = static_cast<int>(i);
            }
        }
        if (bestI >= 0) {
            best.type = SnapType::Vertex;
            best.point = g.vertices[bestI];
            best.dist2 = bestD;
            return best; // endpoint wins outright when within the radius
        }
    }

    if (useEdges) {
        double bestD = r2;
        QPointF bestP;
        for (const auto &s : g.segments) {
            if (s.first < 0 || s.second < 0
                || s.first >= static_cast<int>(g.vertices.size())
                || s.second >= static_cast<int>(g.vertices.size()))
                continue;
            const QPointF c = closestOnSegment(g.vertices[s.first], g.vertices[s.second], p);
            const double dx = c.x() - p.x();
            const double dy = c.y() - p.y();
            const double d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                bestP = c;
            }
        }
        if (bestD < r2) {
            best.type = SnapType::Edge;
            best.point = bestP;
            best.dist2 = bestD;
        }
    }

    return best;
}

} // namespace mervin::snap
