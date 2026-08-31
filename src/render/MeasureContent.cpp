#include "render/MeasureContent.h"

#include "render/MeasureMath.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mervin {

namespace {

QString kindToString(MeasureKind k)
{
    switch (k) {
    case MeasureKind::Distance: return QStringLiteral("Distance");
    case MeasureKind::Polyline: return QStringLiteral("Polyline");
    case MeasureKind::Area:     return QStringLiteral("Area");
    case MeasureKind::Angle:    return QStringLiteral("Angle");
    }
    return QStringLiteral("Distance");
}

MeasureKind kindFromString(const QString &s)
{
    if (s == QLatin1String("Polyline")) return MeasureKind::Polyline;
    if (s == QLatin1String("Area"))     return MeasureKind::Area;
    if (s == QLatin1String("Angle"))    return MeasureKind::Angle;
    return MeasureKind::Distance;
}

QString sourceToString(MeasureSource s)
{
    return s == MeasureSource::Calibrated ? QStringLiteral("Calibrated") : QStringLiteral("Manual");
}

MeasureSource sourceFromString(const QString &s)
{
    return s == QLatin1String("Calibrated") ? MeasureSource::Calibrated : MeasureSource::Manual;
}

// Format a coordinate / length for a PDF content stream: up to 3 decimals, no
// locale, trailing zeros trimmed. (C locale via snprintf, so '.' is the point.)
std::string pnum(double v)
{
    if (std::abs(v) < 1e-6)
        v = 0.0;
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    std::string s(b);
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot)
            --last;
        s.erase(last + 1);
    }
    return s;
}

// Decode to Latin1 (a subset of the WinAnsiEncoding the label font uses, which
// covers the ·, ², ° glyphs the value strings contain) and escape the PDF string
// delimiters. Characters outside Latin1 become '?'.
std::string escapeLatin1(const QString &s)
{
    const QByteArray b = s.toLatin1();
    std::string out;
    out.reserve(static_cast<size_t>(b.size()) + 8);
    for (char c : b) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) >= 32) {
            out += c;
        }
    }
    return out;
}

} // namespace

MeasureModel MeasureDoc::overridesModel() const
{
    MeasureModel m;
    for (const PageScale &ps : pageScales) {
        MeasureScale s;
        s.mmPerPointX = ps.mmPerPointX;
        s.mmPerPointY = ps.mmPerPointY;
        s.label = ps.label;
        s.source = ps.source;
        if (s.valid())
            m.setOverride(ps.page, s);
    }
    return m;
}

