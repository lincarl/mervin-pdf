#include "render/AnnotModel.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

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
    void failedReplacementPreservesBothFiles();
    void pageOperationsRemapMeasurements();
    void generatedScanRendersInComfortAndTiles();
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

static MeasureDoc readMarks(const QString &path)
{
    MeasureDoc data;
    if (auto blob = MeasureExport::readMervinBlob(path))
        parseMeasurements(*blob, &data);
    return data;
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
    QCOMPARE(readMarks(saved).measurements.size(), 1u);
    const QString cleared = dir.filePath("cleared.pdf");
    QVERIFY2(DocumentOutput::snapshot(*reopened, {}, cleared, {}, &error), qPrintable(error));
    QVERIFY(readMarks(cleared).measurements.empty());
    QVERIFY(readMarks(cleared).pageScales.empty());
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
    QCOMPARE(data.measurements.size(), 1u);
    QCOMPARE(data.measurements[0].page, 0);
    QCOMPARE(data.pageScales[0].page, 0);

    const QString extracted = dir.filePath("extracted.pdf");
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {1, 0, 1}, {}}}, extracted, &error),
             PageOps::Status::Ok);
    data = readMarks(extracted);
    QCOMPARE(data.measurements.size(), 2u);
    QCOMPARE(data.measurements[0].page, 0);
    QCOMPARE(data.measurements[1].page, 2);
    QCOMPARE(data.pageScales.size(), 2u);

    QStringList split;
    QCOMPARE(PageOps::split(source, dir.path(), "split", {}, &split, &error), PageOps::Status::Ok);
    QCOMPARE(readMarks(split[1]).measurements[0].page, 0);
    QVERIFY(readMarks(split[0]).measurements.empty());

    const QString rotated = dir.filePath("rotated.pdf");
    QCOMPARE(PageOps::rotatePages(source, rotated, {1}, 90, true, {}, &error), PageOps::Status::Ok);
    data = readMarks(rotated);
    QCOMPARE(data.measurements[0].pts[0], QPointF(772, 10));
    QCOMPARE(data.pageScales[0].mmPerPointX, 3.0);
    QCOMPARE(data.pageScales[0].mmPerPointY, 2.0);
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

QTEST_GUILESS_MAIN(TstDocumentOutput)
#include "tst_document_output.moc"
