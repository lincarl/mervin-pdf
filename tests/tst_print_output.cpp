#include "config/ConfigPaths.h"
#include "dialogs/PrintDialog.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace mervin;

class TstPrintOutput : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void applicationPrintPlacement_data();
    void applicationPrintPlacement();

private:
    QTemporaryDir files_;
    QString sourcePath_;
};

void TstPrintOutput::initTestCase()
{
    QVERIFY(files_.isValid());
    ConfigPaths::setOverrideDir(files_.filePath("profile"));
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    sourcePath_ = files_.filePath("source.pdf");
    QPdfWriter writer(sourcePath_);
    writer.setResolution(72);
    writer.setPageSize(QPageSize(QSizeF(100, 160), QPageSize::Point));
    writer.setPageMargins(QMarginsF());
    QPainter painter(&writer);
    QVERIFY(painter.isActive());
    painter.fillRect(QRectF(0, 0, 100, 160), QColor(220, 20, 20));
    painter.fillRect(QRectF(10, 10, 20, 20), QColor(200, 200, 200));
    QVERIFY(painter.end());
}

void TstPrintOutput::cleanupTestCase()
{
    ConfigPaths::setOverrideDir({});
}

void TstPrintOutput::applicationPrintPlacement_data()
{
    QTest::addColumn<int>("alignment");
    QTest::addColumn<int>("scaleMode");
    QTest::addColumn<int>("percent");
    QTest::addColumn<int>("colour");
    QTest::newRow("actual centered") << 0 << 1 << 100 << 0;
    QTest::newRow("actual vertical") << 1 << 1 << 100 << 0;
    QTest::newRow("actual horizontal") << 2 << 1 << 100 << 0;
    QTest::newRow("actual top left") << 3 << 1 << 100 << 0;
    QTest::newRow("custom centered") << 0 << 2 << 150 << 0;
    QTest::newRow("custom top left") << 3 << 2 << 50 << 0;
    QTest::newRow("fit centered") << 0 << 0 << 100 << 0;
    QTest::newRow("grayscale") << 0 << 1 << 100 << 1;
    QTest::newRow("black and white") << 0 << 1 << 100 << 2;
}

