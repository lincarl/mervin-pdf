#include "render/Document.h"
#include "render/GeometryTypes.h"
#include "render/MeasureMath.h"
#include "render/MeasureModel.h"
#include "render/MeasureSnap.h"
#include "render/MeasureTypes.h"
#include "render/RenderEngine.h"

#include <QFileInfo>
#include <QTest>

#include <cmath>
#include <vector>

using namespace mervin;
using namespace mervin::measure;

// Pure measurement math always runs. The viewport/scale-detection cases that
// need a real PDF open docs/drawing.pdf (path baked in via MERVIN_DRAWING_PDF)
// and QSKIP when it is absent, so the suite stays green without the sample.
class TstMeasure : public QObject
{
    Q_OBJECT

private slots:
    void unitConversionRoundTrips();
    void distanceReducesToPointDistanceWhenSquare();
    void areaOfUnitSquare();
    void rightAngleIsNinety();
    void ratioLabels();
    void formatters();
    void autoSiBrackets();
    void resolveScalePicksInnermostViewport();
    void resolveScaleHonoursOverride();
    void calibrationAndRatioOverrides();
    void detectsScaleInDrawingPdf();
    void snapClosestOnSegmentClamps();
    void snapPrefersVertexOverEdge();
    void snapsToEdgeWhenNoVertexInRange();
    void snapReturnsNoneOutsideRadius();
    void extractsGeometryFromDrawingPdf();
};

void TstMeasure::unitConversionRoundTrips()
{
    QCOMPARE(mmPerUnit(MeasureUnit::Millimeter), 1.0);
    QCOMPARE(mmPerUnit(MeasureUnit::Centimeter), 10.0);
    QCOMPARE(mmPerUnit(MeasureUnit::Meter), 1000.0);
    QCOMPARE(mmPerUnit(MeasureUnit::Inch), 25.4);
    QCOMPARE(mmPerUnit(MeasureUnit::Foot), 304.8);
    QCOMPARE(unitFromString(QStringLiteral("MM")), MeasureUnit::Millimeter);
    QCOMPARE(unitFromString(QStringLiteral("ft")), MeasureUnit::Foot);
    QCOMPARE(unitFromString(QStringLiteral("")), MeasureUnit::Millimeter); // fallback
}

void TstMeasure::distanceReducesToPointDistanceWhenSquare()
{
    MeasureScale s;
    s.mmPerPointX = s.mmPerPointY = 2.0;
    s.source = MeasureSource::Manual;
    // 3-4-5 triangle: point distance 5 -> 10 mm at 2 mm/pt.
    QCOMPARE(segmentMm(QPointF(0, 0), QPointF(3, 4), s), 10.0);
}

void TstMeasure::areaOfUnitSquare()
{
    MeasureScale s;
    s.mmPerPointX = s.mmPerPointY = 3.0;
    s.source = MeasureSource::Manual;
    // 1x1 point square -> 1 * 3 * 3 = 9 mm^2; perimeter 4 pts -> 12 mm.
    const std::vector<QPointF> sq = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    QCOMPARE(areaMm2(sq, s), 9.0);
    QCOMPARE(perimeterMm(sq, s), 12.0);
}

void TstMeasure::rightAngleIsNinety()
{
    MeasureScale s;
    s.mmPerPointX = s.mmPerPointY = 1.0;
    s.source = MeasureSource::Manual;
    const double a = angleDeg(QPointF(0, 0), QPointF(1, 0), QPointF(0, 1), s);
    QVERIFY(std::abs(a - 90.0) < 1e-6);
    const double straight = angleDeg(QPointF(0, 0), QPointF(1, 0), QPointF(-1, 0), s);
    QVERIFY(std::abs(straight - 180.0) < 1e-6);
}

