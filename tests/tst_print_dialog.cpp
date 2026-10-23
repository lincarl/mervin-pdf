#include "dialogs/PrintDialog.h"
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace mervin;

Q_DECLARE_METATYPE(PrintDialog::PaperCapabilities)

namespace {

QColor centreColour(PrintPreviewWidget *preview)
{
    const QImage image = preview->grab().toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

QRect matchingBounds(const QImage &image, bool paper)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const bool match = paper ? pixel.red() > 250 && pixel.green() > 250 && pixel.blue() > 250
                                     : pixel.red() > 180 && pixel.green() < 80 && pixel.blue() < 80;
            if (match)
                bounds |= QRect(x, y, 1, 1);
        }
    }
    return bounds;
}

// Where the preview should draw the fixture's red region, the left 75 x 160 points of a
// page, for a page whose top-left corner is at `origin` on the sheet, in points, and
// that prints at `scale` times its size. `paper` is the sheet's bounds in the preview.
QRectF expectedRed(const QRect &paper, const QPageLayout &layout, const QPointF &origin,
                   double scale)
{
    const double perPoint = paper.width() / layout.fullRect(QPageLayout::Point).width();
    return {QPointF(paper.topLeft()) + origin * perPoint, QSizeF(75, 160) * scale * perPoint};
}

// Whether the preview draws the red region within two pixels of `expected` on every
// edge. Wait for the whole rectangle, never one edge or one dimension of it. A later
// render of an unchanged placement can move one edge by a pixel, and the dialog can
// apply several changed settings in one refresh. `found` describes the bounds drawn.
bool redPlacedAt(PrintPreviewWidget *preview, const QRectF &expected, QString *found)
{
    const QRect bounds = matchingBounds(preview->grab().toImage(), false);
    *found = QStringLiteral("red drawn at %1,%2 %3 x %4, expected %5,%6 %7 x %8")
                 .arg(bounds.left()).arg(bounds.top()).arg(bounds.width()).arg(bounds.height())
                 .arg(expected.left(), 0, 'f', 1).arg(expected.top(), 0, 'f', 1)
                 .arg(expected.width(), 0, 'f', 1).arg(expected.height(), 0, 'f', 1);
    return !bounds.isEmpty() && std::abs(bounds.left() - expected.left()) <= 2
           && std::abs(bounds.top() - expected.top()) <= 2
           && std::abs(bounds.left() + bounds.width() - expected.right()) <= 2
           && std::abs(bounds.top() + bounds.height() - expected.bottom()) <= 2;
}

// Writes a one-page PDF with a complete cross-reference table and returns its path.
QString writePagePdf(const QTemporaryDir &dir, const QString &name, const QByteArray &page,
                     const QByteArray &content)
{
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R " + page + " /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content
            + "\nendstream",
    };
    QByteArray pdf = "%PDF-1.7\n";
    QByteArray xref = "0000000000 65535 f \n";
    for (qsizetype i = 0; i < objects.size(); ++i) {
        xref += QByteArray::number(pdf.size()).rightJustified(10, '0') + " 00000 n \n";
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const QByteArray start = QByteArray::number(pdf.size());
    const QByteArray size = QByteArray::number(objects.size() + 1);
    pdf += "xref\n0 " + size + "\n" + xref + "trailer\n<< /Size " + size
           + " /Root 1 0 R >>\nstartxref\n" + start + "\n%%EOF\n";
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(pdf) != pdf.size())
        return {};
    return path;
}

void configurePrinter(QPrinter &printer)
{
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageMargins(QMarginsF(18, 24, 30, 36), QPageLayout::Point);
}

// Observes retirement of the preview's queued and active render jobs after delivery.
bool previewRenderRetired(const RenderEngine &engine, const QSignalSpy &results)
{
    if (results.isEmpty())
        return false;
    const auto result = results.constLast().constFirst().value<RenderResult>();
    return RenderEngine::testing::pendingRequests(engine, result.requester) == 0;
}

} // namespace

class TstPrintDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void defaultsAndLiveSettings();
    void printerChangesSelectDefaultPaper();
    void printerPaperAliasesReuseEntries();
    void nativePaperCapabilities_data();
    void nativePaperCapabilities();
    void nativePaperSwitchAndPdfFallback();
    void systemHandoffKeepsSelectedPrinterDefault_data();
    void systemHandoffKeepsSelectedPrinterDefault();
    void customOrderValidationAndStaleResults();
    void sheetAlwaysFitsAfterResizeAndOrientation();
    void blackAndWhitePreviewConvertsOnce();
    void previewReportsAnUnrenderablePage();
    void previewPredictsAPrintRefusal();
    void previewShowsTheVisiblePartOfALargePage();
    void cancellingSaveKeepsDialogOpen();
    void staleSamePagePreviewCannotReplaceNewGeneration();

private:
    QTemporaryDir files_;
    QString sourcePath_;
};