void TstPrintOutput::applicationPrintPlacement()
{
    QFETCH(int, alignment);
    QFETCH(int, scaleMode);
    QFETCH(int, percent);
    QFETCH(int, colour);
    const QString output = files_.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(window.openFile(sourcePath_));

    // Rebuild the printer the way the app does. It creates this same printer
    // before showing its dialog, and the dialog keeps that printer's margins
    // when it switches to PDF output, clamped to what the page allows. Without
    // printers (Linux CI) the first printer is already PDF, with Qt's nonzero
    // default margins, which expose the former double-offset bug. On Windows it
    // is the default printer, whose margins can be zero.
    const QMarginsF preferred =
        QPrinter(QPrinter::HighResolution).pageLayout().margins(QPageLayout::Point);
    QPrinter reference(QPrinter::HighResolution);
    reference.setOutputFormat(QPrinter::PdfFormat);
    reference.setFullPage(false);
    reference.setPageSize(QPageSize(QPageSize::A4));
    reference.setPageOrientation(QPageLayout::Portrait);
    QPageLayout layout = reference.pageLayout();
    layout.setUnits(QPageLayout::Point);
    const QMarginsF minimum = layout.minimumMargins();
    const QMarginsF maximum = layout.maximumMargins();
    QVERIFY(reference.setPageMargins(
        QMarginsF(qBound(minimum.left(), preferred.left(), maximum.left()),
                  qBound(minimum.top(), preferred.top(), maximum.top()),
                  qBound(minimum.right(), preferred.right(), maximum.right()),
                  qBound(minimum.bottom(), preferred.bottom(), maximum.bottom())),
        QPageLayout::Point));
    const QRectF printable = reference.pageLayout().paintRect(QPageLayout::Point);
    if (QPrinterInfo::availablePrinters().isEmpty()) {
        QVERIFY(printable.left() > 0);
        QVERIFY(printable.top() > 0);
    }

    QStringList errors;
    bool configured = false;
    bool printQueued = false;
    bool saveQueued = false;
    bool saved = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer dialogs;
    connect(&dialogs, &QTimer::timeout, &window, [&] {
        QWidget *modal = QApplication::activeModalWidget();
        if (errors.isEmpty() && elapsed.elapsed() > 20000) {
            const QString stage = !configured ? QStringLiteral("waiting for the print dialog")
                : !printQueued ? QStringLiteral("waiting for validated print settings")
                : !saveQueued ? QStringLiteral("waiting for the save dialog")
                : !saved ? QStringLiteral("saving the selected output path")
                         : QStringLiteral("waiting for print completion");
            errors.append(QStringLiteral("Print interaction timed out while %1").arg(stage));
        }
        if (!errors.isEmpty()) {
            if (auto *dialog = qobject_cast<QDialog *>(modal))
                dialog->reject();
            return;
        }
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            errors.append(box->text());
            box->reject();
            return;
        }
        if (auto *dialog = qobject_cast<PrintDialog *>(modal)) {
            auto *print = dialog->findChild<QPushButton *>("printConfirm");
            if (!configured) {
                configured = true;
                auto *toFile = dialog->findChild<QCheckBox *>("printToFile");
                auto *paper = dialog->findChild<QComboBox *>("printPaperSize");
                auto *align = dialog->findChild<QComboBox *>("printAlignment");
                auto *scale = dialog->findChild<QComboBox *>("printScale");
                auto *percentage = dialog->findChild<QSpinBox *>("printScalePercent");
                auto *colourCombo = dialog->findChild<QComboBox *>("printColour");
                auto *quality = dialog->findChild<QComboBox *>("printQuality");
                if (!toFile || !paper || !align || !scale || !percentage || !colourCombo
                    || !quality || !print) {
                    errors.append(QStringLiteral("Print controls were missing"));
                    dialog->reject();
                    return;
                }
                toFile->setChecked(true);
                for (int index = 0; index < paper->count(); ++index) {
                    if (paper->itemData(index).value<QPageSize>().id() == QPageSize::A4) {
                        paper->setCurrentIndex(index);
                        break;
                    }
                }
                align->setCurrentIndex(alignment);
                scale->setCurrentIndex(scaleMode);
                percentage->setValue(percent);
                colourCombo->setCurrentIndex(colour);
                // Raster quality does not affect placement. Keep this probe at 72 DPI.
                quality->setItemData(quality->currentIndex(), 72);
            }
            // Validation is debounced. Wait for its result instead of assuming
            // it finished after a fixed delay on a busy machine.
            if (!printQueued && print->isEnabled()) {
                printQueued = true;
                QTimer::singleShot(0, print, &QPushButton::click);
            }
            return;
        }
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            if (saveQueued)
                return;
            auto *name = picker->findChild<QLineEdit *>("fileNameEdit");
            if (!name) {
                errors.append(QStringLiteral("The save dialog filename field was missing"));
                picker->reject();
                return;
            }
            saveQueued = true;
            QTimer::singleShot(0, picker, [&, picker, name] {
                // An absolute typed filename avoids selectFile() racing the
                // asynchronous directory model and restoring the suggested name.
                name->setText(QDir::toNativeSeparators(output));
                const QStringList selected = picker->selectedFiles();
                if (selected.size() != 1
                    || QFileInfo(selected.front()).absoluteFilePath() != output) {
                    errors.append(QStringLiteral("The save dialog selected the wrong output path"));
                    picker->reject();
                    return;
                }
                // Validate and accept in one event turn so directory updates
                // cannot replace the filename between these two operations.
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                saved = picker->result() == QDialog::Accepted;
            });
        }
    });
    dialogs.start(10);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    dialogs.stop();
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(configured);
    QVERIFY(saved);
    QVERIFY(QFileInfo::exists(output));
    auto printed = engine.openDocument(output);
    QVERIFY(printed);
    QCOMPARE(printed->pageCount(), 1);
    const QImage image = engine.renderPageImage(printed.get(), 0, 1.0, 0);
    QVERIFY(!image.isNull());

    QRect actual;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() < 240 || pixel.green() < 240 || pixel.blue() < 240)
                actual |= QRect(x, y, 1, 1);
        }
    }
    QVERIFY(!actual.isEmpty());
    double factor = scaleMode == 2 ? percent / 100.0 : 1.0;
    if (scaleMode == 0)
        factor = std::min(printable.width() / 100, printable.height() / 160);
    const QSizeF expectedSize(100 * factor, 160 * factor);
    double left = printable.left();
    double top = printable.top();
    if (alignment == 0 || alignment == 2)
        left += (printable.width() - expectedSize.width()) / 2;
    if (alignment == 0 || alignment == 1)
        top += (printable.height() - expectedSize.height()) / 2;
    const QRectF expected(QPointF(left, top), expectedSize);
    QVERIFY2(std::abs(actual.left() - expected.left()) <= 2,
             qPrintable(QString("Printed x %1, expected %2").arg(actual.left()).arg(expected.left())));
    QVERIFY2(std::abs(actual.top() - expected.top()) <= 2,
             qPrintable(QString("Printed y %1, expected %2").arg(actual.top()).arg(expected.top())));
    QVERIFY(std::abs(actual.width() - expected.width()) <= 2);
    QVERIFY(std::abs(actual.height() - expected.height()) <= 2);
    const QColor ink = image.pixelColor(actual.center());
    if (colour == 0) {
        QVERIFY(ink.red() > 180 && ink.green() < 80 && ink.blue() < 80);
    } else if (colour == 1) {
        QCOMPARE(ink.red(), ink.green());
        QCOMPARE(ink.green(), ink.blue());
        QVERIFY(ink.red() > 20 && ink.red() < 220);
    } else {
        QCOMPARE(ink, QColor(Qt::black));
        // Check the actual PDF image samples, avoiding viewer interpolation at
        // the page edges. GrayScale alone would retain the source's 200-gray patch.
        QPDF pdf;
        pdf.processFile(output.toUtf8().constData());
        auto page = QPDFPageDocumentHelper(pdf).getAllPages().front();
        auto images = page.getImages();
        QVERIFY(!images.empty());
        bool black = false;
        bool white = false;
        for (auto &[name, stream] : images) {
            const int bits = stream.getDict().getKey("/BitsPerComponent").getIntValue();
            QVERIFY(bits == 1 || bits == 8);
            const auto data = stream.getStreamData(qpdf_dl_all);
            for (size_t i = 0; i < data->getSize(); ++i) {
                const unsigned char sample = data->getBuffer()[i];
                if (bits == 8)
                    QVERIFY(sample == 0 || sample == 255);
                black |= sample != 255;
                white |= sample != 0;
            }
        }
        QVERIFY(black);
        QVERIFY(white);
    }
}

QTEST_MAIN(TstPrintOutput)
#include "tst_print_output.moc"
