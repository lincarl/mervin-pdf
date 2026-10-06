#include "dialogs/PrintDialog.h"
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
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
    void customOrderValidationAndStaleResults();
    void sheetAlwaysFitsAfterResizeAndOrientation();
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
    const int fitWidth = matchingBounds(preview->grab().toImage(), false).width();
    scale->setCurrentIndex(1);
    QTRY_VERIFY(matchingBounds(preview->grab().toImage(), false).width() < fitWidth);
    const QRect centered = matchingBounds(preview->grab().toImage(), false);
    QVERIFY(!centered.isEmpty());
    alignment->setCurrentIndex(3);
    QTRY_VERIFY(matchingBounds(preview->grab().toImage(), false).left() < centered.left());
    const QRect topLeft = matchingBounds(preview->grab().toImage(), false);
    QVERIFY(topLeft.left() < centered.left());
    QVERIFY(topLeft.top() < centered.top());
    QVERIFY(std::abs(topLeft.width() - centered.width()) <= 1);
    QVERIFY(std::abs(topLeft.height() - centered.height()) <= 1);
    scale->setCurrentIndex(2);
    percent->setValue(50);
    QTRY_VERIFY(matchingBounds(preview->grab().toImage(), false).width() < topLeft.width());
    const QRect smaller = matchingBounds(preview->grab().toImage(), false);
    QVERIFY(std::abs(smaller.width() * 2 - topLeft.width()) <= 2);
    QVERIFY(std::abs(smaller.height() * 2 - topLeft.height()) <= 2);
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
    QCoreApplication::processEvents();
    QTRY_VERIFY(matchingBounds(preview->grab().toImage(), true).width()
                > matchingBounds(preview->grab().toImage(), true).height());
    const QRect rotated = matchingBounds(preview->grab().toImage(), true);
    QVERIFY(rotated.width() > rotated.height());
    QVERIFY(std::abs(double(rotated.width()) / rotated.height() - 297.0 / 210.0) < 0.01);
    QVERIFY(preview->rect().contains(rotated));

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
