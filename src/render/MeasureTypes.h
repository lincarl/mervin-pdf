#pragma once

#include <QRectF>
#include <QString>

#include <vector>

namespace mervin {

// Real-world display unit for measurements.
enum class MeasureUnit { Millimeter, Centimeter, Meter, Inch, Foot };

// One /VP viewport with its rectilinear /Measure dictionary, as read from the
// PDF. cx/cy are the raw /X[0]/C and /Y[0]/C conversion factors (real units per
// PDF point); unit is the /U label (empty when the PDF leaves it blank, as
// AutoCAD does); ratio is the /R string (often empty). bbox is in page-point
// space (default user space, unrotated).
struct MeasureViewport
{
    QRectF bbox;
    double cx = 0.0;
    double cy = 0.0;
    QString unit;
    QString ratio;
    bool rectilinear = true;
};

// All embedded measurement metadata for one page (hasEmbedded is false when the
// PDF carries none). Viewports are in /VP array order; later entries take
// precedence where BBoxes overlap.
struct PageMeasurement
{
    bool hasEmbedded = false;
    std::vector<MeasureViewport> viewports;
    double userUnit = 1.0;
};

enum class MeasureSource { None, Embedded, Calibrated, Manual };

// A resolved, ready-to-use scale for one point on a page. Internally normalized
// to MILLIMETRES per PDF point so all geometry math is unit-free; the display
// unit is applied only at format time. label is a human ratio ("1:100").
struct MeasureScale
{
    double mmPerPointX = 0.0;
    double mmPerPointY = 0.0;
    QString label;
    MeasureSource source = MeasureSource::None;

    bool valid() const { return source != MeasureSource::None && mmPerPointX > 0.0; }
};

} // namespace mervin