void TstPrintDialog::initTestCase()
{
    QVERIFY(files_.isValid());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    // Use the same pinned Latin font as the translation layout suite on both
    // platforms. MERVIN_TEST_FONT names a font in the shared test-font directory.
    const QString fontPath = qEnvironmentVariable("MERVIN_TEST_FONT");
    if (!fontPath.isEmpty()) {
        const QString latinPath = QFileInfo(fontPath).absoluteDir().filePath(
            QStringLiteral("NotoSans-Regular.ttf"));
        QVERIFY2(QFontDatabase::addApplicationFont(latinPath) >= 0,
                 qPrintable(QStringLiteral("Could not load %1. Run scripts/fetch-test-font.py.")
                                .arg(latinPath)));
    }
    QVERIFY2(QFontDatabase::families().contains(QStringLiteral("Noto Sans")),
             "Noto Sans is required. Set MERVIN_TEST_FONT after running scripts/fetch-test-font.py.");
    QApplication::setFont(QFont(QStringLiteral("Noto Sans"), 10));
    sourcePath_ = files_.filePath("coloured-pages.pdf");
    QPdfWriter writer(sourcePath_);
    writer.setResolution(72);
    writer.setPageSize(QPageSize(QSizeF(100, 160), QPageSize::Point));
    writer.setPageMargins(QMarginsF());
    QPainter painter(&writer);
    QVERIFY(painter.isActive());
    const QColor colours[] = {QColor(220, 20, 20), QColor(20, 170, 20), QColor(20, 20, 220)};
    for (int page = 0; page < 3; ++page) {
        if (page)
            QVERIFY(writer.newPage());
        painter.fillRect(QRectF(0, 0, 75, 160), colours[page]);
    }
    QVERIFY(painter.end());
}

void TstPrintDialog::defaultsAndLiveSettings()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 2, "coloured");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *alignment = dialog.findChild<QComboBox *>("printAlignment");
    auto *colour = dialog.findChild<QComboBox *>("printColour");
    auto *scale = dialog.findChild<QComboBox *>("printScale");
    auto *percent = dialog.findChild<QSpinBox *>("printScalePercent");
    QVERIFY(preview && alignment && colour && scale && percent);
    QCOMPARE(alignment->count(), 4);
    QCOMPARE(alignment->itemText(0), QStringLiteral("Centered"));
    QCOMPARE(alignment->itemText(1), QStringLiteral("Centered vertically"));
    QCOMPARE(alignment->itemText(2), QStringLiteral("Centered horizontally"));
    QCOMPARE(alignment->itemText(3), QStringLiteral("Top left"));
    QCOMPARE(alignment->currentIndex(), 0);
    QCOMPARE(scale->currentIndex(), 0);
    QCOMPARE(colour->count(), 3);
    QCOMPARE(colour->itemText(2), QStringLiteral("Black and white"));
    QTRY_VERIFY(preview->isReady());
    QVERIFY(centreColour(preview).red() > 180);

    colour->setCurrentIndex(1);
    QTRY_COMPARE(centreColour(preview).red(), centreColour(preview).green());
    const QColor gray = centreColour(preview);
    QCOMPARE(gray.red(), gray.green());
    QCOMPARE(gray.green(), gray.blue());
    QVERIFY(gray.red() > 0 && gray.red() < 255);
    colour->setCurrentIndex(2);
    QTRY_COMPARE(centreColour(preview), QColor(Qt::black));
    colour->setCurrentIndex(0);

    QTRY_VERIFY(centreColour(preview).red() > 180);

    // Each placement is checked against where the settings put the 100 x 160 pt page.
    const QPageLayout layout = printer.pageLayout();
    const QRectF printable = layout.paintRect(QPageLayout::Point);
    const QRect paper = matchingBounds(preview->grab().toImage(), true);
    QVERIFY(!paper.isEmpty());
    const double fit = std::min(printable.width() / 100, printable.height() / 160);
    const auto centred = [&printable](double scale) {
        return printable.center() - QPointF(50, 80) * scale;
    };
    QString found;
    QTRY_VERIFY2(redPlacedAt(preview, expectedRed(paper, layout, centred(fit), fit), &found),
                 qPrintable(found));
    scale->setCurrentIndex(1);
    QTRY_VERIFY2(redPlacedAt(preview, expectedRed(paper, layout, centred(1), 1), &found),
                 qPrintable(found));
    // Top left moves both edges and keeps the size.
    alignment->setCurrentIndex(3);
    QTRY_VERIFY2(redPlacedAt(preview, expectedRed(paper, layout, printable.topLeft(), 1), &found),
                 qPrintable(found));
    scale->setCurrentIndex(2);
    percent->setValue(50);
    QTRY_VERIFY2(
        redPlacedAt(preview, expectedRed(paper, layout, printable.topLeft(), 0.5), &found),
        qPrintable(found));
}

