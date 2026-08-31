#pragma once

// Pure, header-only measurement math: unit conversion, geometry in page-point
// space, value formatting, and the scale-resolution logic that turns a page's
// embedded /VP measurement (plus any manual/calibrated override) into a usable
// mm-per-point scale at a given point. No MuPDF, no Qt widgets - safe to call
// from any thread and trivially unit-testable.

#include "render/MeasureTypes.h"

#include <QPointF>
#include <QString>

#include <cmath>
#include <vector>

namespace mervin::measure {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kMmPerPoint = 25.4 / 72.0; // 0.3527777... mm in one PDF point

inline double mmPerUnit(MeasureUnit u)
{
    switch (u) {
    case MeasureUnit::Millimeter: return 1.0;
    case MeasureUnit::Centimeter: return 10.0;
    case MeasureUnit::Meter:      return 1000.0;
    case MeasureUnit::Inch:       return 25.4;
    case MeasureUnit::Foot:       return 304.8;
    }
    return 1.0;
}

inline QString unitSuffix(MeasureUnit u)
{
    switch (u) {
    case MeasureUnit::Millimeter: return QStringLiteral("mm");
    case MeasureUnit::Centimeter: return QStringLiteral("cm");
    case MeasureUnit::Meter:      return QStringLiteral("m");
    case MeasureUnit::Inch:       return QStringLiteral("in");
    case MeasureUnit::Foot:       return QStringLiteral("ft");
    }
    return QString();
}

inline MeasureUnit unitFromString(const QString &s, MeasureUnit fallback = MeasureUnit::Millimeter)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("mm")) return MeasureUnit::Millimeter;
    if (t == QLatin1String("cm")) return MeasureUnit::Centimeter;
    if (t == QLatin1String("m"))  return MeasureUnit::Meter;
    if (t == QLatin1String("in") || t == QLatin1String("inch")) return MeasureUnit::Inch;
    if (t == QLatin1String("ft") || t == QLatin1String("foot") || t == QLatin1String("feet"))
        return MeasureUnit::Foot;
    return fallback;
}

// --- geometry in page-point space -> millimetres ---------------------------

inline double segmentMm(QPointF a, QPointF b, const MeasureScale &s)
{
    const double dx = (b.x() - a.x()) * s.mmPerPointX;
    const double dy = (b.y() - a.y()) * s.mmPerPointY;
    return std::hypot(dx, dy);
}

inline double polylineMm(const std::vector<QPointF> &pts, const MeasureScale &s)
{
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i)
        total += segmentMm(pts[i - 1], pts[i], s);
    return total;
}

// Perimeter of the closed polygon (adds the closing edge back to the first pt).
inline double perimeterMm(const std::vector<QPointF> &pts, const MeasureScale &s)
{
    double total = polylineMm(pts, s);
    if (pts.size() >= 3)
        total += segmentMm(pts.back(), pts.front(), s);
    return total;
}

// Polygon area in mm^2 (shoelace formula in mm-scaled coordinates).
inline double areaMm2(const std::vector<QPointF> &pts, const MeasureScale &s)
{
    if (pts.size() < 3)
        return 0.0;
    double acc = 0.0;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const QPointF &p = pts[i];
        const QPointF &q = pts[(i + 1) % n];
        acc += (p.x() * s.mmPerPointX) * (q.y() * s.mmPerPointY)
             - (q.x() * s.mmPerPointX) * (p.y() * s.mmPerPointY);
    }
    return std::abs(acc) * 0.5;
}

// Interior angle in degrees at `vertex`, between the rays to a and b. Scaling
// each component makes the result the real-world angle when mmX != mmY.
inline double angleDeg(QPointF vertex, QPointF a, QPointF b, const MeasureScale &s)
{
    const double ax = (a.x() - vertex.x()) * s.mmPerPointX;
    const double ay = (a.y() - vertex.y()) * s.mmPerPointY;
    const double bx = (b.x() - vertex.x()) * s.mmPerPointX;
    const double by = (b.y() - vertex.y()) * s.mmPerPointY;
    double deg = std::abs(std::atan2(ay, ax) - std::atan2(by, bx)) * 180.0 / kPi;
    if (deg > 180.0)
        deg = 360.0 - deg;
    return deg;
}

// --- formatting (C locale, so '.' is always the decimal separator) ----------

inline QString formatLengthMm(double mm, MeasureUnit display, int prec)
{
    const double v = mm / mmPerUnit(display);
    return QString::number(v, 'f', prec) + QLatin1Char(' ') + unitSuffix(display);
}

inline QString formatAreaMm2(double mm2, MeasureUnit display, int prec)
{
    const double scale = mmPerUnit(display);
    const double v = mm2 / (scale * scale);
    return QString::number(v, 'f', prec) + QLatin1Char(' ') + unitSuffix(display)
           + QString::fromUtf8("\xC2\xB2"); // superscript two
}

inline QString formatAngle(double deg, int prec)
{
    return QString::number(deg, 'f', prec) + QString::fromUtf8("\xC2\xB0"); // degree sign
}

// --- auto SI-unit reduction -------------------------------------------------
//
// A value shown in the user's chosen unit is hard to read once its integer part
// has more than three digits (e.g. "7128.93 mm"). These helpers append the same
// value re-expressed in the next-larger unit that brings the integer part back
// under four digits, in brackets - e.g. "7128.93 mm (712.89 cm)". They step up a
// unit ladder (metric: mm→cm→m→km; imperial: in→ft→mi), squaring the factor for
// areas. They return the bare single-unit string when no reduction is needed
// (value already < 1000) or no larger unit exists.

