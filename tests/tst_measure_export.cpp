#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QByteArray>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <string>

using namespace mervin;

namespace {

// Build a valid PDF with `n` Letter pages; optionally a non-zero MediaBox origin
// and a /Rotate, to exercise the page->PDF transform.
void makePdf(const QString &path, int n, int rotate = 0, double ox = 0.0, double oy = 0.0)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    for (int i = 0; i < n; ++i) {
        QPDFObjectHandle box = QPDFObjectHandle::newArray();
        box.appendItem(QPDFObjectHandle::newReal(ox, 2));
        box.appendItem(QPDFObjectHandle::newReal(oy, 2));
        box.appendItem(QPDFObjectHandle::newReal(ox + 612.0, 2));
        box.appendItem(QPDFObjectHandle::newReal(oy + 792.0, 2));
        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", box);
        if (rotate != 0)
            page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(rotate));
        dh.addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    }
    QPDFWriter w(q, path.toUtf8().constData());
    w.write();
}

std::string pageContent(QPDFObjectHandle page)
{
    std::string out;
    auto append = [&out](QPDFObjectHandle s) {
        if (!s.isStream())
            return;
        auto b = s.getStreamData();
        if (b)
            out.append(reinterpret_cast<const char *>(b->getBuffer()),
                       static_cast<size_t>(b->getSize()));
        out += '\n';
    };
    auto c = page.getKey("/Contents");
    if (c.isStream())
        append(c);
    else if (c.isArray())
        for (int i = 0; i < c.getArrayNItems(); ++i)
            append(c.getArrayItem(i));
    return out;
}

Measurement distance(int page, QPointF a, QPointF b)
{
    Measurement m;
    m.page = page;
    m.kind = MeasureKind::Distance;
    m.pts = {a, b};
    return m;
}

RenderMeasurement render(const Measurement &m, const QString &label)
{
    RenderMeasurement rm;
    rm.page = m.page;
    rm.kind = m.kind;
    rm.pts = m.pts;
    rm.hasLabelPos = m.hasLabelPos;
    rm.labelPos = m.labelPos;
    rm.label = label;
    rm.toPdf = Mat6{}; // identity is fine for output-presence checks
    return rm;
}

} // namespace

class TstMeasureExport : public QObject
{
    Q_OBJECT

private slots:
    void init() { QVERIFY(dir_.isValid()); }

    void serializeParseRoundTrip();
    void overrideOnlyRoundTrip();
    void embedReadRoundTrip();
    void catalogGateMatchesBlobPresence();
    void embedKeyIsPrivateToCatalog();
    void flattenAppendsContent();
    void transformMapsCornersIntoMediaBox();
    void roundTripsThroughRealDrawing();

private:
    QTemporaryDir dir_;
    QString p(const QString &name) const { return dir_.filePath(name); }
};

void TstMeasureExport::serializeParseRoundTrip()
{
    MeasureDoc md;
    md.unit = MeasureUnit::Centimeter;
    md.precision = 3;
    md.measurements.push_back(distance(0, QPointF(10.0, 20.0), QPointF(110.5, 20.0)));
    Measurement ang;
    ang.page = 1;
    ang.kind = MeasureKind::Angle;
    ang.pts = {QPointF(0, 0), QPointF(50, 50), QPointF(100, 0)};
    ang.hasLabelPos = true;
    ang.labelPos = QPointF(55.0, 40.0);
    md.measurements.push_back(ang);
    PageScale ps;
    ps.page = 0;
    ps.mmPerPointX = ps.mmPerPointY = 0.3528 * 100.0;
    ps.label = QStringLiteral("1:100");
    ps.source = MeasureSource::Manual;
    md.pageScales.push_back(ps);

    MeasureDoc back;
    QVERIFY(parseMeasurements(serializeMeasurements(md), &back));
    QCOMPARE(back.unit, MeasureUnit::Centimeter);
    QCOMPARE(back.precision, 3);
    QCOMPARE(static_cast<int>(back.measurements.size()), 2);
    QCOMPARE(back.measurements[0].kind, MeasureKind::Distance);
    QCOMPARE(back.measurements[0].pts.size(), size_t(2));
    QCOMPARE(back.measurements[0].pts[1].x(), 110.5);
    QCOMPARE(back.measurements[1].kind, MeasureKind::Angle);
    QVERIFY(back.measurements[1].hasLabelPos);
    QCOMPARE(back.measurements[1].labelPos, QPointF(55.0, 40.0));
    QCOMPARE(static_cast<int>(back.pageScales.size()), 1);
    QCOMPARE(back.pageScales[0].page, 0);
    QVERIFY(back.overridesModel().hasOverride(0));
}