void TstPrintDialog::printerChangesSelectDefaultPaper()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    const QPageSize custom(QSizeF(180, 260), QPageSize::Millimeter,
                           QStringLiteral("Custom stock"), QPageSize::ExactMatch);
    QVERIFY(printer.setPageSize(custom));
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    auto *printers = dialog.findChild<QComboBox *>("printPrinter");
    auto *paper = dialog.findChild<QComboBox *>("printPaperSize");
    auto *colour = dialog.findChild<QComboBox *>("printColour");
    auto *print = dialog.findChild<QPushButton *>("printConfirm");
    QVERIFY(printers && paper && colour && print);
    QCOMPARE(paper->currentData().value<QPageSize>(), custom);

    // Supply device metadata without installed printers. PDF output still exercises
    // the dialog's timer and QPrinter settings without sending a native print job.
    const QList<QPageSize> defaults{QPageSize(QPageSize::A4), QPageSize(QPageSize::Letter),
                                    custom, QPageSize()};
    const int first = printers->count();
    for (qsizetype i = 0; i < defaults.size(); ++i) {
        printers->addItem(QStringLiteral("Test printer %1").arg(i), QString::number(i));
        printers->setItemData(first + i, QVariant::fromValue(defaults.at(i)), Qt::UserRole + 1);
    }
    const auto selectedPaper = [paper] { return paper->currentData().value<QPageSize>(); };
    for (int index : {0, 1, 2, 0}) {
        printers->setCurrentIndex(first + index);
        QCOMPARE(selectedPaper(), defaults.at(index)); // also before the preview timer fires
        QTRY_VERIFY(print->isEnabled());
        QCOMPARE(printer.pageLayout().pageSize(), defaults.at(index));
    }

    // Manual paper changes survive unrelated refreshes, then reset on a device switch.
    int a3 = -1;
    for (int i = 0; i < paper->count(); ++i) {
        if (paper->itemData(i).value<QPageSize>().id() == QPageSize::A3)
            a3 = i;
    }
    QVERIFY(a3 >= 0);
    paper->setCurrentIndex(a3);
    colour->setCurrentIndex(1);
    QTRY_VERIFY(print->isEnabled());
    QCOMPARE(printer.pageLayout().pageSize().id(), QPageSize::A3);
    printers->setCurrentIndex(first + 1);
    QCOMPARE(selectedPaper(), defaults.at(1));
    QTRY_VERIFY(print->isEnabled());
    QCOMPARE(printer.pageLayout().pageSize(), defaults.at(1));

    // A device that reports no default cannot replace a valid selection with invalid paper.
    printers->setCurrentIndex(first + 3);
    QCOMPARE(selectedPaper(), defaults.at(1));
    QTRY_VERIFY(print->isEnabled());
    QCOMPARE(printer.pageLayout().pageSize(), defaults.at(1));

    // A device that disappears before selection also leaves the paper unchanged.
    printers->addItem(QStringLiteral("Unavailable printer"),
                      QStringLiteral("missing-printer-for-test-f22c5263"));
    printers->setCurrentIndex(printers->count() - 1);
    QCOMPARE(selectedPaper(), defaults.at(1));
    const QVariant missingDefault = printers->currentData(Qt::UserRole + 1);
    QVERIFY(missingDefault.isValid()); // cache the failed lookup for this dialog
    QVERIFY(!missingDefault.value<QPageSize>().isValid());
    QTRY_VERIFY(print->isEnabled());
    QCOMPARE(printer.pageLayout().pageSize(), defaults.at(1));
}

void TstPrintDialog::printerPaperAliasesReuseEntries()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    auto *printers = dialog.findChild<QComboBox *>("printPrinter");
    auto *paper = dialog.findChild<QComboBox *>("printPaperSize");
    QVERIFY(printers && paper);
    const int commonCount = paper->count();
    const QList<QPageSize> defaults{
        QPageSize(QSizeF(8.5, 11), QPageSize::Inch, QStringLiteral("US Letter"), QPageSize::ExactMatch),
        QPageSize(QSizeF(8.5, 11), QPageSize::Inch, QStringLiteral("Letter"), QPageSize::ExactMatch),
        QPageSize(QSizeF(180, 260), QPageSize::Millimeter, QStringLiteral("Custom stock"), QPageSize::ExactMatch),
        QPageSize(QSizeF(180, 260), QPageSize::Millimeter, QStringLiteral("Custom alias"), QPageSize::ExactMatch),
        QPageSize(QSizeF(510, 737), QPageSize::Point, QStringLiteral("Distinct custom key"), QPageSize::ExactMatch),
    };
    QVERIFY(defaults.at(2).isEquivalentTo(defaults.at(4)));
    QVERIFY(defaults.at(2).key() != defaults.at(4).key());
    const QList<int> addedCounts{0, 0, 1, 1, 2};
    for (qsizetype i = 0; i < defaults.size(); ++i) {
        printers->addItem(QStringLiteral("Alias printer %1").arg(i), QString::number(i));
        printers->setItemData(printers->count() - 1, QVariant::fromValue(defaults.at(i)),
                              Qt::UserRole + 1);
        printers->setCurrentIndex(printers->count() - 1);
        QCOMPARE(paper->count(), commonCount + addedCounts.at(i));
        QCOMPARE(paper->currentData().value<QPageSize>(), defaults.at(i));
        QCOMPARE(paper->currentText(), defaults.at(i).name());
    }
}