void TstMeasure::ratioLabels()
{
    // Inner drawing viewport: 35.27573 mm/pt -> 1:100; sheet: 0.35278 -> 1:1.
    QCOMPARE(deriveRatioLabel(35.27573), QStringLiteral("1:100"));
    QCOMPARE(deriveRatioLabel(0.35278), QStringLiteral("1:1"));
    QCOMPARE(deriveRatioLabel(kMmPerPoint * 50), QStringLiteral("1:50"));
}

void TstMeasure::formatters()
{
    QCOMPARE(formatLengthMm(4200.0, MeasureUnit::Meter, 2), QStringLiteral("4.20 m"));
    QCOMPARE(formatLengthMm(4200.0, MeasureUnit::Millimeter, 0), QStringLiteral("4200 mm"));
    // 1 m^2 == 1e6 mm^2.
    QCOMPARE(formatAreaMm2(1.0e6, MeasureUnit::Meter, 2), QString::fromUtf8("1.00 m\xC2\xB2"));
    QCOMPARE(formatAngle(90.0, 1), QString::fromUtf8("90.0\xC2\xB0"));
}

void TstMeasure::autoSiBrackets()
{
    const QString sup2 = QString::fromUtf8("\xC2\xB2");

    // Under four integer digits: no bracket, identical to the plain formatter.
    QCOMPARE(formatLengthMmAuto(120.0, MeasureUnit::Millimeter, 2), QStringLiteral("120.00 mm"));
    QCOMPARE(formatAreaMm2Auto(500.0, MeasureUnit::Millimeter, 2),
             QStringLiteral("500.00 mm") + sup2);

    // Length: 7128.93 mm -> next unit that drops under four digits is cm.
    QCOMPARE(formatLengthMmAuto(7128.93, MeasureUnit::Millimeter, 2),
             QStringLiteral("7128.93 mm (712.89 cm)"));
    // Larger still steps to m.
    QCOMPARE(formatLengthMmAuto(71289.3, MeasureUnit::Millimeter, 2),
             QStringLiteral("71289.30 mm (71.29 m)"));

    // Area: one step (mm^2 -> cm^2) is enough at this magnitude.
    QCOMPARE(formatAreaMm2Auto(9135.93, MeasureUnit::Millimeter, 2),
             QStringLiteral("9135.93 mm") + sup2 + QStringLiteral(" (91.36 cm") + sup2
                 + QLatin1Char(')'));
    // Area: two steps (mm^2 -> cm^2 still >= 1000 -> m^2).
    QCOMPARE(formatAreaMm2Auto(9123456.93, MeasureUnit::Millimeter, 2),
             QStringLiteral("9123456.93 mm") + sup2 + QStringLiteral(" (9.12 m") + sup2
                 + QLatin1Char(')'));

    // Imperial ladder: inches step up to feet.
    QCOMPARE(formatLengthMmAuto(5000.0 * 25.4, MeasureUnit::Inch, 2),
             QStringLiteral("5000.00 in (416.67 ft)"));
}

void TstMeasure::resolveScalePicksInnermostViewport()
{
    PageMeasurement pm;
    pm.hasEmbedded = true;
    MeasureViewport sheet;
    sheet.bbox = QRectF(0, 0, 1190, 841);
    sheet.cx = sheet.cy = 0.35278; // mm/pt -> 1:1
    MeasureViewport drawing;
    drawing.bbox = QRectF(289, 184, 953 - 289, 756 - 184);
    drawing.cx = drawing.cy = 35.27573; // mm/pt -> 1:100
    pm.viewports = {sheet, drawing};

    // A point inside both BBoxes resolves to the later (drawing) viewport.
    const MeasureScale inside = resolveScale(pm, QPointF(600, 400), MeasureScale{});
    QCOMPARE(inside.source, MeasureSource::Embedded);
    QCOMPARE(inside.label, QStringLiteral("1:100"));
    QVERIFY(std::abs(inside.mmPerPointX - 35.27573) < 1e-4);

    // A point only inside the sheet resolves to 1:1.
    const MeasureScale margin = resolveScale(pm, QPointF(50, 50), MeasureScale{});
    QCOMPARE(margin.label, QStringLiteral("1:1"));
}

