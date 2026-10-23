#include "ocr/TessdataFile.h"
#include "render/Document.h"
#include "render/OcrService.h"
#include "render/RenderEngine.h"
#include "render/TextIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <QSemaphore>
#include <QImage>

#include <memory>
#include <thread>

using mervin::OcrService;
using mervin::RenderEngine;

// OCR drives MuPDF's bundled Tesseract over a re-rendered region. CMake generates
// the PDF fixture and fetch-test-tessdata.py supplies a pinned test-only model.
class TstOcr : public QObject
{
    Q_OBJECT

private slots:
    void recognizesTextOnFirstPage_data();
    void recognizesTextOnFirstPage();
    void rejectsInvalidDefaultModel_data();
    void rejectsInvalidDefaultModel();
    void twoRecognitionsAtOnceBothFinish();
    void recognizesOnlySelectedImageRegion();
    void invalidAndCanceledRequests();
};

namespace {

// Use test data only. Never fall back to an installed application's profile.
QString tessdataDirForTest()
{
    const QString override = qEnvironmentVariable("MERVIN_TEST_TESSDATA_DIR");
    if (!override.isEmpty())
        return override;
    return QString::fromUtf8(MERVIN_TEST_TESSDATA_DIR);
}

// Assemble a self-contained PDF for controlled image/text region tests.
QByteArray pdfObjects(const QList<QByteArray> &objects)
{
    QByteArray pdf("%PDF-1.7\n");
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

// Write authored bytes and report a failed write to the calling test.
bool writePdf(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

/// Exercises explicit and default English through the same recognition contract.
void TstOcr::recognizesTextOnFirstPage_data()
{
    QTest::addColumn<QStringList>("languages");
    QTest::newRow("explicit English") << QStringList{QStringLiteral("eng")};
    QTest::newRow("default English") << QStringList{};
}

void TstOcr::recognizesTextOnFirstPage()
{
    QFETCH(QStringList, languages);
    QByteArray pdf = qgetenv("MERVIN_TEST_PDF");
#ifdef MERVIN_OCR_PDF
    if (pdf.isEmpty())
        pdf = QByteArray(MERVIN_OCR_PDF);
#endif
    if (pdf.isEmpty())
        QSKIP("set MERVIN_TEST_PDF to a text PDF to run the OCR test");
    if (!QFileInfo::exists(QString::fromUtf8(pdf)))
        QSKIP("the OCR test document is not present in this tree");

    const QString tessdata = tessdataDirForTest();
    const QString model = QDir(tessdata).filePath(QStringLiteral("eng.traineddata"));
    QVERIFY2(QFileInfo::exists(model),
             "Run scripts/fetch-test-tessdata.py and set MERVIN_TEST_TESSDATA_DIR to its output directory");
    QString modelError;
    QVERIFY2(mervin::TessdataFile::validate(model, &modelError), qPrintable(modelError));

    RenderEngine engine;
    QString err;
    std::unique_ptr<mervin::Document> doc =
        engine.openDocument(QString::fromUtf8(pdf), QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(doc->pageCount() > 0);

    const QSizeF sz = doc->pageSize(0);
    const QRectF whole(0, 0, sz.width(), sz.height());

    OcrService ocr(&engine);
    const QString text =
        ocr.recognize(doc.get(), 0, whole, languages, tessdata, &err);
    QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("OCR returned no text: %1").arg(err)));

    // The generated page has large STANDARD TEST text. OCR re-rasterizes the
    // page, so this establishes recognition but not image-only source input.
    const QString up = text.toUpper();
    const bool found = up.contains(QStringLiteral("DEPARTMENT")) || up.contains(QStringLiteral("DEFENSE"))
                       || up.contains(QStringLiteral("STANDARD")) || up.contains(QStringLiteral("TEST"));
    QVERIFY2(found, qPrintable(QStringLiteral("unexpected OCR text: %1").arg(text.left(200))));
}

/// Covers damaged models for explicit English and both implicit default forms.
void TstOcr::rejectsInvalidDefaultModel_data()
{
    QTest::addColumn<QStringList>("languages");
    QTest::newRow("explicit English") << QStringList{QStringLiteral("eng")};
    QTest::newRow("default English") << QStringList{};
    QTest::newRow("blank default English") << QStringList{QString()};
}

/// Rejects bad default language data before invoking native OCR initialization.
void TstOcr::rejectsInvalidDefaultModel()
{
    QFETCH(QStringList, languages);
    QTemporaryDir models;
    QVERIFY(models.isValid());
    {
        QFile model(models.filePath(QStringLiteral("eng.traineddata")));
        QVERIFY(model.open(QIODevice::WriteOnly));
        QCOMPARE(model.write("invalid model"), qint64(13));
    }
    QString expected;
    QVERIFY(!mervin::TessdataFile::validateLanguages(
        models.path(), {QStringLiteral("eng")}, &expected));
    QVERIFY(!expected.isEmpty());
    RenderEngine engine;
    QString error;
    auto doc = engine.openDocument(QStringLiteral(MERVIN_OCR_PDF), {}, &error);
    QVERIFY2(doc, qPrintable(error));
    OcrService ocr(&engine);
    QVERIFY(ocr.recognize(doc.get(), 0, QRectF(QPointF(), doc->pageSize(0)),
                         languages, models.path(), &error).isEmpty());
    QCOMPARE(error, expected);
}

// A canceled recognition can still be running when the next one starts, each on
// its own background thread and service. MuPDF's Tesseract allows one at a time
// in a process, so the second used to fail ("Attempt to use Leptonica from 2
// threads at once!"); now it waits its turn.
void TstOcr::twoRecognitionsAtOnceBothFinish()
{
    QByteArray pdf = qgetenv("MERVIN_TEST_PDF");
#ifdef MERVIN_OCR_PDF
    if (pdf.isEmpty())
        pdf = QByteArray(MERVIN_OCR_PDF);
#endif
    if (pdf.isEmpty())
        QSKIP("set MERVIN_TEST_PDF to a text PDF to run the OCR test");
    const QString tessdata = tessdataDirForTest();
    QVERIFY(QFileInfo::exists(QDir(tessdata).filePath(QStringLiteral("eng.traineddata"))));

    RenderEngine engine;
    QString err;
    std::unique_ptr<mervin::Document> doc =
        engine.openDocument(QString::fromUtf8(pdf), QString(), &err);
    QVERIFY2(doc != nullptr, qPrintable(err));
    const QSizeF sz = doc->pageSize(0);
    const QRectF whole(0, 0, sz.width(), sz.height());

    OcrService first(&engine);
    OcrService second(&engine);
    QString texts[2];
    QString errors[2];
    QSemaphore nativeReady;
    bool observedNativeProgress = false;
    QSemaphore secondWaiting;
    QSemaphore releaseFirst;
    OcrService::testing::observe(first, [&](auto phase) {
        if (phase == OcrService::testing::Phase::NativeProgress && !observedNativeProgress) {
            observedNativeProgress = true;
            nativeReady.release();
            releaseFirst.tryAcquire(1, 10000);
        }
    });
    OcrService::testing::observe(second, [&](auto phase) {
        if (phase == OcrService::testing::Phase::WaitingForTurn)
            secondWaiting.release();
    });
    std::jthread a([&] {
        texts[0] = first.recognize(doc.get(), 0, whole, {QStringLiteral("eng")}, tessdata, &errors[0]);
    });
    const auto release = qScopeGuard([&] { releaseFirst.release(); });
    QVERIFY(nativeReady.tryAcquire(1, 5000));
    std::jthread b([&] {
        texts[1] = second.recognize(doc.get(), 0, whole, {QStringLiteral("eng")}, tessdata, &errors[1]);
    });
    const bool overlapped = secondWaiting.tryAcquire(1, 5000);
    releaseFirst.release();
    a.join();
    b.join();
    QVERIFY(overlapped); // the second request reached native admission while the first owned it
    for (int i = 0; i < 2; ++i)
    {
        QVERIFY2(errors[i].isEmpty(), qPrintable(errors[i]));
        QVERIFY2(texts[i].contains(QStringLiteral("STANDARD"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("run %1 returned: %2").arg(i).arg(texts[i])));
    }
}

// Rasterize the authored words into an image-only PDF with a shifted PDF media box.
void TstOcr::recognizesOnlySelectedImageRegion()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QByteArray content = "BT /F1 36 Tf 150 500 Td (SELECTED ORCHARD) Tj ET "
                               "BT /F1 36 Tf 150 300 Td (EXCLUDED BANANA) Tj ET";
    QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 700 600] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    const QString source = files.filePath("text.pdf");
    QVERIFY(writePdf(source, pdfObjects(objects)));
    RenderEngine engine;
    auto textDoc = engine.openDocument(source);
    QVERIFY(textDoc);
    const QImage raster = engine.renderPageImage(textDoc.get(), 0, 2.0, 0)
                              .convertToFormat(QImage::Format_RGB888);
    QVERIFY(!raster.isNull());
    QByteArray pixels;
    for (int row = 0; row < raster.height(); ++row)
        pixels.append(reinterpret_cast<const char *>(raster.constScanLine(row)), raster.width() * 3);
    const QByteArray compressed = qCompress(pixels).mid(4);
    const QByteArray drawImage("q 600 0 0 400 100 200 cm /Im0 Do Q");
    objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 700 600] "
                 "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>";
    objects[3] = "<< /Length " + QByteArray::number(drawImage.size()) + " >>\nstream\n"
                 + drawImage + "\nendstream";
    objects[4] = "<< /Type /XObject /Subtype /Image /Width " + QByteArray::number(raster.width())
                 + " /Height " + QByteArray::number(raster.height())
                 + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length "
                 + QByteArray::number(compressed.size()) + " >>\nstream\n" + compressed + "\nendstream";
    const QString imagePath = files.filePath("image.pdf");
    QVERIFY(writePdf(imagePath, pdfObjects(objects)));
    auto imageDoc = engine.openDocument(imagePath);
    QVERIFY(imageDoc);
    mervin::TextIndex index(engine.baseContext(), imageDoc.get());
    QVERIFY(index.pageText(0).isEmpty()); // no hidden text can satisfy this recognition
    OcrService ocr(&engine);
    QString error;
    const QString whole = ocr.recognize(imageDoc.get(), 0, {0, 0, 600, 400},
                                       {"eng"}, tessdataDirForTest(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(whole.contains("ORCHARD") && whole.contains("BANANA"), qPrintable(whole));
    const QString selected = ocr.recognize(imageDoc.get(), 0, {30, 50, 550, 100},
                                          {"eng"}, tessdataDirForTest(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(selected.contains("ORCHARD"), qPrintable(selected));
    QVERIFY2(!selected.contains("BANANA") && !selected.contains("EXCLUDED"), qPrintable(selected));

    // Original PDF text outside the selection must not leak through the OCR device either.
    const QByteArray mixed = drawImage + " BT /F1 24 Tf 150 230 Td (OUTSIDE VECTOR) Tj ET";
    objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [100 200 700 600] "
                 "/Resources << /XObject << /Im0 5 0 R >> /Font << /F1 6 0 R >> >> /Contents 4 0 R >>";
    objects[3] = "<< /Length " + QByteArray::number(mixed.size()) + " >>\nstream\n" + mixed + "\nendstream";
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const QString mixedPath = files.filePath("mixed.pdf");
    QVERIFY(writePdf(mixedPath, pdfObjects(objects)));
    auto mixedDoc = engine.openDocument(mixedPath);
    QVERIFY(mixedDoc);
    mervin::TextIndex mixedIndex(engine.baseContext(), mixedDoc.get());
    QVERIFY(mixedIndex.pageText(0).contains("OUTSIDE VECTOR"));
    const QString mixedSelection = ocr.recognize(mixedDoc.get(), 0, {30, 50, 550, 100},
                                                {"eng"}, tessdataDirForTest(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(mixedSelection.contains("ORCHARD"), qPrintable(mixedSelection));
    QVERIFY2(!mixedSelection.contains("VECTOR") && !mixedSelection.contains("BANANA"),
             qPrintable(mixedSelection));
}

// Invalid requests fail cleanly; cancellation and dead lifetimes return no text or stale error.
void TstOcr::invalidAndCanceledRequests()
{
    RenderEngine engine;
    auto document = engine.openDocument(QStringLiteral(MERVIN_OCR_PDF));
    QVERIFY(document);
    OcrService ocr(&engine);
    QString error("stale error");
    QVERIFY(ocr.recognize(nullptr, 0, {0, 0, 100, 100}, {}, {}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Invalid OCR request."));
    QVERIFY(ocr.recognize(document.get(), 0, {}, {}, {}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Invalid OCR request."));
    OcrService noEngine(nullptr);
    QVERIFY(noEngine.recognize(document.get(), 0, {0, 0, 100, 100}, {}, {}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Invalid OCR request."));
    QVERIFY(ocr.recognize(document.get(), -1, {0, 0, 100, 100}, {"eng"},
                         tessdataDirForTest(), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    auto lifetime = document->lifetime();
    std::atomic<bool> canceled{true};
    error = QStringLiteral("stale error");
    QVERIFY(ocr.recognize(lifetime, 0, {0, 0, 100, 100}, {}, {}, &error, &canceled).isEmpty());
    QVERIFY(error.isEmpty());
    document.reset();
    error = QStringLiteral("stale error");
    QVERIFY(ocr.recognize(lifetime, 0, {0, 0, 100, 100}, {"eng"},
                         tessdataDirForTest(), &error).isEmpty());
    QVERIFY(error.isEmpty());
}

QTEST_GUILESS_MAIN(TstOcr)
#include "tst_ocr.moc"