void TstMeasureExport::overrideOnlyRoundTrip()
{
    // A calibration with NO committed measurement must persist on its own: the
    // page-scale override is what the "Reset to embedded scale" feature relies on
    // being restored when the file is reopened (TabPage::open). Lock the contract
    // that an override-only doc serializes and parses back with the override intact
    // and no measurements.
    MeasureDoc md;
    md.unit = MeasureUnit::Meter;
    PageScale ps;
    ps.page = 2;
    ps.mmPerPointX = ps.mmPerPointY = 0.3528 * 50.0;
    ps.label = QStringLiteral("1:50");
    ps.source = MeasureSource::Calibrated;
    md.pageScales.push_back(ps);

    MeasureDoc back;
    QVERIFY(parseMeasurements(serializeMeasurements(md), &back));
    QVERIFY(back.measurements.empty());
    QCOMPARE(static_cast<int>(back.pageScales.size()), 1);
    const MeasureModel m = back.overridesModel();
    QVERIFY(m.hasAnyOverride());
    QVERIFY(m.hasOverride(2));
    QCOMPARE(m.override(2).source, MeasureSource::Calibrated);
}

void TstMeasureExport::embedReadRoundTrip()
{
    const QString in = p("in.pdf"), out = p("embed.pdf");
    makePdf(in, 2);
    MeasureDoc md;
    md.measurements.push_back(distance(0, QPointF(5, 5), QPointF(55, 5)));

    QString err;
    QCOMPARE(MeasureExport::embedMervin(in, out, md, QString(), &err), MeasureExport::Status::Ok);

    auto blob = MeasureExport::readMervinBlob(out);
    QVERIFY(blob.has_value());
    MeasureDoc back;
    QVERIFY(parseMeasurements(*blob, &back));
    QCOMPARE(static_cast<int>(back.measurements.size()), 1);
    QCOMPARE(back.measurements[0].pts[1].x(), 55.0);

    // A document without the key reads back as nullopt.
    QVERIFY(!MeasureExport::readMervinBlob(in).has_value());
}

void TstMeasureExport::catalogGateMatchesBlobPresence()
{
    // TabPage::open only pays for the qpdf reopen readMervinBlob needs when
    // Document::hasMervinMeasurements() says the catalog carries the blob - that
    // reopen re-parses the whole file, and almost no file has the key. A false
    // negative here would silently drop a document's saved measurements and
    // calibration, so tie the MuPDF-side gate to what qpdf actually finds.
    const QString plain = p("gate-plain.pdf"), embedded = p("gate-embed.pdf");
    makePdf(plain, 2);
    MeasureDoc md;
    md.measurements.push_back(distance(0, QPointF(5, 5), QPointF(55, 5)));
    QCOMPARE(MeasureExport::embedMervin(plain, embedded, md), MeasureExport::Status::Ok);

    RenderEngine engine;
    QString err;
    bool needsPw = false;

    auto before = engine.openDocument(plain, QString(), &err, &needsPw);
    QVERIFY2(before != nullptr, qPrintable(err));
    QVERIFY(!MeasureExport::readMervinBlob(plain).has_value());
    QVERIFY(!before->hasMervinMeasurements()); // no blob -> no second parse

    auto after = engine.openDocument(embedded, QString(), &err, &needsPw);
    QVERIFY2(after != nullptr, qPrintable(err));
    QVERIFY(MeasureExport::readMervinBlob(embedded).has_value());
    QVERIFY(after->hasMervinMeasurements()); // blob present -> gate opens
    QVERIFY(after->hasMervinMeasurements()); // and the cached answer is stable
}

void TstMeasureExport::embedKeyIsPrivateToCatalog()
{
    const QString in = p("in2.pdf"), out = p("embed2.pdf");
    makePdf(in, 3);
    MeasureDoc md;
    md.measurements.push_back(distance(1, QPointF(0, 0), QPointF(10, 0)));
    QCOMPARE(MeasureExport::embedMervin(in, out, md), MeasureExport::Status::Ok);

    QPDF q;
    q.processFile(out.toUtf8().constData());
    QVERIFY(q.getRoot().hasKey("/Mervin_Measurements"));
    QPDFPageDocumentHelper dh(q);
    auto pages = dh.getAllPages();
    QCOMPARE(static_cast<int>(pages.size()), 3); // page count unchanged
    for (auto &page : pages)
        QVERIFY(!page.getObjectHandle().hasKey("/Mervin_Measurements")); // not on pages
}

void TstMeasureExport::flattenAppendsContent()
{
    const QString in = p("in3.pdf"), out = p("flat.pdf");
    makePdf(in, 1);
    std::vector<RenderMeasurement> marks{
        render(distance(0, QPointF(50, 50), QPointF(200, 50)), QStringLiteral("1234.50 mm"))};
    QString err;
    QCOMPARE(MeasureExport::flatten(in, out, marks, QString(), &err), MeasureExport::Status::Ok);

    QPDF q;
    q.processFile(out.toUtf8().constData());
    QPDFPageDocumentHelper dh(q);
    auto page = dh.getAllPages()[0].getObjectHandle();
    const std::string content = pageContent(page);
    QVERIFY(content.find(" S") != std::string::npos);  // a stroked path
    QVERIFY(content.find("Tj") != std::string::npos);   // the value label
    QVERIFY(content.find("1234.50") != std::string::npos);
    // The label font was registered on the page resources.
    auto fonts = page.getKey("/Resources").getKey("/Font");
    QVERIFY(fonts.isDictionary());
    QVERIFY(fonts.hasKey(std::string("/") + kMeasureFontResource));
}

