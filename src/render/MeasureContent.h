#pragma once

// Shared, GUI-free measurement <-> PDF glue, used by the save / export / print
// flows. Three responsibilities, none of which touch MuPDF or qpdf:
//   1. (De)serialize a measurement set to/from the Mervin-only JSON blob that is
//      embedded in a PDF's catalog (so reopening in Mervin restores the marks).
//   2. Format a measurement's value string (the same logic the on-screen panel
//      uses, factored out here so burned-in / annotated labels match exactly).
//   3. Emit a PDF content-stream operator string that draws one measurement in
//      PDF user space - reused verbatim for flatten-export, the print temp PDF,
//      and annotation /AP appearance streams.
//
// Lives in mervin_core so the security-layer writer (MeasureExport) and the unit
// tests can link it without the widgets executable.

#include "render/MeasureModel.h"
#include "render/MeasureTypes.h"
#include "ui/MeasureTypes.h"

#include <QByteArray>
#include <QPointF>
#include <QString>

#include <array>
#include <string>
#include <vector>

namespace mervin {

// 2x3 affine transform {a,b,c,d,e,f}, MuPDF's fz_matrix convention:
//   x' = a*x + c*y + e,  y' = b*x + d*y + f.
struct Mat6
{
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
};

inline QPointF applyMat(const Mat6 &m, QPointF p)
{
    return QPointF(m.a * p.x() + m.c * p.y() + m.e, m.b * p.x() + m.d * p.y() + m.f);
}

// One persisted per-page scale override (Manual / Calibrated only - None and
// Embedded scales are re-derived from the PDF on open and never stored).
struct PageScale
{
    int page = -1;
    double mmPerPointX = 0.0;
    double mmPerPointY = 0.0;
    QString label;
    MeasureSource source = MeasureSource::Manual;
};

// The Mervin-only persisted measurement set for one document.
struct MeasureDoc
{
    int version = 1;
    MeasureUnit unit = MeasureUnit::Millimeter;
    int precision = 2;
    double lineWidth = 2.0;                 // measurement stroke width (points)
    std::vector<Measurement> measurements; // page-point space
    std::vector<PageScale> pageScales;

    // Rebuild a MeasureModel (per-page overrides) from pageScales.
    MeasureModel overridesModel() const;
};

QByteArray serializeMeasurements(const MeasureDoc &doc);
bool parseMeasurements(const QByteArray &json, MeasureDoc *out);

// A measurement paired with everything the PDF emitter needs: its page-point
// geometry, the page transform that lands it in PDF user space, and the
// pre-formatted value label. Built by the UI layer (which has the live Document)
// and handed to MeasureExport (which must not touch MuPDF).
struct RenderMeasurement
{
    int page = -1;
    MeasureKind kind = MeasureKind::Distance;
    std::vector<QPointF> pts; // page-point space
    bool hasLabelPos = false;
    QPointF labelPos;     // page-point space
    QString label;        // formatted value (e.g. "1234.50 mm (1.23 m)")
    Mat6 toPdf;           // page-point -> PDF user space
    double lineWidth = 0.0; // stroke width (points); 0 = use the EmitStyle default
};

// Style for the emitted marks. These are PDF content-stream operands (RG/rg),
// burned into the exported file, so they are export colours in their own right and
// deliberately NOT the UI accent token: a theme change must never alter what an
// already-flattened drawing looks like, and export output has to stay reproducible
// across releases. They happen to start out near the default accent.
struct EmitStyle
{
    double strokeR = 0.0, strokeG = 0.40, strokeB = 0.75; // a blue close to #0067C0
    double fillR = 0.85, fillG = 0.92, fillB = 0.98;      // light area fill
    double lineWidth = 1.6;
    // Burned-in label text size (PDF points). toPdf carries no scale, so this is
    // the literal point size in the output. Kept small to sit alongside a
    // drawing's native dimension text rather than dwarf it.
    double fontSize = 6.0;
};

// The Helvetica resource name the emitter references for label text. The PDF
// writer must add a font under exactly this name to the page (flatten) or the
// appearance-stream XObject (annotate) /Resources.
inline constexpr char kMeasureFontResource[] = "Fluc";

// Emit PDF content-stream operators (a self-contained q...Q block) that draw one
// measurement in absolute PDF user space, using rm.toPdf to place geometry.
std::string emitMeasurementOps(const RenderMeasurement &rm, const EmitStyle &style = {});

// Format a measurement's value string. Identical to what the on-screen panel
// shows (the viewer delegates here), so burned-in / annotated labels match.
QString formatMeasurementValue(MeasureKind kind, const std::vector<QPointF> &pts,
                               const MeasureScale &scale, MeasureUnit unit, int precision);

} // namespace mervin
