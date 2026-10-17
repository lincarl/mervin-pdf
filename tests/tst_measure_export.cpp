#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "security/MeasurementPages.h"
#include "security/PageOps.h"

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_String.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <functional>
#include <map>
#include <string>
#include <vector>

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
    const QByteArray outputName = path.toUtf8();
    QPDFWriter w(q, outputName.constData());
    w.write();
}

// Compresses `data` once with qpdf's Flate encoder. Pl_String exists from qpdf 11.0.
std::string deflate(const std::string &data)
{
    std::string out;
    Pl_String sink("deflated", nullptr, out);
    Pl_Flate flate("deflate", &sink, Pl_Flate::a_deflate);
    flate.writeString(data);
    flate.finish();
    return out;
}

// Writes a one-page PDF whose catalog holds `encoded` as /Mervin_Measurements, stored as
// given under `filters` (null for none). `encrypt`, when given, sets the writer's
// encryption with password bytes passed to qpdf unchanged. `extra` adds stream
// dictionary entries, written in PDF syntax.
void makeBlobPdf(const QString &path, const std::string &encoded, QPDFObjectHandle filters,
                 const std::function<void(QPDFWriter &)> &encrypt = {},
                 const std::map<std::string, std::string> &extra = {})
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper(q).addPage(QPDFPageObjectHelper(q.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] /Resources << >> >>"))), false);
    if (!encoded.empty()) {
        auto blob = q.newStream();
        blob.replaceStreamData(encoded, filters, QPDFObjectHandle::newNull());
        for (const auto &[key, value] : extra)
            blob.getDict().replaceKey(key, QPDFObjectHandle::parse(value));
        q.getRoot().replaceKey("/Mervin_Measurements", blob);
    }
    const QByteArray outputName = path.toUtf8();
    QPDFWriter w(q, outputName.constData());
    w.setStreamDataMode(qpdf_s_preserve);
    w.setDecodeLevel(qpdf_dl_none);
    if (encrypt)
        encrypt(w);
    w.write();
}

// R4 AES-128 keyed with the PDFDocEncoding bytes of "café", as the PDF format requires.
void pdfDocCafe(QPDFWriter &w)
{
    w.setR4EncryptionParametersInsecure("caf\xE9", "caf\xE9", true, true, true, true, true, true,
                                        qpdf_r3p_full, true, true);
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

// Prefixes each 16-byte row of `data`, padded with leading spaces, with PNG filter
// type 0, as /Predictor 12 /Columns 16 expects before Flate.
std::string pngRows(const std::string &data)
{
    const std::string padded = std::string((16 - data.size() % 16) % 16, ' ') + data;
    std::string rows;
    for (size_t at = 0; at < padded.size(); at += 16)
        rows += '\0' + padded.substr(at, 16);
    return rows;
}

// One manual scale on page 0 whose label holds `labelLength` characters, a cheap way to
// give the measurement blob a chosen size.
MeasureDoc longLabel(qsizetype labelLength)
{
    MeasureDoc md;
    PageScale scale;
    scale.page = 0;
    scale.mmPerPointX = scale.mmPerPointY = 1.0;
    scale.label = QString(labelLength, QLatin1Char('x'));
    md.pageScales.push_back(scale);
    return md;
}

// The reason a write gives when the measurements would pass the read limit.
QString tooLargeReason()
{
    return QCoreApplication::translate("mervin::measurementPages",
                                       "The measurements are too large to save.");
}

// One distance on page 0.
MeasureDoc oneDistance()
{
    MeasureDoc md;
    md.measurements.push_back(distance(0, QPointF(5, 5), QPointF(55, 5)));
    return md;
}

// Rows for a decoded blob exactly at the cap and one byte past it.
void capRows()
{
    QTest::addColumn<qsizetype>("size");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("at the cap") << kMaxMeasurementBlobBytes << true;
    QTest::newRow("one byte over") << kMaxMeasurementBlobBytes + 1 << false;
}

// Writes a one-page PDF whose blob is valid measurement JSON padded with leading spaces
// to `size` bytes, compressed once as Mervin writes it. It stays small on disk.
void makeLargeBlobPdf(const QString &path, qsizetype size)
{
    const QByteArray json = serializeMeasurements(oneDistance());
    const std::string data = std::string(size_t(size - json.size()), ' ') + json.toStdString();
    makeBlobPdf(path, deflate(data), QPDFObjectHandle::newName("/FlateDecode"));
    QVERIFY(QFileInfo(path).size() < 64 * 1024);
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

// Knows only the "(paper)" note, so a test can tell translated value text from
// English without depending on what a real catalog holds.
class PaperNoteTranslator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText, const char *, int) const override
    {
        if (qstrcmp(context, "MeasureContent") == 0 && qstrcmp(sourceText, "%1 (paper)") == 0)
            return QStringLiteral("%1 (translated)");
        return {};
    }
    bool isEmpty() const override { return false; }
};

} // namespace