namespace detail {

// (millimetres per one unit, suffix) ascending in size. Kept independent of the
// MeasureUnit enum so the ladder can reach km/mi, which the unit picker does not
// expose but which keep huge values readable.
struct LadderUnit
{
    double mmPer;
    const char *suffix;
};

inline const std::vector<LadderUnit> &metricLadder()
{
    static const std::vector<LadderUnit> l = {{1.0, "mm"}, {10.0, "cm"}, {1000.0, "m"}, {1.0e6, "km"}};
    return l;
}

inline const std::vector<LadderUnit> &imperialLadder()
{
    static const std::vector<LadderUnit> l = {{25.4, "in"}, {304.8, "ft"}, {304.8 * 5280.0, "mi"}};
    return l;
}

inline bool isImperial(MeasureUnit u) { return u == MeasureUnit::Inch || u == MeasureUnit::Foot; }

// True when |v|, rounded to `prec` decimals (i.e. as displayed), has four or more
// integer digits.
inline bool exceedsThreeDigits(double v, int prec)
{
    const double pow10 = std::pow(10.0, prec);
    const double rounded = std::round(std::abs(v) * pow10) / pow10;
    return rounded >= 1000.0;
}

// Shared core: given the value in mm (or mm² when `area`), the display unit and
// precision, build "<primary> (<converted>)" or just "<primary>". `square`
// applies the unit factor twice and appends the ² suffix.
inline QString formatAutoReduced(double mmValue, MeasureUnit display, int prec, bool square)
{
    const QString sq = square ? QString::fromUtf8("\xC2\xB2") : QString();
    const double displayMmPer = mmPerUnit(display);
    const double displayFactor = square ? displayMmPer * displayMmPer : displayMmPer;
    const double primaryVal = mmValue / displayFactor;
    const QString primary = QString::number(primaryVal, 'f', prec) + QLatin1Char(' ')
                            + unitSuffix(display) + sq;
    if (!exceedsThreeDigits(primaryVal, prec))
        return primary;

    const std::vector<LadderUnit> &ladder = isImperial(display) ? imperialLadder() : metricLadder();
    const LadderUnit *chosen = nullptr; // smallest larger unit that fits, else the largest
    for (const LadderUnit &u : ladder) {
        if (u.mmPer <= displayMmPer)
            continue; // must step UP to reduce the digit count
        chosen = &u;
        const double factor = square ? u.mmPer * u.mmPer : u.mmPer;
        if (!exceedsThreeDigits(mmValue / factor, prec))
            break; // first larger unit that drops under four integer digits wins
    }
    if (!chosen)
        return primary; // already the largest unit on the ladder

    const double factor = square ? chosen->mmPer * chosen->mmPer : chosen->mmPer;
    const QString conv = QString::number(mmValue / factor, 'f', prec) + QLatin1Char(' ')
                         + QString::fromLatin1(chosen->suffix) + sq;
    return primary + QStringLiteral(" (") + conv + QLatin1Char(')');
}

} // namespace detail

// Length, with the bracketed next-larger unit when the primary exceeds 3 digits.
inline QString formatLengthMmAuto(double mm, MeasureUnit display, int prec)
{
    return detail::formatAutoReduced(mm, display, prec, /*square=*/false);
}

// Area, with the bracketed next-larger unit when the primary exceeds 3 digits.
inline QString formatAreaMm2Auto(double mm2, MeasureUnit display, int prec)
{
    return detail::formatAutoReduced(mm2, display, prec, /*square=*/true);
}

// Human ratio label, e.g. "1:100" for the drawing area, "1:1" for paper scale.
inline QString deriveRatioLabel(double mmPerPoint)
{
    if (mmPerPoint <= 0.0)
        return QString();
    const double ratio = mmPerPoint / kMmPerPoint;
    const double r = std::round(ratio);
    if (r >= 1.0 && std::abs(ratio - r) < 0.02)
        return QStringLiteral("1:%1").arg(static_cast<long long>(r));
    return QStringLiteral("1:%1").arg(ratio, 0, 'f', 2);
}

// --- scale resolution -------------------------------------------------------

// Resolve the active scale for `pagePt` on a page. A valid per-page override
// (manual/calibrated) always wins; otherwise the embedded viewport whose BBox
// contains the point is used (last match wins, per PDF precedence), falling
// back to the last viewport when no BBox contains the point.
inline MeasureScale resolveScale(const PageMeasurement &pm, QPointF pagePt,
                                 const MeasureScale &pageOverride)
{
    if (pageOverride.valid())
        return pageOverride;

    MeasureScale s;
    if (!pm.hasEmbedded || pm.viewports.empty())
        return s; // source == None

    int chosen = -1;
    for (int i = 0; i < static_cast<int>(pm.viewports.size()); ++i) {
        if (pm.viewports[i].bbox.contains(pagePt))
            chosen = i;
    }
    if (chosen < 0)
        chosen = static_cast<int>(pm.viewports.size()) - 1;

    const MeasureViewport &vp = pm.viewports[chosen];
    // /U is usually blank in CAD exports -> assume mm (consistent with /C being
    // a multiple of mm-per-point). Normalize the raw /C to mm per point.
    const MeasureUnit srcUnit = vp.unit.isEmpty()
                                    ? MeasureUnit::Millimeter
                                    : unitFromString(vp.unit, MeasureUnit::Millimeter);
    const double k = mmPerUnit(srcUnit);
    s.mmPerPointX = vp.cx * k;
    s.mmPerPointY = (vp.cy > 0.0 ? vp.cy : vp.cx) * k;
    s.source = MeasureSource::Embedded;
    s.label = !vp.ratio.isEmpty() ? vp.ratio : deriveRatioLabel(s.mmPerPointX);
    return s;
}

} // namespace mervin::measure
