#include "dialogs/PrintDialog.h"
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
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

} // namespace

class TstPrintDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void defaultsAndLiveSettings();
    void printerChangesSelectDefaultPaper();
    void systemHandoffKeepsSelectedPrinterDefault_data();
    void systemHandoffKeepsSelectedPrinterDefault();
    void customOrderValidationAndStaleResults();
    void sheetAlwaysFitsAfterResizeAndOrientation();
    void blackAndWhitePreviewConvertsOnce();
    void previewReportsAnUnrenderablePage();
    void previewPredictsAPrintRefusal();
    void previewShowsTheVisiblePartOfALargePage();
    void cancellingSaveKeepsDialogOpen();

private:
    QTemporaryDir files_;
    QString sourcePath_;
};

void TstPrintDialog::initTestCase()
{
    QVERIFY(files_.isValid());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
    auto *system = dialog.findChild<QPushButton *>("printSystemDialog");
    QVERIFY(printers && system);
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
    QTest::qWait(300); // no further render is due
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
    QTest::qWait(300);
    QVERIFY(preview->size() != before);
    QCOMPARE(centreColour(preview), QColor(Qt::black));
    QCOMPARE(preview->reductions(), reductions + 1);
    for (int i = 0; i < 5; ++i)
        preview->repaint();
    QCOMPARE(preview->reductions(), reductions + 1);
    QCOMPARE(rendered.size(), renders);
    QCOMPARE(preview->conversions(), conversions);
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
    QTest::qWait(300); // no further render is due
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
    QTest::qWait(300); // no further render is due
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