// Models the same supported/default/custom-bound fields exposed by QPrinterInfo.
void TstPrintDialog::nativePaperCapabilities_data()
{
    QTest::addColumn<PrintDialog::PaperCapabilities>("capabilities");
    QTest::addColumn<QPageSize>("requested");
    QTest::addColumn<QPageSize>("expected");
    const QPageSize a4(QPageSize::A4);
    const QPageSize letter(QSizeF(8.5, 11), QPageSize::Inch,
                           QStringLiteral("Driver US Letter"), QPageSize::ExactMatch);
    const PrintDialog::PaperCapabilities fixed{{a4, letter}, false, {}, {}};
    QTest::newRow("A4") << fixed << a4 << a4;
    QTest::newRow("Letter keeps driver definition")
        << fixed << QPageSize(QPageSize::Letter) << letter;
    QTest::newRow("unsupported A3") << fixed << QPageSize(QPageSize::A3) << QPageSize();
    const auto custom = [](double width, double height) {
        return QPageSize(QSizeF(width, height), QPageSize::Point,
                         QStringLiteral("Custom stock"), QPageSize::ExactMatch);
    };
    const QPageSize stock = custom(510, 737);
    const PrintDialog::PaperCapabilities variable{{a4}, true, custom(200, 300), custom(700, 900)};
    QTest::newRow("custom unsupported") << fixed << stock << QPageSize();
    QTest::newRow("custom within bounds") << variable << stock << stock;
    QTest::newRow("unlisted standard dimensions within custom bounds")
        << variable << QPageSize(QPageSize::Letter) << QPageSize(QPageSize::Letter);
    QTest::newRow("unlisted standard dimensions beyond custom bounds")
        << variable << QPageSize(QPageSize::A3) << QPageSize();
    QTest::newRow("custom minimum") << variable << variable.minimum << variable.minimum;
    QTest::newRow("custom maximum") << variable << variable.maximum << variable.maximum;
    QTest::newRow("custom too narrow") << variable << custom(199, 500) << QPageSize();
    QTest::newRow("custom too short") << variable << custom(500, 299) << QPageSize();
    QTest::newRow("custom too wide") << variable << custom(701, 500) << QPageSize();
    QTest::newRow("custom too tall") << variable << custom(500, 901) << QPageSize();
    QTest::newRow("custom no bounds")
        << PrintDialog::PaperCapabilities{{a4}, true, {}, {}} << stock << stock;
    QTest::newRow("missing metadata") << PrintDialog::PaperCapabilities{} << stock << stock;
    QTest::newRow("invalid paper") << PrintDialog::PaperCapabilities{} << QPageSize() << QPageSize();
    // Same physical sheet, different driver keys. Equivalence alone must not merge
    // variants such as borderless media into a driver definition it did not report.
    const QPageSize alias(QSizeF(180, 260), QPageSize::Millimeter,
                          QStringLiteral("Other media key"), QPageSize::ExactMatch);
    QVERIFY(alias.isEquivalentTo(stock));
    QVERIFY(alias.key() != stock.key());
    QTest::newRow("distinct media key")
        << PrintDialog::PaperCapabilities{{stock}, false, {}, {}} << alias << QPageSize();
}

// Checks policy independently of whether this host has a native printer installed.
void TstPrintDialog::nativePaperCapabilities()
{
    QFETCH(PrintDialog::PaperCapabilities, capabilities);
    QFETCH(QPageSize, requested);
    QFETCH(QPageSize, expected);
    QCOMPARE(capabilities.acceptedPaper(requested), expected);
}