void TstMeasure::resolveScaleHonoursOverride()
{
    PageMeasurement pm; // no embedded data
    MeasureScale ov = MeasureModel::fromRatio(50);
    const MeasureScale r = resolveScale(pm, QPointF(10, 10), ov);
    QCOMPARE(r.source, MeasureSource::Manual);
    QCOMPARE(r.label, QStringLiteral("1:50"));

    // With no embedded data and no override -> None.
    const MeasureScale none = resolveScale(pm, QPointF(10, 10), MeasureScale{});
    QVERIFY(!none.valid());
}

void TstMeasure::calibrationAndRatioOverrides()
{
    // Drew 100 pt over a true 5000 mm dimension -> 50 mm/pt -> 1:~142.
    const MeasureScale cal = MeasureModel::fromCalibration(100.0, 5000.0, MeasureUnit::Millimeter);
    QCOMPARE(cal.source, MeasureSource::Calibrated);
    QVERIFY(std::abs(cal.mmPerPointX - 50.0) < 1e-9);

    // Drew 100 pt over 5 m -> same 50 mm/pt.
    const MeasureScale cal2 = MeasureModel::fromCalibration(100.0, 5.0, MeasureUnit::Meter);
    QVERIFY(std::abs(cal2.mmPerPointX - 50.0) < 1e-9);

    const MeasureScale ratio = MeasureModel::fromRatio(100);
    QVERIFY(std::abs(ratio.mmPerPointX - kMmPerPoint * 100) < 1e-9);

    MeasureModel m;
    QVERIFY(!m.hasOverride(0));
    m.setOverride(0, ratio);
    QVERIFY(m.hasOverride(0));
    QCOMPARE(m.override(0).label, QStringLiteral("1:100"));
    m.clearOverride(0);
    QVERIFY(!m.hasOverride(0));
}

void TstMeasure::detectsScaleInDrawingPdf()
{
    const QString path = QStringLiteral(MERVIN_DRAWING_PDF);
    if (!QFileInfo::exists(path))
        QSKIP("docs/drawing.pdf not present");

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(doc->pageCount() >= 1);

    const PageMeasurement pm = doc->pageMeasurement(0);
    QVERIFY(pm.hasEmbedded);
    QVERIFY(pm.viewports.size() >= 2);

    // Inside the drawing area (BBox [289 184 953 756]) -> 1:100.
    const MeasureScale drawing = resolveScale(pm, QPointF(600, 400), MeasureScale{});
    QCOMPARE(drawing.label, QStringLiteral("1:100"));
    QVERIFY(std::abs(drawing.mmPerPointX - 35.27573) < 1e-2);

    // Out on the sheet margin -> 1:1 (paper-true mm).
    const MeasureScale sheet = resolveScale(pm, QPointF(50, 50), MeasureScale{});
    QCOMPARE(sheet.label, QStringLiteral("1:1"));
}

namespace {
// A unit square: vertices CCW, four edges closing the loop.
PageGeometry unitSquareGeometry()
{
    PageGeometry g;
    g.vertices = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    g.segments = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    return g;
}

void verifyClose(QPointF got, double ex, double ey)
{
    QVERIFY2(std::abs(got.x() - ex) < 1e-9 && std::abs(got.y() - ey) < 1e-9,
             qPrintable(QStringLiteral("got (%1,%2) expected (%3,%4)")
                            .arg(got.x())
                            .arg(got.y())
                            .arg(ex)
                            .arg(ey)));
}
} // namespace

void TstMeasure::snapClosestOnSegmentClamps()
{
    // Projection lands inside the segment.
    verifyClose(snap::closestOnSegment(QPointF(0, 0), QPointF(10, 0), QPointF(5, 3)), 5, 0);
    // Projection past an end clamps to that end.
    verifyClose(snap::closestOnSegment(QPointF(0, 0), QPointF(10, 0), QPointF(-5, 3)), 0, 0);
    verifyClose(snap::closestOnSegment(QPointF(0, 0), QPointF(10, 0), QPointF(20, 3)), 10, 0);
    // Degenerate segment returns the point itself.
    verifyClose(snap::closestOnSegment(QPointF(4, 4), QPointF(4, 4), QPointF(9, 9)), 4, 4);
}

