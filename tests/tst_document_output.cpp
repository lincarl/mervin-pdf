#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace mervin;

class TstDocumentOutput : public QObject
{
    Q_OBJECT
private slots:
    void snapshotPreservesEditsAndMeasurementDeletion();
    void malformedMarksAreNotEmpty();
    void failedReplacementPreservesBothFiles();
    void pageOperationsRemapMeasurements();
    void generatedScanRendersInComfortAndTiles();
    void embeddedPicturesKeepTheirTreatment_data();
    void embeddedPicturesKeepTheirTreatment();
    void flattenPreservesInheritedResources();
};

void TstDocumentOutput::flattenPreservesInheritedResources()
{
    QTemporaryDir dir;
    const QString source = QStringLiteral(MERVIN_INHERITED_PDF);
    const QString output = dir.filePath("flattened.pdf");
    RenderMeasurement mark;
    mark.page = 0;
    mark.pts = {{30, 30}, {100, 30}};
    mark.label = QStringLiteral("70 mm");
    QCOMPARE(MeasureExport::flatten(source, output, {mark}, {}), MeasureExport::Status::Ok);
    QPDF pdf;
    const QByteArray name = output.toUtf8();
    pdf.processFile(name.constData());
    const auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    auto page = pages[0];
    auto fonts = page.getAttribute("/Resources", false).getKey("/Font");
    QCOMPARE(fonts.getKey("/F1").getKey("/BaseFont").getName(), std::string("/Courier"));
    QCOMPARE(fonts.getKey("/Fluc").getKey("/BaseFont").getName(), std::string("/Courier"));
    QCOMPARE(fonts.getKey("/Fluc1").getKey("/BaseFont").getName(), std::string("/Helvetica"));
    page = pages[1];
    QVERIFY(!page.getAttribute("/Resources", false).getKey("/Font").hasKey("/Fluc1"));
    RenderEngine engine;
    auto before = engine.openDocument(source);
    auto after = engine.openDocument(output);
    QVERIFY(before && after);
    const QImage original = engine.renderPageImage(before.get(), 0, 1, 0);
    const QImage flattened = engine.renderPageImage(after.get(), 0, 1, 0);
    QVERIFY(!original.isNull() && !flattened.isNull());
    QCOMPARE(flattened.copy(0, 0, 612, 200), original.copy(0, 0, 612, 200));
    QVERIFY(flattened != original); // the label is present, below the original text
}

static MeasureDoc marks()
{
    MeasureDoc data;
    data.measurements.push_back({1, MeasureKind::Distance, {{10, 20}, {30, 40}}});
    data.pageScales.push_back({1, 2.0, 3.0, QStringLiteral("custom"), MeasureSource::Manual});
    return data;
}

// Absence means unreadable data, while a valid empty document means no marks.
static std::optional<MeasureDoc> readMarks(const QString &path)
{
    RenderEngine engine;
    auto doc = engine.openDocument(path);
    if (!doc)
        return std::nullopt;
    const auto blob = doc->mervinMeasurements();
    MeasureDoc data;
    if (doc->measurementsUnreadable() || (blob && !parseMeasurements(*blob, &data)))
        return std::nullopt;
    return data;
}

void TstDocumentOutput::malformedMarksAreNotEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QPDF pdf;
    pdf.processFile(MERVIN_FIXTURE_PDF);
    pdf.getRoot().replaceKey("/Mervin_Measurements", pdf.newStream("not JSON"));
    const QString path = dir.filePath(QStringLiteral("malformed.pdf"));
    const QByteArray encoded = path.toUtf8();
    QPDFWriter writer(pdf, encoded.constData());
    writer.write();
    QVERIFY(!readMarks(path));
    const auto absent = readMarks(QStringLiteral(MERVIN_FIXTURE_PDF));
    QVERIFY(absent);
    QVERIFY(absent->measurements.empty());
}

void TstDocumentOutput::snapshotPreservesEditsAndMeasurementDeletion()
{
    QTemporaryDir dir;
    RenderEngine engine;
    auto doc = engine.openDocument(QStringLiteral(MERVIN_FIXTURE_PDF));
    QVERIFY(doc);
    AnnotModel annotations(*doc);
    QVERIFY(annotations.addTextNote(0, {50, 50}, Qt::yellow, {}, QStringLiteral("Unsaved note")) >= 0);
    QString error;
    const QString saved = dir.filePath("saved.pdf");
    QVERIFY2(DocumentOutput::snapshot(*doc, marks(), saved, {}, &error), qPrintable(error));
    auto reopened = engine.openDocument(saved);
    QVERIFY(reopened);
    AnnotModel loaded(*reopened);
    QCOMPARE(loaded.pageAnnots(0).size(), 1u);
    QCOMPARE(loaded.pageAnnots(0)[0].contents, QStringLiteral("Unsaved note"));
    const auto savedMarks = readMarks(saved);
    QVERIFY(savedMarks);
    QCOMPARE(savedMarks->measurements.size(), 1u);
    const QString cleared = dir.filePath("cleared.pdf");
    QVERIFY2(DocumentOutput::snapshot(*reopened, {}, cleared, {}, &error), qPrintable(error));
    const auto clearedMarks = readMarks(cleared);
    QVERIFY(clearedMarks);
    QVERIFY(clearedMarks->measurements.empty());
    QVERIFY(clearedMarks->pageScales.empty());
    auto empty = engine.openDocument(cleared);
    QVERIFY(empty);
    QCOMPARE(AnnotModel(*empty).pageAnnots(0).size(), 1u);
}