QByteArray serializeMeasurements(const MeasureDoc &doc)
{
    QJsonObject root;
    root[QStringLiteral("mervinMeasureVersion")] = doc.version;
    root[QStringLiteral("unit")] = measure::unitSuffix(doc.unit);
    root[QStringLiteral("precision")] = doc.precision;
    root[QStringLiteral("lineWidth")] = doc.lineWidth;

    QJsonArray scales;
    for (const PageScale &ps : doc.pageScales) {
        QJsonObject o;
        o[QStringLiteral("page")] = ps.page;
        o[QStringLiteral("mmPerPointX")] = ps.mmPerPointX;
        o[QStringLiteral("mmPerPointY")] = ps.mmPerPointY;
        o[QStringLiteral("label")] = ps.label;
        o[QStringLiteral("source")] = sourceToString(ps.source);
        scales.append(o);
    }
    root[QStringLiteral("pageScales")] = scales;

    QJsonArray ms;
    for (const Measurement &m : doc.measurements) {
        QJsonObject o;
        o[QStringLiteral("page")] = m.page;
        o[QStringLiteral("kind")] = kindToString(m.kind);
        QJsonArray pts;
        for (const QPointF &p : m.pts) {
            QJsonArray xy;
            xy.append(p.x());
            xy.append(p.y());
            pts.append(xy);
        }
        o[QStringLiteral("pts")] = pts;
        if (m.hasLabelPos) {
            o[QStringLiteral("hasLabelPos")] = true;
            QJsonArray lp;
            lp.append(m.labelPos.x());
            lp.append(m.labelPos.y());
            o[QStringLiteral("labelPos")] = lp;
        }
        ms.append(o);
    }
    root[QStringLiteral("measurements")] = ms;

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool parseMeasurements(const QByteArray &json, MeasureDoc *out)
{
    if (!out)
        return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();

    *out = MeasureDoc{};
    out->version = root.value(QStringLiteral("mervinMeasureVersion")).toInt(1);
    out->unit = measure::unitFromString(root.value(QStringLiteral("unit")).toString(),
                                        MeasureUnit::Millimeter);
    out->precision = root.value(QStringLiteral("precision")).toInt(2);
    out->lineWidth = root.value(QStringLiteral("lineWidth")).toDouble(2.0);

    for (const QJsonValue &v : root.value(QStringLiteral("pageScales")).toArray()) {
        const QJsonObject o = v.toObject();
        PageScale ps;
        ps.page = o.value(QStringLiteral("page")).toInt(-1);
        ps.mmPerPointX = o.value(QStringLiteral("mmPerPointX")).toDouble();
        ps.mmPerPointY = o.value(QStringLiteral("mmPerPointY")).toDouble();
        ps.label = o.value(QStringLiteral("label")).toString();
        ps.source = sourceFromString(o.value(QStringLiteral("source")).toString());
        if (ps.page >= 0)
            out->pageScales.push_back(ps);
    }

    for (const QJsonValue &v : root.value(QStringLiteral("measurements")).toArray()) {
        const QJsonObject o = v.toObject();
        Measurement m;
        m.page = o.value(QStringLiteral("page")).toInt(-1);
        m.kind = kindFromString(o.value(QStringLiteral("kind")).toString());
        for (const QJsonValue &pv : o.value(QStringLiteral("pts")).toArray()) {
            const QJsonArray xy = pv.toArray();
            if (xy.size() == 2)
                m.pts.emplace_back(xy.at(0).toDouble(), xy.at(1).toDouble());
        }
        const QJsonArray lp = o.value(QStringLiteral("labelPos")).toArray();
        if (o.value(QStringLiteral("hasLabelPos")).toBool() && lp.size() == 2) {
            m.hasLabelPos = true;
            m.labelPos = QPointF(lp.at(0).toDouble(), lp.at(1).toDouble());
        }
        if (m.page >= 0 && m.pts.size() >= 2)
            out->measurements.push_back(std::move(m));
    }
    return true;
}

QString formatMeasurementValue(MeasureKind kind, const std::vector<QPointF> &pts,
                               const MeasureScale &scaleIn, MeasureUnit unit, int precision)
{
    using namespace measure;
    if (pts.empty())
        return {};
    MeasureScale s = scaleIn;
    const bool noScale = !s.valid();
    if (noScale)
        s.mmPerPointX = s.mmPerPointY = kMmPerPoint; // fall back to paper (1:1)

    QString out;
    switch (kind) {
    case MeasureKind::Distance:
        if (pts.size() < 2)
            return {};
        out = formatLengthMmAuto(segmentMm(pts[0], pts[1], s), unit, precision);
        break;
    case MeasureKind::Polyline:
        if (pts.size() < 2)
            return {};
        out = formatLengthMmAuto(polylineMm(pts, s), unit, precision);
        break;
    case MeasureKind::Area:
        if (pts.size() < 3) {
            if (pts.size() < 2)
                return {};
            out = formatLengthMmAuto(polylineMm(pts, s), unit, precision);
        } else {
            // Area on the first line, perimeter on a second line below it. No
            // "Perimeter" label is needed - the units (mm² vs mm) make it clear
            // which is which. The '\n' is honoured by the on-screen readout/pill
            // and the burned-in PDF label; the compact measurement list flattens
            // it to an inline form.
            out = QCoreApplication::translate("MeasureContent", "%1\n%2")
                      .arg(formatAreaMm2Auto(areaMm2(pts, s), unit, precision),
                           formatLengthMmAuto(perimeterMm(pts, s), unit, precision));
        }
        break;
    case MeasureKind::Angle:
        if (pts.size() < 3) {
            if (pts.size() < 2)
                return {};
            out = formatLengthMmAuto(segmentMm(pts[0], pts[1], s), unit, precision);
        } else {
            out = formatAngle(angleDeg(pts[1], pts[0], pts[2], s), precision);
        }
        break;
    }
    if (noScale && kind != MeasureKind::Angle)
        out += QCoreApplication::translate("MeasureContent", " (paper)");
    return out;
}

std::string emitMeasurementOps(const RenderMeasurement &rm, const EmitStyle &st)
{
    if (rm.pts.size() < 2)
        return {};

    std::vector<QPointF> P;
    P.reserve(rm.pts.size());
    for (const QPointF &pt : rm.pts)
        P.push_back(applyMat(rm.toPdf, pt));

    std::string s;
    s.reserve(512);
    char buf[160];
    s += "q\n";
    std::snprintf(buf, sizeof(buf), "%s %s %s RG\n", pnum(st.strokeR).c_str(),
                  pnum(st.strokeG).c_str(), pnum(st.strokeB).c_str());
    s += buf;
    std::snprintf(buf, sizeof(buf), "%s w 1 J 1 j\n", pnum(st.lineWidth).c_str());
    s += buf;

    auto moveTo = [&](QPointF p) {
        std::snprintf(buf, sizeof(buf), "%s %s m\n", pnum(p.x()).c_str(), pnum(p.y()).c_str());
        s += buf;
    };
    auto lineTo = [&](QPointF p) {
        std::snprintf(buf, sizeof(buf), "%s %s l\n", pnum(p.x()).c_str(), pnum(p.y()).c_str());
        s += buf;
    };
    auto polyPath = [&] {
        moveTo(P[0]);
        for (size_t i = 1; i < P.size(); ++i)
            lineTo(P[i]);
    };

    if (rm.kind == MeasureKind::Area && P.size() >= 3) {
        std::snprintf(buf, sizeof(buf), "%s %s %s rg\n", pnum(st.fillR).c_str(),
                      pnum(st.fillG).c_str(), pnum(st.fillB).c_str());
        s += buf;
        polyPath();
        s += "h f\n";
        polyPath();
        s += "h S\n";
    } else if (rm.kind == MeasureKind::Angle && rm.pts.size() >= 3) {
        moveTo(P[0]);
        lineTo(P[1]);
        lineTo(P[2]);
        s += "S\n";
        // Small arc at the vertex, generated in page-point space (so it sweeps the
        // real interior angle) then transformed - matches the on-screen arc.
        const QPointF v = rm.pts[1], a = rm.pts[0], b = rm.pts[2];
        double a0 = std::atan2(a.y() - v.y(), a.x() - v.x());
        double a1 = std::atan2(b.y() - v.y(), b.x() - v.x());
        double span = a1 - a0;
        while (span <= -measure::kPi)
            span += 2.0 * measure::kPi;
        while (span > measure::kPi)
            span -= 2.0 * measure::kPi;
        const double r = 16.0;
        const int steps = std::max(2, static_cast<int>(std::round(std::abs(span) / (measure::kPi / 18.0))));
        for (int i = 0; i <= steps; ++i) {
            const double ang = a0 + span * (static_cast<double>(i) / steps);
            const QPointF q = applyMat(rm.toPdf,
                                       QPointF(v.x() + r * std::cos(ang), v.y() + r * std::sin(ang)));
            if (i == 0)
                moveTo(q);
            else
                lineTo(q);
        }
        s += "S\n";
    } else {
        polyPath();
        s += "S\n";
    }

    if (!rm.label.isEmpty()) {
        QPointF anchor;
        if (rm.hasLabelPos) {
            anchor = applyMat(rm.toPdf, rm.labelPos);
        } else if (rm.kind == MeasureKind::Angle) {
            anchor = P[1];
        } else {
            double cx = 0.0, cy = 0.0;
            for (const QPointF &p : P) {
                cx += p.x();
                cy += p.y();
            }
            anchor = QPointF(cx / P.size(), cy / P.size());
        }
        // A label may carry several lines (e.g. an area's value with its perimeter
        // below). Split on '\n' BEFORE escaping - escapeLatin1 drops control chars,
        // so the newline would otherwise vanish and the lines would run together.
        const QStringList rawLines = rm.label.split(QLatin1Char('\n'));
        std::vector<std::string> lines;
        int maxLen = 1;
        for (const QString &ln : rawLines) {
            lines.push_back(escapeLatin1(ln));
            maxLen = std::max(maxLen, static_cast<int>(ln.length()));
        }
        const double fs = st.fontSize;
        const double lineH = fs * 1.25; // baseline-to-baseline leading
        const double tw = 0.52 * fs * maxLen;
        const double pad = 3.0;
        const double pw = tw + 2.0 * pad;
        // n=1 reduces to the original single-line box (fs + 2*pad); each extra line
        // adds one leading. firstBaseY below then reproduces the old baseline exactly.
        const double ph = fs + static_cast<double>(lines.size() - 1) * lineH + 2.0 * pad;
        const double x = anchor.x() - pw / 2.0;
        const double y = anchor.y() - ph / 2.0;
        s += "q\n1 1 1 rg\n";
        std::snprintf(buf, sizeof(buf), "%s %s %s %s re f\n", pnum(x).c_str(), pnum(y).c_str(),
                      pnum(pw).c_str(), pnum(ph).c_str());
        s += buf;
        std::snprintf(buf, sizeof(buf), "0.6 0.6 0.6 RG 0.5 w %s %s %s %s re S\n", pnum(x).c_str(),
                      pnum(y).c_str(), pnum(pw).c_str(), pnum(ph).c_str());
        s += buf;
        s += "0 0 0 rg BT ";
        std::snprintf(buf, sizeof(buf), "/%s %s Tf %s TL ", kMeasureFontResource, pnum(fs).c_str(),
                      pnum(lineH).c_str());
        s += buf;
        // Baseline of the first (top) line: a pad below the box top, less the ascent.
        const double firstBaseY = y + ph - pad - fs * 0.82;
        std::snprintf(buf, sizeof(buf), "%s %s Td ", pnum(x + pad).c_str(), pnum(firstBaseY).c_str());
        s += buf;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0)
                s += "T* "; // advance one line (uses the TL leading set above)
            s += "(";
            s += lines[i];
            s += ") Tj ";
        }
        s += "ET\nQ\n";
    }

    s += "Q\n";
    return s;
}

} // namespace mervin