void TstMeasure::snapPrefersVertexOverEdge()
{
    const PageGeometry g = unitSquareGeometry();
    // (10,1) lies exactly on the right edge (dist 0) but is only 1.0 from the
    // (10,0) corner: within the radius the endpoint must win outright.
    const snap::SnapResult r = snap::snap(g, QPointF(10, 1), 2.0);
    QCOMPARE(r.type, snap::SnapType::Vertex);
    verifyClose(r.point, 10, 0);
    QVERIFY(std::abs(r.dist2 - 1.0) < 1e-9);
}

void TstMeasure::snapsToEdgeWhenNoVertexInRange()
{
    const PageGeometry g = unitSquareGeometry();
    // Midpoint of the bottom edge: nearest vertex is 5 away (out of range),
    // the edge is 0.5 below -> snaps to the edge.
    const snap::SnapResult r = snap::snap(g, QPointF(5, 0.5), 2.0);
    QCOMPARE(r.type, snap::SnapType::Edge);
    verifyClose(r.point, 5, 0);
    QVERIFY(std::abs(r.dist2 - 0.25) < 1e-9);
}

void TstMeasure::snapReturnsNoneOutsideRadius()
{
    const PageGeometry g = unitSquareGeometry();
    // Dead centre: every edge/vertex is >= 5 away, radius 2 catches nothing.
    QCOMPARE(snap::snap(g, QPointF(5, 5), 2.0).type, snap::SnapType::None);
    // Vertex-only / edge-only toggles are honoured.
    QCOMPARE(snap::snap(g, QPointF(0.5, 0.5), 2.0, /*vertices=*/false, /*edges=*/true).type,
             snap::SnapType::Edge);
    QCOMPARE(snap::snap(g, QPointF(5, 0.5), 2.0, /*vertices=*/true, /*edges=*/false).type,
             snap::SnapType::None);
}

void TstMeasure::extractsGeometryFromDrawingPdf()
{
    const QString path = QStringLiteral(MERVIN_DRAWING_PDF);
    if (!QFileInfo::exists(path))
        QSKIP("docs/drawing.pdf not present");

    RenderEngine engine;
    QString err;
    std::unique_ptr<Document> doc = engine.openDocument(path, QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(doc->pageCount() >= 1);

    const PageGeometry g = doc->pageGeometry(0);
    // A CAD drawing has plenty of vector linework; every segment must reference
    // valid vertex indices.
    QVERIFY(!g.segments.empty());
    QVERIFY(!g.vertices.empty());
    for (const auto &s : g.segments) {
        QVERIFY(s.first >= 0 && s.first < static_cast<int>(g.vertices.size()));
        QVERIFY(s.second >= 0 && s.second < static_cast<int>(g.vertices.size()));
    }
    // Coordinate-space sanity: geometry is reported in (0,0)-based page points,
    // so at least some of the drawing's vertices must land on the sheet itself
    // (off-page/clipped content may legitimately fall outside, so we only
    // require *some* on-page geometry, not all of it).
    const QSizeF pageSz = doc->pageSize(0);
    int onPage = 0;
    for (const QPointF &v : g.vertices) {
        if (v.x() >= 0 && v.x() <= pageSz.width() && v.y() >= 0 && v.y() <= pageSz.height())
            ++onPage;
    }
    QVERIFY(onPage > 0);
    // Calling again returns the cached result (identical sizes).
    const PageGeometry g2 = doc->pageGeometry(0);
    QCOMPARE(g2.vertices.size(), g.vertices.size());
    QCOMPARE(g2.segments.size(), g.segments.size());
}

QTEST_GUILESS_MAIN(TstMeasure)
#include "tst_measure.moc"