// Exercises the real dialog's device switches, lazy metadata and PDF independence.
void TstPrintDialog::nativePaperSwitchAndPdfFallback()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    QStringList queries;
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1,
        "coloured", nullptr, [&queries](const QString &name) {
            queries.append(name);
            if (name == QStringLiteral("A4"))
                return PrintDialog::PaperCapabilities{{QPageSize(QPageSize::A4)}, false, {}, {}};
            if (name == QStringLiteral("Letter"))
                return PrintDialog::PaperCapabilities{{QPageSize(QPageSize::Letter)}, false, {}, {}};
            return PrintDialog::PaperCapabilities{};
        });
    auto *printers = dialog.findChild<QComboBox *>("printPrinter");
    auto *paper = dialog.findChild<QComboBox *>("printPaperSize");
    auto *toFile = dialog.findChild<QCheckBox *>("printToFile");
    auto *print = dialog.findChild<QPushButton *>("printConfirm");
    QVERIFY(printers && paper && toFile && print);
    QVERIFY(queries.isEmpty());
    const int first = printers->count();
    for (const auto &[name, size] : QList<QPair<QString, QPageSize>>{
             {QStringLiteral("A4"), QPageSize(QPageSize::A4)},
             {QStringLiteral("Letter"), QPageSize(QPageSize::Letter)},
             {QStringLiteral("Missing"), QPageSize()}}) {
        printers->addItem(name, name);
        printers->setItemData(printers->count() - 1, QVariant::fromValue(size), Qt::UserRole + 1);
    }
    printers->setCurrentIndex(first);
    toFile->setEnabled(true);
    toFile->setChecked(false);
    QCOMPARE(queries, QStringList{"A4"});
    QCOMPARE(paper->count(), 1);
    QCOMPARE(paper->currentData().value<QPageSize>().id(), QPageSize::A4);
    // A stale or externally supplied choice must still fail at acceptance.
    paper->addItem("A3", QVariant::fromValue(QPageSize(QPageSize::A3)));
    paper->setCurrentIndex(paper->count() - 1);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "accept"));
    QVERIFY(!print->isEnabled());
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    printers->setCurrentIndex(first + 1);
    QCOMPARE(paper->count(), 1);
    QCOMPARE(paper->currentData().value<QPageSize>().id(), QPageSize::Letter);
    printers->setCurrentIndex(first);
    QCOMPARE(queries, (QStringList{"A4", "Letter"}));
    printers->setCurrentIndex(first + 2);
    QVERIFY(paper->count() >= 9);
    QCOMPARE(paper->currentData().value<QPageSize>().id(), QPageSize::A4);
    toFile->setChecked(true);
    const int a3 = paper->findData(QVariant::fromValue(QPageSize(QPageSize::A3)));
    QVERIFY(a3 >= 0);
    paper->setCurrentIndex(a3);
    QTRY_VERIFY(print->isEnabled());
    QCOMPARE(printer.outputFormat(), QPrinter::PdfFormat);
    QCOMPARE(printer.pageLayout().pageSize().id(), QPageSize::A3);
    QCOMPARE(queries, (QStringList{"A4", "Letter", "Missing"}));
}

void TstPrintDialog::systemHandoffKeepsSelectedPrinterDefault_data()
{
    QTest::addColumn<int>("switches");
    QTest::newRow("unchanged printer keeps initial layout") << 0;
    QTest::newRow("new printer default") << 1;
    QTest::newRow("original printer default after switching back") << 2;
}

void TstPrintDialog::systemHandoffKeepsSelectedPrinterDefault()
{
    QFETCH(int, switches);
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    QVERIFY(printer.setPageSize(QPageSize(QPageSize::A5)));
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    auto *printers = dialog.findChild<QComboBox *>("printPrinter");
    auto *paper = dialog.findChild<QComboBox *>("printPaperSize");
    auto *system = dialog.findChild<QPushButton *>("printSystemDialog");
    QVERIFY(printers && paper && system);
    const int initial = printers->currentIndex();
    printers->setItemData(initial, QVariant::fromValue(QPageSize(QPageSize::A4)),
                          Qt::UserRole + 1);
    const QPageSize letter(QPageSize::Letter);
    printers->addItem(QStringLiteral("Letter printer"), QStringLiteral("test-letter"));
    printers->setItemData(printers->count() - 1, QVariant::fromValue(letter), Qt::UserRole + 1);
    if (switches > 0)
        printers->setCurrentIndex(printers->count() - 1);
    if (switches > 1)
        printers->setCurrentIndex(initial);
    // A manual choice must not replace the native handoff's printer default.
    const int a3 = paper->findData(QVariant::fromValue(QPageSize(QPageSize::A3)));
    QVERIFY(a3 >= 0);
    paper->setCurrentIndex(a3);
    QTRY_COMPARE(printer.pageLayout().pageSize().id(), QPageSize::A3);
    // Exercise the handoff callback even on hosts without a native print device.
    system->setEnabled(true);
    system->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.useSystemDialog());
    const QPageSize expected = switches == 0 ? QPageSize(QPageSize::A5)
        : switches == 1 ? letter : QPageSize(QPageSize::A4);
    // Native drivers may give equivalent paper sizes different display names.
    QVERIFY(printer.pageLayout().pageSize().isEquivalentTo(expected));
}