class TstMeasureExport : public QObject
{
    Q_OBJECT

private slots:
    void init() { QVERIFY(dir_.isValid()); }

    void serializeParseRoundTrip();
    void overrideOnlyRoundTrip();
    void embedReadRoundTrip();
    void pdfDocKeyedSourceReadsMeasurements();
    void pdfDocKeyedSourceSavesMeasurements();
    void blobReadStopsAtTheCap_data();
    void blobReadStopsAtTheCap();
    void pageOperationsStopAtTheCap_data();
    void pageOperationsStopAtTheCap();
    void foreignEncodingsGiveNoData_data();
    void foreignEncodingsGiveNoData();
    void mergeRefusesMeasurementsPastTheCap_data();
    void mergeRefusesMeasurementsPastTheCap();
    void saveRefusesMeasurementsPastTheCap();
    void openReaderMatchesQpdf();
    void embedKeyIsPrivateToCatalog();
    void valueLabelsWithoutScale();
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

/// An R4 file keyed in PDFDocEncoding returns its measurements for the password as typed.
void TstMeasureExport::pdfDocKeyedSourceReadsMeasurements()
{
    const QByteArray json = serializeMeasurements(oneDistance());
    const QString stored = p("pd-stored.pdf");
    makeBlobPdf(stored, json.toStdString(), QPDFObjectHandle::newNull(), pdfDocCafe);
    const auto blob = MeasureExport::readMervinBlob(stored, QStringLiteral("café"));
    QVERIFY(blob.has_value());
    QCOMPARE(*blob, json);
    // Opening reads the same blob through MuPDF, which decrypts it with the open document.
    RenderEngine engine;
    QString err;
    const auto document = engine.openDocument(stored, QStringLiteral("café"), &err, nullptr);
    QVERIFY2(document, qPrintable(err));
    QVERIFY(document->mervinMeasurements() == blob);
}

/// Save edits stores measurements in an R4 file keyed in PDFDocEncoding.
void TstMeasureExport::pdfDocKeyedSourceSavesMeasurements()
{
    const QString password = QStringLiteral("café");
    const QString plain = p("pd-plain.pdf"), saved = p("pd-saved.pdf");
    makeBlobPdf(plain, {}, QPDFObjectHandle::newNull(), pdfDocCafe);
    QString err;
    QCOMPARE(MeasureExport::embedMervin(plain, saved, oneDistance(), password, &err),
             MeasureExport::Status::Ok);
    const auto blob = MeasureExport::readMervinBlob(saved, password);
    QVERIFY(blob.has_value());
    MeasureDoc back;
    QVERIFY(parseMeasurements(*blob, &back));
    QCOMPARE(static_cast<int>(back.measurements.size()), 1);
}

void TstMeasureExport::blobReadStopsAtTheCap_data()
{
    capRows();
}

/// Both blob readers return a compressed blob only up to the cap.
void TstMeasureExport::blobReadStopsAtTheCap()
{
    QFETCH(qsizetype, size);
    QFETCH(bool, accepted);
    const QString in = p("cap-read.pdf");
    makeLargeBlobPdf(in, size);
    const auto blob = MeasureExport::readMervinBlob(in);
    QCOMPARE(blob.has_value(), accepted);
    if (accepted)
        QCOMPARE(blob->size(), size);
    // Opening a document reads the blob through MuPDF.
    RenderEngine engine;
    const auto document = engine.openDocument(in);
    QVERIFY(document);
    const auto opened = document->mervinMeasurements();
    QCOMPARE(opened.has_value(), accepted);
    if (accepted)
        QCOMPARE(opened->size(), size);
}

void TstMeasureExport::pageOperationsStopAtTheCap_data()
{
    capRows();
}

/// Page operations refuse a blob past the cap as invalid metadata.
void TstMeasureExport::pageOperationsStopAtTheCap()
{
    QFETCH(qsizetype, size);
    QFETCH(bool, accepted);
    const QString in = p("cap-rotate.pdf"), out = p("cap-rotated.pdf");
    makeLargeBlobPdf(in, size);
    QString err;
    QCOMPARE(PageOps::rotatePages(in, out, {0}, 90, true, {}, &err),
             accepted ? PageOps::Status::Ok : PageOps::Status::Failed);
    if (!accepted)
        QCOMPARE(err, QCoreApplication::translate("mervin::measurementPages",
                                                  "Invalid measurement metadata."));
}

void TstMeasureExport::foreignEncodingsGiveNoData_data()
{
    QTest::addColumn<QString>("filter");   // the /Filter value, in PDF syntax
    QTest::addColumn<QString>("key");      // another stream dictionary key, if any
    QTest::addColumn<QString>("value");    // its value
    QTest::addColumn<int>("deflations");   // times the data is Flate-compressed
    QTest::addColumn<bool>("png");         // rows prefixed for a PNG predictor
    QTest::addColumn<bool>("hex");         // ASCIIHex-encoded
    QTest::addColumn<bool>("accepted");
    const QString flate = QStringLiteral("/FlateDecode");
    const QString predictor = QStringLiteral("<< /Predictor 12 /Columns 16 >>");
    // Mervin writes the blob with a single /FlateDecode.
    QTest::newRow("plain") << QStringLiteral("null") << QString() << QString() << 0 << false
                           << false << true;
    QTest::newRow("Flate") << flate << QString() << QString() << 1 << false << false << true;
    QTest::newRow("Flate in a one-element array")
        << QStringLiteral("[/FlateDecode]") << QString() << QString() << 1 << false << false << true;
    // Chained Flate stages can inflate data that the next stage consumes without output.
    QTest::newRow("two Flate filters") << QStringLiteral("[/FlateDecode /FlateDecode]") << QString()
                                       << QString() << 2 << false << false << false;
    // qpdf sizes a predictor's row buffers from /Columns before any data arrives.
    QTest::newRow("Flate with a PNG predictor")
        << flate << QStringLiteral("/DecodeParms") << predictor << 1 << true << false << false;
    QTest::newRow("predictor under the /DP abbreviation")
        << flate << QStringLiteral("/DP") << predictor << 1 << true << false << false;
    // MuPDF reads /F as /Filter when /Filter is absent, and qpdf ignores it, so only the
    // refusal keeps the two readers in agreement.
    QTest::newRow("Flate under the /F abbreviation")
        << QStringLiteral("null") << QStringLiteral("/F") << flate << 1 << false << false << false;
    QTest::newRow("hex filter") << QStringLiteral("/ASCIIHexDecode") << QString() << QString() << 0
                                << false << true << false;
}

/// The blob's readers decode only the plain or single-Flate streams Mervin writes, before
/// any filter can allocate memory from the stream's own parameters.
void TstMeasureExport::foreignEncodingsGiveNoData()
{
    QFETCH(QString, filter);
    QFETCH(QString, key);
    QFETCH(QString, value);
    QFETCH(int, deflations);
    QFETCH(bool, png);
    QFETCH(bool, hex);
    QFETCH(bool, accepted);
    const std::string json = serializeMeasurements(oneDistance()).toStdString();
    std::string encoded = png ? pngRows(json) : json;
    for (int i = 0; i < deflations; ++i)
        encoded = deflate(encoded);
    if (hex)
        encoded = QByteArray::fromStdString(encoded).toHex().toStdString() + '>';
    std::map<std::string, std::string> extra;
    if (!key.isEmpty())
        extra[key.toStdString()] = value.toStdString();
    const QString in = p("foreign.pdf"), out = p("foreign-rotated.pdf");
    makeBlobPdf(in, encoded, QPDFObjectHandle::parse(filter.toStdString()), {}, extra);

    const auto blob = MeasureExport::readMervinBlob(in);
    QCOMPARE(blob.has_value(), accepted);
    if (accepted)
        QCOMPARE(blob->trimmed(), QByteArray::fromStdString(json));
    RenderEngine engine;
    const auto document = engine.openDocument(in);
    QVERIFY(document);
    QVERIFY(document->mervinMeasurements() == blob);
    QString err;
    QCOMPARE(PageOps::rotatePages(in, out, {0}, 90, true, {}, &err),
             accepted ? PageOps::Status::Ok : PageOps::Status::Failed);
}

void TstMeasureExport::mergeRefusesMeasurementsPastTheCap_data()
{
    QTest::addColumn<qsizetype>("labelLength"); // per source
    QTest::addColumn<bool>("accepted");
    // Each source's blob stays below the cap, so both open with their measurements.
    QTest::newRow("together past the cap") << kMaxMeasurementBlobBytes / 2 + 1024 << false;
    QTest::newRow("together under the cap") << kMaxMeasurementBlobBytes / 2 - 64 * 1024 << true;
}

/// Merge writes no output whose measurements its readers would refuse, so they are never
/// lost on the next open.
void TstMeasureExport::mergeRefusesMeasurementsPastTheCap()
{
    QFETCH(qsizetype, labelLength);
    QFETCH(bool, accepted);
    const QString plain = p("cap-plain.pdf"), first = p("cap-first.pdf"),
                  second = p("cap-second.pdf"), merged = p("cap-merged.pdf");
    QFile::remove(merged);
    makePdf(plain, 1);
    QString err;
    QCOMPARE(MeasureExport::embedMervin(plain, first, longLabel(labelLength), {}, &err),
             MeasureExport::Status::Ok);
    QCOMPARE(MeasureExport::embedMervin(plain, second, longLabel(labelLength), {}, &err),
             MeasureExport::Status::Ok);
    QVERIFY(MeasureExport::readMervinBlob(first).has_value());

    const auto status = PageOps::merge({{first, {}, {}}, {second, {}, {}}}, merged, &err);
    if (!accepted) {
        QCOMPARE(status, PageOps::Status::Failed);
        QCOMPARE(err, tooLargeReason());
        QVERIFY(!QFileInfo::exists(merged));
        return;
    }
    QCOMPARE(status, PageOps::Status::Ok);
    RenderEngine engine;
    const auto document = engine.openDocument(merged);
    QVERIFY(document);
    const auto blob = document->mervinMeasurements();
    QVERIFY(blob.has_value());
    MeasureDoc back;
    QVERIFY(parseMeasurements(*blob, &back));
    QCOMPARE(int(back.pageScales.size()), 2);
    QCOMPARE(back.pageScales.back().page, 1);
    QCOMPARE(back.pageScales.back().label.size(), labelLength);
}

/// Saving measurements that would pass the read limit fails before anything is written.
void TstMeasureExport::saveRefusesMeasurementsPastTheCap()
{
    const QString plain = p("cap-save-plain.pdf"), saved = p("cap-save.pdf");
    makePdf(plain, 1);
    QString err;
    QCOMPARE(MeasureExport::embedMervin(plain, saved, longLabel(kMaxMeasurementBlobBytes), {}, &err),
             MeasureExport::Status::Failed);
    QCOMPARE(err, tooLargeReason());
    QVERIFY(!QFileInfo::exists(saved));
}

void TstMeasureExport::openReaderMatchesQpdf()
{
    // Opening reads saved measurements through MuPDF (Document::mervinMeasurements),
    // while page operations read them with qpdf. A disagreement here would silently drop
    // a document's saved measurements and calibration when it opens, so tie the
    // MuPDF reader to what qpdf actually finds.
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
    QVERIFY(!before->mervinMeasurements());

    auto after = engine.openDocument(embedded, QString(), &err, &needsPw);
    QVERIFY2(after != nullptr, qPrintable(err));
    const auto stored = MeasureExport::readMervinBlob(embedded);
    QVERIFY(stored.has_value());
    QVERIFY(after->mervinMeasurements() == stored);
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

void TstMeasureExport::valueLabelsWithoutScale()
{
    const MeasureScale none; // no scale: values fall back to the size on paper
    const MeasureUnit mm = MeasureUnit::Millimeter;
    const std::vector<QPointF> inch = {QPointF(0, 0), QPointF(72, 0)};
    const std::vector<QPointF> square = {QPointF(0, 0), QPointF(72, 0), QPointF(72, 72),
                                         QPointF(0, 72)};
    const std::vector<QPointF> rightAngle = {QPointF(72, 0), QPointF(0, 0), QPointF(0, 72)};

    for (const MeasureLabelText text : {MeasureLabelText::Ui, MeasureLabelText::Pdf}) {
        QCOMPARE(formatMeasurementValue(MeasureKind::Distance, inch, none, mm, 2, text),
                 QStringLiteral("25.40 mm (paper)"));
        // Area above, perimeter below; the note ends the last line.
        QCOMPARE(formatMeasurementValue(MeasureKind::Area, square, none, mm, 2, text),
                 QString::fromUtf8("645.16 mm\xC2\xB2\n101.60 mm (paper)"));
        // An angle does not depend on the scale, so it carries no note.
        QCOMPARE(formatMeasurementValue(MeasureKind::Angle, rightAngle, none, mm, 1, text),
                 QString::fromUtf8("90.0\xC2\xB0"));
        QVERIFY(formatMeasurementValue(MeasureKind::Distance, {QPointF(0, 0)}, none, mm, 2, text)
                    .isEmpty());
    }

    // A translation reaches the screen text only. The default (what the export path
    // uses) stays English: the label font covers Latin-1 only.
    PaperNoteTranslator translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    const QString ui =
        formatMeasurementValue(MeasureKind::Distance, inch, none, mm, 2, MeasureLabelText::Ui);
    const QString pdf = formatMeasurementValue(MeasureKind::Distance, inch, none, mm, 2);
    QCoreApplication::removeTranslator(&translator);
    QCOMPARE(ui, QStringLiteral("25.40 mm (translated)"));
    QCOMPARE(pdf, QStringLiteral("25.40 mm (paper)"));
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
    auto blob = MeasureExport::readMervinBlob(emb);
    QVERIFY(blob.has_value());
    QVERIFY(edoc->mervinMeasurements() == blob); // the open-time reader agrees on a real file
    MeasureDoc back;
    QVERIFY(parseMeasurements(*blob, &back));
    QCOMPARE(static_cast<int>(back.measurements.size()), 1);
#endif
}

QTEST_MAIN(TstMeasureExport)
#include "tst_measure_export.moc"