void TstMeasureExport::transformMapsCornersIntoMediaBox()
{
    // A page with a non-zero MediaBox origin and a 90° rotation: the four corners
    // of app page-point space must map onto the four MediaBox corners.
    const QString in = p("rot.pdf");
    makePdf(in, 1, /*rotate=*/90, /*ox=*/100.0, /*oy=*/200.0);

    RenderEngine engine;
    QString err;
    bool needsPw = false;
    auto doc = engine.openDocument(in, QString(), &err, &needsPw);
    if (!doc) {
        QSKIP("MuPDF could not open the fixture");
    }
    const std::array<double, 6> m = doc->pagePointToPdfMatrix(0);
    const QSizeF sz = doc->pageSize(0); // rotated bounds (width/height swapped)
    const Mat6 mat{m[0], m[1], m[2], m[3], m[4], m[5]};

    const QPointF corners[4] = {QPointF(0, 0), QPointF(sz.width(), 0), QPointF(0, sz.height()),
                                QPointF(sz.width(), sz.height())};
    const QPointF mbox[4] = {QPointF(100, 200), QPointF(712, 200), QPointF(100, 992),
                             QPointF(712, 992)};
    for (const QPointF &c : corners) {
        const QPointF pdf = applyMat(mat, c);
        bool near = false;
        for (const QPointF &mb : mbox)
            if (std::abs(pdf.x() - mb.x()) < 1.0 && std::abs(pdf.y() - mb.y()) < 1.0)
                near = true;
        QVERIFY2(near, qPrintable(QStringLiteral("corner mapped to (%1,%2)")
                                      .arg(pdf.x())
                                      .arg(pdf.y())));
    }
}

void TstMeasureExport::roundTripsThroughRealDrawing()
{
#ifndef MERVIN_DRAWING_PDF
    QSKIP("no sample drawing configured");
#else
    const QString src = QStringLiteral(MERVIN_DRAWING_PDF);
    if (!QFileInfo::exists(src))
        QSKIP("sample drawing not present");

    RenderEngine engine;
    QString err;
    bool needsPw = false;
    auto doc = engine.openDocument(src, QString(), &err, &needsPw);
    QVERIFY2(doc != nullptr, qPrintable(err));
    const int pages = doc->pageCount();
    QVERIFY(pages > 0);

    // A real measurement on page 0, using the page's actual transform.
    const std::array<double, 6> a = doc->pagePointToPdfMatrix(0);
    const QSizeF sz = doc->pageSize(0);
    RenderMeasurement rm;
    rm.page = 0;
    rm.kind = MeasureKind::Distance;
    rm.pts = {QPointF(sz.width() * 0.25, sz.height() * 0.25),
              QPointF(sz.width() * 0.75, sz.height() * 0.25)};
    rm.label = QStringLiteral("test 123.0 mm");
    rm.toPdf = Mat6{a[0], a[1], a[2], a[3], a[4], a[5]};

    // Each output must be a valid PDF MuPDF can reopen, with the page count intact.
    const QString flat = p("real-flat.pdf");
    QCOMPARE(MeasureExport::flatten(src, flat, {rm}, QString(), &err), MeasureExport::Status::Ok);
    auto fdoc = engine.openDocument(flat, QString(), &err, &needsPw);
    QVERIFY2(fdoc != nullptr, qPrintable(err));
    QCOMPARE(fdoc->pageCount(), pages);

    MeasureDoc md;
    Measurement m = distance(0, rm.pts[0], rm.pts[1]);
    md.measurements.push_back(m);
    const QString emb = p("real-emb.pdf");
    QCOMPARE(MeasureExport::embedMervin(src, emb, md, QString(), &err), MeasureExport::Status::Ok);
    auto edoc = engine.openDocument(emb, QString(), &err, &needsPw);
    QVERIFY2(edoc != nullptr, qPrintable(err));
    QCOMPARE(edoc->pageCount(), pages);
    QVERIFY(edoc->hasMervinMeasurements()); // the open-time gate agrees on a real file
    auto blob = MeasureExport::readMervinBlob(emb);
    QVERIFY(blob.has_value());
    MeasureDoc back;
    QVERIFY(parseMeasurements(*blob, &back));
    QCOMPARE(static_cast<int>(back.measurements.size()), 1);
#endif
}

QTEST_MAIN(TstMeasureExport)
#include "tst_measure_export.moc"
