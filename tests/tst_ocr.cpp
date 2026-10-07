#include "ocr/TessdataFile.h"
#include "render/Document.h"
#include "render/OcrService.h"
#include "render/RenderEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTest>

#include <memory>

using mervin::OcrService;
using mervin::RenderEngine;

// OCR drives MuPDF's bundled Tesseract over a re-rendered region. CMake generates
// the PDF fixture and fetch-test-tessdata.py supplies a pinned test-only model.
class TstOcr : public QObject
{
    Q_OBJECT

private slots:
    void recognizesTextOnFirstPage();
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

} // namespace

void TstOcr::recognizesTextOnFirstPage()
{
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
        ocr.recognize(doc.get(), 0, whole, {QStringLiteral("eng")}, tessdata, &err);
    QVERIFY2(!text.isEmpty(), qPrintable(QStringLiteral("OCR returned no text: %1").arg(err)));

    // The MIL-STD test PDF's cover page has large, clear text; OCR of the
    // 300-DPI render should recover at least one of these words. (Loose to
    // tolerate OCR variance while still proving the pipeline works.)
    const QString up = text.toUpper();
    const bool found = up.contains(QStringLiteral("DEPARTMENT")) || up.contains(QStringLiteral("DEFENSE"))
                       || up.contains(QStringLiteral("STANDARD")) || up.contains(QStringLiteral("TEST"));
    QVERIFY2(found, qPrintable(QStringLiteral("unexpected OCR text: %1").arg(text.left(200))));
}

QTEST_GUILESS_MAIN(TstOcr)
#include "tst_ocr.moc"