void TstPrintDialog::customOrderValidationAndStaleResults()
{
    RenderEngine engine;
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 2, "coloured");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *custom = dialog.findChild<QRadioButton *>("printCustomPages");
    auto *edit = dialog.findChild<QLineEdit *>("printPages");
    auto *next = dialog.findChild<QPushButton *>("printNextPage");
    auto *previous = dialog.findChild<QPushButton *>("printPreviousPage");
    auto *print = dialog.findChild<QPushButton *>("printConfirm");
    QVERIFY(preview && custom && edit && next && previous && print);
    QTRY_VERIFY(preview->isReady());
    QVERIFY(!results.isEmpty());
    const RenderResult obsolete = qvariant_cast<RenderResult>(results.last().first());

    edit->setText("3,1,3");
    custom->setChecked(true);
    QTRY_VERIFY(preview->isReady());
    QTRY_VERIFY(centreColour(preview).blue() > 180);
    QVERIFY(!previous->isEnabled());
    QVERIFY(next->isEnabled());
    next->click();
    QTRY_VERIFY(preview->isReady());
    QTRY_VERIFY(centreColour(preview).red() > 180);
    next->click();
    QTRY_VERIFY(preview->isReady());
    QTRY_VERIFY(centreColour(preview).blue() > 180);
    QVERIFY(!next->isEnabled());
    QVERIFY(previous->isEnabled());

    // Simulate a completed worker result delivered after navigation superseded it.
    engine.resultReady(obsolete);
    QCoreApplication::processEvents();
    QVERIFY(centreColour(preview).blue() > 180);
    previous->click();
    next->click();
    previous->click();
    next->click();
    QTRY_VERIFY(preview->isReady());
    QTRY_VERIFY(centreColour(preview).blue() > 180);

    edit->setText("9");
    QTRY_VERIFY(!print->isEnabled());
    QVERIFY(!preview->isReady());
    QVERIFY(!next->isEnabled());
    QVERIFY(!previous->isEnabled());
    edit->setText("1");
    QTRY_VERIFY(print->isEnabled());
    QTRY_VERIFY(preview->isReady());
    QTRY_VERIFY(centreColour(preview).red() > 180);
}

void TstPrintDialog::sheetAlwaysFitsAfterResizeAndOrientation()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *landscape = dialog.findChild<QRadioButton *>("printLandscape");
    QVERIFY(preview && landscape);
    QTRY_VERIFY(preview->isReady());
    const QRect portrait = matchingBounds(preview->grab().toImage(), true);
    QVERIFY(!portrait.isEmpty());
    QVERIFY(portrait.height() > portrait.width());
    QVERIFY(preview->rect().contains(portrait));
    QVERIFY(std::abs(double(portrait.width()) / portrait.height() - 210.0 / 297.0) < 0.01);

    dialog.resize(dialog.width() + 240, dialog.height() + 160);
    QCoreApplication::processEvents();
    const QRect enlarged = matchingBounds(preview->grab().toImage(), true);
    QVERIFY(enlarged.width() > portrait.width());
    QVERIFY(enlarged.height() > portrait.height());
    QVERIFY(preview->rect().contains(enlarged));
    landscape->setChecked(true);
    // Wait for the whole landscape sheet, not only a sheet wider than it is tall.
    const auto landscapeSheet = [preview] {
        const QRect sheet = matchingBounds(preview->grab().toImage(), true);
        return !sheet.isEmpty() && preview->rect().contains(sheet)
               && std::abs(double(sheet.width()) / sheet.height() - 297.0 / 210.0) < 0.01;
    };
    QTRY_VERIFY(landscapeSheet());

    // Viewer rotation changes the content too. The source's 75 × 160 red region
    // becomes 160 × 75 after a quarter turn, independently of paper orientation.
    QPrinter rotatedPrinter;
    configurePrinter(rotatedPrinter);
    PrintDialog rotatedDialog(&rotatedPrinter, &engine, document.get(), 90,
                              QPageLayout::Landscape, 1, "rotated");
    rotatedDialog.show();
    auto *rotatedPreview = rotatedDialog.findChild<PrintPreviewWidget *>();
    QVERIFY(rotatedPreview);
    QTRY_VERIFY(rotatedPreview->isReady());
    const QRect turnedContent = matchingBounds(rotatedPreview->grab().toImage(), false);
    QVERIFY(!turnedContent.isEmpty());
    QVERIFY(std::abs(double(turnedContent.width()) / turnedContent.height() - 160.0 / 75.0) < 0.05);
}

void TstPrintDialog::blackAndWhitePreviewConvertsOnce()
{
    RenderEngine engine;
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    // A screen-resolution printer would cap the black-and-white preview below 300 dpi.
    QPrinter printer(QPrinter::HighResolution);
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *colour = dialog.findChild<QComboBox *>("printColour");
    QVERIFY(preview && colour);
    QTRY_VERIFY(preview->isReady());
    QSignalSpy rendered(preview, &PrintPreviewWidget::previewReady);
    colour->setCurrentIndex(2);
    QTRY_VERIFY(!rendered.isEmpty());
    QTRY_VERIFY(previewRenderRetired(engine, results));
    QCOMPARE(centreColour(preview), QColor(Qt::black));
    const int renders = rendered.size();
    const int conversions = preview->conversions();
    const int reductions = preview->reductions();
    QVERIFY(conversions > 0);
    QVERIFY(reductions > 0);

    // Exposes reuse the prepared raster. A resize reduces the conversion once for the
    // new device size and converts nothing, since the render's scale follows the print
    // quality rather than the widget size.
    for (int i = 0; i < 5; ++i)
        preview->repaint();
    QCOMPARE(preview->conversions(), conversions);
    QCOMPARE(preview->reductions(), reductions);
    const QSize before = preview->size();
    dialog.resize(dialog.width() + 120, dialog.height() + 80);
    QTRY_VERIFY(preview->size() != before);
    QCOMPARE(centreColour(preview), QColor(Qt::black));
    QTRY_COMPARE(preview->reductions(), reductions + 1);
    // The resize schedules a 100 ms debounce even when its raster is reusable.
    // Observe that interval before checking no request was submitted; worker
    // completion itself is observed below instead of guessed from a delay.
    const int rasterizations = RenderEngine::testing::rasterizations(engine);
    QTest::qWait(150);
    QTRY_VERIFY(previewRenderRetired(engine, results));
    for (int i = 0; i < 5; ++i)
        preview->repaint();
    QCOMPARE(preview->reductions(), reductions + 1);
    QCOMPARE(rendered.size(), renders);
    QCOMPARE(RenderEngine::testing::rasterizations(engine), rasterizations);
    QCOMPARE(preview->conversions(), conversions);
}

