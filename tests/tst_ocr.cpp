#include "ocr/TessdataManager.h"
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

// OCR drives MuPDF's bundled Tesseract over a re-rendered region, so a real
// test needs both a PDF and a language model. The language model ships in the
// repository; the PDF is an optional local fixture, and MERVIN_TEST_PDF can
// override it.
//
// It used to QSKIP unless MERVIN_TEST_PDF was set, which is how a corrupt
// shipped eng.traineddata went unnoticed for six weeks - see tst_tessdata.
class TstOcr : public QObject
{
    Q_OBJECT

private slots:
    void recognizesTextOnFirstPage();
};

namespace {

// The tessdata folder to OCR against: prefer the one in the source tree, so the
// test exercises the data we actually ship rather than whatever the developer
// happens to have installed. Falls back to the user's folder for trees that
// strip the model (some Linux source packages do).
QString tessdataDirForTest()
{
#ifdef MERVIN_TESSDATA_DIR
    const QString shipped = QString::fromUtf8(MERVIN_TESSDATA_DIR);
    if (QFileInfo::exists(QDir(shipped).filePath(QStringLiteral("eng.traineddata"))))
        return shipped;
#endif
    if (mervin::TessdataManager::installedLanguages().contains(QStringLiteral("eng")))
        return mervin::TessdataManager::directory();
    return {};
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
    if (tessdata.isEmpty())
        QSKIP("no eng.traineddata in the source tree or the tessdata folder");

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