void TstDocumentOutput::failedReplacementPreservesBothFiles()
{
    QTemporaryDir dir;
    const QString original = dir.filePath("original.pdf");
    QVERIFY(QFile::copy(QStringLiteral(MERVIN_FIXTURE_PDF), original));
    QFile file(original);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll();
    file.close();
    QString error;
    QVERIFY(!DocumentOutput::replace(dir.filePath("missing.pdf"), original, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    QVERIFY(!DocumentOutput::replace(original, dir.path(), &error));
    QVERIFY(QFile::exists(original));
}

void TstDocumentOutput::pageOperationsRemapMeasurements()
{
    QTemporaryDir dir;
    QString error;
    const QString source = dir.filePath("marks.pdf");
    QCOMPARE(MeasureExport::embedMervin(QStringLiteral(MERVIN_FIXTURE_PDF), source, marks(), {}, &error),
             MeasureExport::Status::Ok);
    const QString deleted = dir.filePath("deleted.pdf");
    QCOMPARE(PageOps::deletePages(source, deleted, {0}, {}, &error), PageOps::Status::Ok);
    auto data = readMarks(deleted);
    QVERIFY(data);
    QCOMPARE(data->measurements.size(), 1u);
    QCOMPARE(data->measurements[0].page, 0);
    QCOMPARE(data->pageScales.size(), 1u);
    QCOMPARE(data->pageScales[0].page, 0);

    const QString extracted = dir.filePath("extracted.pdf");
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {1, 0, 1}, {}}}, extracted, &error),
             PageOps::Status::Ok);
    data = readMarks(extracted);
    QVERIFY(data);
    QCOMPARE(data->measurements.size(), 2u);
    QCOMPARE(data->measurements[0].page, 0);
    QCOMPARE(data->measurements[1].page, 2);
    QCOMPARE(data->pageScales.size(), 2u);

    QStringList split;
    QCOMPARE(PageOps::split(source, dir.path(), "split", {}, &split, &error), PageOps::Status::Ok);
    QCOMPARE(split.size(), 4);
    const auto markedSplit = readMarks(split[1]);
    QVERIFY(markedSplit);
    QCOMPARE(markedSplit->measurements.size(), 1u);
    QCOMPARE(markedSplit->measurements[0].page, 0);
    const auto emptySplit = readMarks(split[0]);
    QVERIFY(emptySplit);
    QVERIFY(emptySplit->measurements.empty());

    const QString rotated = dir.filePath("rotated.pdf");
    QCOMPARE(PageOps::rotatePages(source, rotated, {1}, 90, true, {}, &error), PageOps::Status::Ok);
    data = readMarks(rotated);
    QVERIFY(data);
    QCOMPARE(data->measurements.size(), 1u);
    QCOMPARE(data->measurements[0].pts.size(), 2u);
    QCOMPARE(data->measurements[0].pts[0], QPointF(772, 10));
    QCOMPARE(data->pageScales.size(), 1u);
    QCOMPARE(data->pageScales[0].mmPerPointX, 3.0);
    QCOMPARE(data->pageScales[0].mmPerPointY, 2.0);
}

void TstDocumentOutput::generatedScanRendersInComfortAndTiles()
{
    RenderEngine engine;
    auto doc = engine.openDocument(QStringLiteral(MERVIN_SCAN_PDF));
    QVERIFY(doc);
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    RenderRequest request;
    request.document = doc.get();
    request.requester = 77;
    request.theme = PageTheme::Comfort;
    engine.submit(request);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 10000);
    const auto full = results.takeFirst()[0].value<RenderResult>();
    QVERIFY(full.ok);
    QVERIFY(full.image.pixelColor(15, 15).lightness() < 100); // paper becomes dark
    QVERIFY(full.image.pixelColor(1, 15).lightness() > 150); // black scan lines become light
    request.clip = QRect(60, 90, 180, 210);
    engine.submit(request);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 10000);
    const auto tile = results.takeFirst()[0].value<RenderResult>();
    QVERIFY(tile.ok);
    QCOMPARE(tile.image, full.image.copy(request.clip));
}

void TstDocumentOutput::embeddedPicturesKeepTheirTreatment_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("rotation");
    for (const char *kind : {"photo", "authored", "soft-mask", "image-rotation", "text-below", "text-above"})
        QTest::newRow(kind) << QString::fromLatin1(kind) << 0;
    QTest::newRow("page-rotation") << QStringLiteral("photo") << 90;
}