void TstPrintDialog::staleSamePagePreviewCannotReplaceNewGeneration()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    PrintPreviewWidget preview(&engine, document.get(), 0);
    preview.resize(420, 480);
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    QSignalSpy ready(&preview, &PrintPreviewWidget::previewReady);
    const QPageLayout layout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                             QMarginsF(), QPageLayout::Point);
    printing::Settings settings;
    preview.setPage(0, layout, settings);
    preview.show();
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(results.size(), 1);
    QVERIFY(centreColour(&preview).red() > 180);
    RenderResult stale = results.front().front().value<RenderResult>();

    settings.colourMode = printing::ColourMode::BlackAndWhite;
    preview.setPage(0, layout, settings);
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE(results.size(), 2);
    const auto current = results.back().front().value<RenderResult>();
    QCOMPARE(current.requester, stale.requester);
    QCOMPARE(current.pageNo, stale.pageNo);
    QVERIFY(current.epoch != stale.epoch);
    QCOMPARE(centreColour(&preview), QColor(Qt::black));
    const QImage frame = preview.grab().toImage();

    // A late completion for this same page must not replace the newer raster.
    // White would visibly replace the current black ink if generation is ignored.
    stale.image.fill(Qt::white);
    emit engine.resultReady(stale);
    QCOMPARE(ready.size(), 2);
    QCOMPARE(preview.grab().toImage(), frame);

    // The same payload with the current generation is accepted, so the rejection
    // above cannot pass merely because result delivery or painting is disconnected.
    stale.epoch = current.epoch;
    stale.scale = current.scale;
    emit engine.resultReady(stale);
    QCOMPARE(ready.size(), 3);
    QCOMPARE(centreColour(&preview), QColor(Qt::white));
}

// A page whose raster at the preview's resolution is beyond what MuPDF draws must say
// so instead of staying on the loading text or keeping an earlier picture.
void TstPrintDialog::previewReportsAnUnrenderablePage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 14.4 million points square (UserUnit 1000). Fitted to A4 it renders, but at
    // actual size even a screen-resolution raster is far beyond 2^20 pixels.
    const QString path = writePagePdf(dir, QStringLiteral("huge.pdf"),
                                      "/MediaBox [0 0 14400 14400] /UserUnit 1000",
                                      "0.86 0.08 0.08 rg 0 0 14400 14400 re f");
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    auto document = engine.openDocument(path);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "huge");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *scale = dialog.findChild<QComboBox *>("printScale");
    QVERIFY(preview && scale);
    QTRY_VERIFY(preview->isReady());
    QVERIFY(centreColour(preview).red() > 180);

    QSignalSpy answered(preview, &PrintPreviewWidget::previewReady);
    scale->setCurrentIndex(1); // Actual size
    QTRY_VERIFY(!answered.isEmpty());
    QVERIFY(!preview->isReady());
    // The message replaces the sheet, so no paper is drawn.
    QVERIFY(matchingBounds(preview->grab().toImage(), true).isEmpty());
    scale->setCurrentIndex(0);
    QTRY_VERIFY(preview->isReady());
    QVERIFY(centreColour(preview).red() > 180);
}

// The preview renders at a lower resolution than printing. A page that printing would
// refuse at the print quality must say so in the preview too, instead of showing it.
void TstPrintDialog::previewPredictsAPrintRefusal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 432000 points square (UserUnit 30). At actual size and 300 dpi its raster is
    // 1.8 million pixels square, beyond 2^20, while a screen-resolution raster is not.
    const QString path = writePagePdf(dir, QStringLiteral("long.pdf"),
                                      "/MediaBox [0 0 14400 14400] /UserUnit 30",
                                      "0.86 0.08 0.08 rg 0 0 14400 14400 re f");
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    auto document = engine.openDocument(path);
    QVERIFY(document);
    QPrinter printer(QPrinter::HighResolution);
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "long");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *scale = dialog.findChild<QComboBox *>("printScale");
    QVERIFY(preview && scale);
    QTRY_VERIFY(preview->isReady());

    QSignalSpy answered(preview, &PrintPreviewWidget::previewReady);
    scale->setCurrentIndex(1); // Actual size
    QTRY_VERIFY(!answered.isEmpty());
    QTRY_VERIFY(previewRenderRetired(engine, results));
    QVERIFY(!preview->isReady());
    QVERIFY(matchingBounds(preview->grab().toImage(), true).isEmpty());
}

// At actual size only the middle of a page larger than the sheet is visible. The
// preview renders just that part, places it where the page puts it, and in black and
// white keeps a hairline, as printing does.
void TstPrintDialog::previewShowsTheVisiblePartOfALargePage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A white 1000 x 1400 pt page with a 40 pt black square at its centre and a 0.1 pt
    // hairline 100.5 pt below the centre.
    const QString path = writePagePdf(dir, QStringLiteral("large.pdf"),
                                      "/MediaBox [0 0 1000 1400]",
                                      "0 g 480 680 40 40 re f 0 G 0.1 w 400 599.5 m 600 599.5 l S");
    QVERIFY(!path.isEmpty());
    RenderEngine engine;
    QSignalSpy results(&engine, &RenderEngine::resultReady);
    auto document = engine.openDocument(path);
    QVERIFY(document);
    // A screen-resolution printer would cap the black-and-white preview below 300 dpi.
    QPrinter printer(QPrinter::HighResolution);
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "large");
    dialog.show();
    auto *preview = dialog.findChild<PrintPreviewWidget *>();
    auto *scale = dialog.findChild<QComboBox *>("printScale");
    auto *colour = dialog.findChild<QComboBox *>("printColour");
    QVERIFY(preview && scale && colour);
    QTRY_VERIFY(preview->isReady());
    QSignalSpy rendered(preview, &PrintPreviewWidget::previewReady);
    scale->setCurrentIndex(1); // Actual size
    colour->setCurrentIndex(2);
    QTRY_VERIFY(!rendered.isEmpty());
    QTRY_VERIFY(previewRenderRetired(engine, results));
    QTRY_VERIFY(preview->isReady());

    // The page centre lands at the printable area's centre, in sheet points.
    const QImage image = preview->grab().toImage();
    const QRect sheet = matchingBounds(image, true);
    QVERIFY(!sheet.isEmpty());
    const QPageLayout layout = printer.pageLayout();
    const double perPoint = sheet.width() / layout.fullRect(QPageLayout::Point).width();
    const QPointF centre = QPointF(sheet.topLeft())
        + layout.paintRect(QPageLayout::Point).center() * perPoint;
    QRect marker;
    const int reach = int(40 * perPoint);
    for (int y = int(centre.y()) - reach; y <= int(centre.y()) + reach; ++y)
        for (int x = int(centre.x()) - reach; x <= int(centre.x()) + reach; ++x)
            if (image.rect().contains(x, y) && qGray(image.pixel(x, y)) < 100)
                marker |= QRect(x, y, 1, 1);
    QVERIFY2(!marker.isEmpty(), "no marker near the printable centre");
    const QPointF found = QRectF(marker).center();
    QVERIFY2(std::abs(found.x() - centre.x()) <= 2 && std::abs(found.y() - centre.y()) <= 2,
             qPrintable(QStringLiteral("marker at %1, %2, expected %3, %4").arg(found.x())
                            .arg(found.y()).arg(centre.x()).arg(centre.y())));
    QVERIFY(std::abs(marker.width() - 40 * perPoint) <= 2);

    // The hairline, rendered without antialiasing before the threshold, shows as a
    // darker row. Antialiased, it would turn white at the threshold.
    const int row = qRound(centre.y() + 100.5 * perPoint);
    int darkest = 255;
    for (int y = row - 1; y <= row + 1; ++y)
        for (int x = int(centre.x() - 50 * perPoint); x <= int(centre.x() + 50 * perPoint); ++x)
            darkest = std::min(darkest, qGray(image.pixel(x, y)));
    QVERIFY2(darkest < 240, qPrintable(QStringLiteral("darkest %1 at %2 px a point")
                                           .arg(darkest).arg(perPoint)));
}

void TstPrintDialog::cancellingSaveKeepsDialogOpen()
{
    RenderEngine engine;
    auto document = engine.openDocument(sourcePath_);
    QVERIFY(document);
    QPrinter printer;
    configurePrinter(printer);
    PrintDialog dialog(&printer, &engine, document.get(), 0, QPageLayout::Portrait, 1, "coloured");
    dialog.show();
    auto *toFile = dialog.findChild<QCheckBox *>("printToFile");
    auto *print = dialog.findChild<QPushButton *>("printConfirm");
    QVERIFY(toFile && print);
    toFile->setChecked(true);
    bool pickerSeen = false;
    QTimer cancel;
    connect(&cancel, &QTimer::timeout, &dialog, [&] {
        if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
            pickerSeen = true;
            picker->reject();
        }
    });
    cancel.start(10);
    QTimer::singleShot(10000, &dialog, &QDialog::reject);
    print->click();
    cancel.stop();
    QVERIFY(pickerSeen);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(printer.outputFileName().isEmpty());
}

QTEST_MAIN(TstPrintDialog)
#include "tst_print_dialog.moc"