// Source pixels form a smooth shaded subject. The image occupies 10% of a page
// with a nonzero origin, so treating it as a scan or missing its placement fails.
void TstDocumentOutput::embeddedPicturesKeepTheirTreatment()
{
    QFETCH(QString, kind);
    QFETCH(int, rotation);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const bool mask = kind == QLatin1String("soft-mask");
    const bool authored = kind == QLatin1String("authored");
    const bool turned = kind == QLatin1String("image-rotation");
    QByteArray pixels;
    QByteArray alpha;
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            const bool subject = x >= 16 && x < 80 && y >= 16 && y < 80;
            const char value = char(subject || authored ? 60 + 2 * x : mask ? 0 : 255);
            pixels.append(QByteArray(3, value));
            alpha.append(char(subject ? 255 : 0));
        }
    }
    const auto stream = [](const QByteArray &attributes, const QByteArray &data) {
        return "<< " + attributes + " /Length " + QByteArray::number(data.size())
            + " >>\nstream\n" + data + "\nendstream";
    };
    const QByteArray text = "0 g BT /F1 20 Tf 120 203 Td (H) Tj ET\n";
    QByteArray content;
    if (kind == QLatin1String("text-below")) content += text;
    content += turned ? "q 0 120 -100 0 237 93 cm /Im0 Do Q\n"
                      : "q 120 0 0 100 77 173 cm /Im0 Do Q\n";
    if (kind == QLatin1String("text-above")) content += text;
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [37 53 437 353] "
        "/Resources << /XObject << /Im0 6 0 R >> /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream({}, content),
        stream(QByteArray("/Type /XObject /Subtype /Image /Width 96 /Height 96 "
                          "/BitsPerComponent 8 /ColorSpace /DeviceRGB")
                   + (mask ? " /SMask 7 0 R" : ""), pixels),
        stream("/Type /XObject /Subtype /Image /Width 96 /Height 96 "
               "/BitsPerComponent 8 /ColorSpace /DeviceGray", alpha),
    };
    QByteArray pdf = "%PDF-1.7\n";
    QByteArray xref = "0000000000 65535 f \n";
    for (qsizetype i = 0; i < objects.size(); ++i) {
        xref += QByteArray::number(pdf.size()).rightJustified(10, '0') + " 00000 n \n";
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto start = QByteArray::number(pdf.size());
    pdf += "xref\n0 8\n" + xref + "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n" + start + "\n%%EOF\n";
    const QString path = dir.filePath(QStringLiteral("picture.pdf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(pdf), pdf.size());
    file.close();
    RenderEngine engine;
    auto doc = engine.openDocument(path);
    QVERIFY(doc);
    QCOMPARE(doc->pageSize(0), QSizeF(400, 300));
    const QImage light = engine.renderPageImage(doc.get(), 0, 1, rotation);
    QVERIFY(!light.isNull());
    const QPoint subject = rotation ? QPoint(170, 100) : turned ? QPoint(150, 200) : QPoint(100, 130);
    const QPoint surround = rotation ? QPoint(215, 45) : turned ? QPoint(105, 145) : QPoint(45, 85);
    const QColor subjectColor = light.pixelColor(subject);
    QVERIFY(subjectColor.red() >= 145 && subjectColor.red() <= 165);
    if (!authored) QCOMPARE(light.pixelColor(surround), QColor(Qt::white));
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    RenderRequest request;
    request.document = doc.get();
    request.requester = 91;
    request.rotation = rotation;
    request.theme = PageTheme::Comfort;
    engine.submit(request);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 10000);
    const auto result = results.takeFirst().front().value<RenderResult>();
    QVERIFY2(result.ok, qPrintable(result.error));
    const QImage comfort = result.image;
    QCOMPARE(comfort.pixelColor(subject), subjectColor);
    QCOMPARE(comfort.pixelColor(10, 10), QColor(24, 26, 30));
    QCOMPARE(comfort.pixelColor(surround), authored ? light.pixelColor(surround) : QColor(24, 26, 30));
    if (kind == QLatin1String("text-below") || kind == QLatin1String("text-above")) {
        int glyphs = 0;
        for (int y = 130; y < 151; ++y) {
            for (int x = 83; x < 98; ++x) {
                if (light.pixelColor(x, y) == QColor(Qt::black)) {
                    ++glyphs;
                    QCOMPARE(comfort.pixelColor(x, y), QColor(214, 217, 222));
                } else if (kind == QLatin1String("text-below")) {
                    QCOMPARE(comfort.pixelColor(x, y), light.pixelColor(x, y));
                }
            }
        }
        if (kind == QLatin1String("text-above")) QVERIFY(glyphs > 20);
        else QCOMPARE(glyphs, 0);
    }
    request.clip = QRect(subject - QPoint(20, 15), QSize(40, 30));
    engine.submit(request);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 10000);
    const auto tile = results.takeFirst().front().value<RenderResult>();
    QVERIFY2(tile.ok, qPrintable(tile.error));
    QCOMPARE(tile.image, comfort.copy(request.clip));
}

QTEST_GUILESS_MAIN(TstDocumentOutput)
#include "tst_document_output.moc"
