#include "TestWaits.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "dialogs/PrintDialog.h"
#include "print/PrintLayout.h"
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "render/AnnotModel.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"
#include "security/MeasureExport.h"
#include "Stalls.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPdfWriter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTimer>
#include <QTransform>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace mervin;

namespace {

// Drives the actual custom dialog and file picker while leaving asynchronous
// completion active for assertions, including worker failures after acceptance.
class PrintInteraction
{
public:
    // Polls real print controls until acceptance or a bounded interaction timeout.
    PrintInteraction(MainWindow &window, QString output,
                     std::function<void(PrintDialog &)> preview = {})
    {
        elapsed_.start();
        QObject::connect(&timer_, &QTimer::timeout, &window,
                [this, output = std::move(output), preview = std::move(preview)] {
            QWidget *modal = QApplication::activeModalWidget();
            if (elapsed_.elapsed() > 20000) {
                if (messages.isEmpty())
                    messages.append(QStringLiteral("Print interaction timed out"));
                if (auto *dialog = qobject_cast<QDialog *>(modal))
                    dialog->reject();
                return;
            }
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                messageOwner = box->parentWidget();
                messages.append(box->text());
                box->accept();
            } else if (auto *dialog = qobject_cast<PrintDialog *>(modal)) {
                auto *print = dialog->findChild<QPushButton *>("printConfirm");
                auto *toFile = dialog->findChild<QCheckBox *>("printToFile");
                auto *quality = dialog->findChild<QComboBox *>("printQuality");
                auto *colour = dialog->findChild<QComboBox *>("printColour");
                if (!print || !toFile || !quality || !colour) {
                    messages.append(QStringLiteral("Print controls were missing"));
                    dialog->reject();
                    return;
                }
                if (!configured_) {
                    configured_ = true;
                    toFile->setChecked(true);
                    // Changing the selection also refreshes the preview at this quality.
                    quality->addItem(QStringLiteral("Test (72 dpi)"), 72);
                    quality->setCurrentIndex(quality->count() - 1);
                    // The red fixture requires colour regardless of the default printer.
                    colour->setCurrentIndex(colour->findData(
                        static_cast<int>(printing::ColourMode::Colour)));
                }
                if (!print->isEnabled() || queued_)
                    return;
                if (preview) {
                    auto *rendered = dialog->findChild<PrintPreviewWidget *>();
                    if (!rendered || !rendered->isReady())
                        return;
                }
                if (preview)
                    preview(*dialog);
                queued_ = true;
                QTimer::singleShot(0, print, &QPushButton::click);
            } else if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
                if (saveQueued_)
                    return;
                auto *name = picker->findChild<QLineEdit *>("fileNameEdit");
                if (!name) {
                    messages.append(QStringLiteral("Save controls were missing"));
                    picker->reject();
                    return;
                }
                saveQueued_ = true;
                picker->setOption(QFileDialog::DontConfirmOverwrite, true);
                name->setText(QDir::toNativeSeparators(output));
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                saved = picker->result() == QDialog::Accepted;
            }
        });
        timer_.start(10);
    }

    QStringList messages;
    QPointer<QWidget> messageOwner;
    bool saved = false;

private:
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool configured_ = false;
    bool queued_ = false;
    bool saveQueued_ = false;
};

// Writes a one-page PDF with a complete cross-reference table to `path`.
bool writePagePdf(const QString &path, const QByteArray &page, const QByteArray &content)
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
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(pdf) == pdf.size();
}

// The printable area of the app's PDF printer on A4. The app creates this same printer
// before showing its dialog, and the dialog keeps that printer's margins when it
// switches to PDF output, clamped to what the page allows. Without printers (Linux CI)
// the first printer is already PDF, with Qt's nonzero default margins, which expose the
// former double-offset bug. On Windows it is the default printer, whose margins can be
// zero. The app turns the sheet to match the page's orientation. Empty if the margins
// cannot be set.
QRectF applicationPrintable(QPageLayout::Orientation orientation = QPageLayout::Portrait)
{
    const QMarginsF preferred =
        QPrinter(QPrinter::HighResolution).pageLayout().margins(QPageLayout::Point);
    QPrinter reference(QPrinter::HighResolution);
    reference.setOutputFormat(QPrinter::PdfFormat);
    reference.setFullPage(false);
    reference.setPageSize(QPageSize(QPageSize::A4));
    reference.setPageOrientation(orientation);
    QPageLayout layout = reference.pageLayout();
    layout.setUnits(QPageLayout::Point);
    const QMarginsF minimum = layout.minimumMargins();
    const QMarginsF maximum = layout.maximumMargins();
    if (!reference.setPageMargins(
            QMarginsF(qBound(minimum.left(), preferred.left(), maximum.left()),
                      qBound(minimum.top(), preferred.top(), maximum.top()),
                      qBound(minimum.right(), preferred.right(), maximum.right()),
                      qBound(minimum.bottom(), preferred.bottom(), maximum.bottom())),
            QPageLayout::Point))
        return {};
    return reference.pageLayout().paintRect(QPageLayout::Point);
}

// Print dialog choices for placement checks. The indexes follow the dialog's
// alignment, scale and colour lists.
struct PlacementChoices
{
    int alignment = 0;
    int scaleMode = 1;
    int percent = 100;
    int colour = 0;
    int qualityDpi = 72;
};

// Drives the custom dialog on A4 with `choices` and the file picker, and records any
// message box, until stop(). Collects what went wrong in `errors`.
class PlacementInteraction
{
public:
    PlacementInteraction(MainWindow &window, QString output, PlacementChoices choices)
    {
        elapsed_.start();
        QObject::connect(&timer_, &QTimer::timeout, &window,
                [this, output = std::move(output), choices] {
            QWidget *modal = QApplication::activeModalWidget();
            if (errors.isEmpty() && elapsed_.elapsed() > 20000) {
                const QString stage = !configured ? QStringLiteral("waiting for the print dialog")
                    : !printQueued_ ? QStringLiteral("waiting for validated print settings")
                    : !saveQueued_ ? QStringLiteral("waiting for the save dialog")
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
                    align->setCurrentIndex(choices.alignment);
                    scale->setCurrentIndex(choices.scaleMode);
                    percentage->setValue(choices.percent);
                    colourCombo->setCurrentIndex(choices.colour);
                    // Most checks probe placement, which raster quality does not affect,
                    // at 72 dpi, where one raster pixel is one printed point.
                    quality->setItemData(quality->currentIndex(), choices.qualityDpi);
                }
                // Validation is debounced. Wait for its result instead of assuming
                // it finished after a fixed delay on a busy machine.
                if (!printQueued_ && print->isEnabled()) {
                    printQueued_ = true;
                    QTimer::singleShot(0, print, &QPushButton::click);
                }
                return;
            }
            if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
                if (saveQueued_)
                    return;
                auto *name = picker->findChild<QLineEdit *>("fileNameEdit");
                if (!name) {
                    errors.append(QStringLiteral("The save dialog filename field was missing"));
                    picker->reject();
                    return;
                }
                saveQueued_ = true;
                QTimer::singleShot(0, picker, [this, output, picker, name] {
                    // An absolute typed filename avoids selectFile() racing the
                    // asynchronous directory model and restoring the suggested name.
                    name->setText(QDir::toNativeSeparators(output));
                    const QStringList selected = picker->selectedFiles();
                    if (selected.size() != 1
                        || QFileInfo(selected.front()).absoluteFilePath() != output) {
                        errors.append(
                            QStringLiteral("The save dialog selected the wrong output path"));
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
        timer_.start(10);
    }

    void stop() { timer_.stop(); }

    QStringList errors;
    bool configured = false;
    bool saved = false;

private:
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool printQueued_ = false;
    bool saveQueued_ = false;
};

// Collects where a page draws each image, in drawing order, by following the content
// stream's q, Q and cm operators. Rectangles are in points from the page's top-left.
class ImagePlacements : public QPDFObjectHandle::ParserCallbacks
{
public:
    explicit ImagePlacements(double pageHeight) : pageHeight_(pageHeight) {}

    void handleObject(QPDFObjectHandle object) override
    {
        if (!object.isOperator()) {
            operands_.push_back(object);
            return;
        }
        const std::string op = object.getOperatorValue();
        if (op == "q") {
            stack_.push_back(ctm_);
        } else if (op == "Q" && !stack_.empty()) {
            ctm_ = stack_.back();
            stack_.pop_back();
        } else if (op == "cm" && operands_.size() == 6) {
            const auto value = [this](size_t i) { return operands_[i].getNumericValue(); };
            ctm_ = QTransform(value(0), value(1), value(2), value(3), value(4), value(5)) * ctm_;
        } else if (op == "Do" && operands_.size() == 1) {
            const QRectF unit = ctm_.mapRect(QRectF(0, 0, 1, 1));
            names.push_back(operands_[0].getName());
            rects.append(QRectF(unit.left(), pageHeight_ - unit.bottom(), unit.width(),
                                unit.height()));
        }
        operands_.clear();
    }
    void handleEOF() override {}

    std::vector<std::string> names; // the image resources, in drawing order
    QList<QRectF> rects;            // where each one lands

private:
    double pageHeight_;
    QTransform ctm_;
    std::vector<QTransform> stack_;
    std::vector<QPDFObjectHandle> operands_;
};

// Reads fixture bytes to prove stopped output leaves an existing destination intact.
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Finds the synthetic page while excluding the surrounding preview controls.
QRect redPageBounds(const QImage &image)
{
    QRect red;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 170 && pixel.green() < 90 && pixel.blue() < 90)
                red |= QRect(x, y, 1, 1);
        }
    return red;
}

// Counts black label text in the full-size flattened print output.
int darkInkOnRedPage(const QImage &image)
{
    const QRect red = redPageBounds(image);
    int ink = 0;
    for (int y = red.top(); !red.isEmpty() && y <= red.bottom(); ++y)
        for (int x = red.left(); x <= red.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            ink += pixel.red() < 80 && pixel.green() < 80 && pixel.blue() < 80;
        }
    return ink;
}

// Counts the blue measurement stroke, which remains visible in low-resolution previews.
int blueInkOnRedPage(const QImage &image)
{
    const QRect red = redPageBounds(image);
    int ink = 0;
    for (int y = red.top(); !red.isEmpty() && y <= red.bottom(); ++y)
        for (int x = red.left(); x <= red.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            ink += pixel.blue() > pixel.red() + 50 && pixel.blue() > pixel.green() + 20;
        }
    return ink;
}

} // namespace

// Checks rendered print placement, worker cancellation and prepared-input ownership.
class TstPrintOutput : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void applicationPrintPlacement_data();
    void applicationPrintPlacement();
    void printedRasterGeometry_data();
    void printedRasterGeometry();
    void unrenderablePageReportsFailure();
    void printDoesNotWaitForPageAccess();
    void printDestinationConflict_data();
    void printDestinationConflict();
    void stoppedFileOutputKeepsDestination_data();
    void stoppedFileOutputKeepsDestination();
    void printEndFailureIsReported();
    void measuredPreparationRunsInBackground();
    void stoppedMeasuredPreparationRemovesItsFiles();
    void editorCommitCanDeletePrintOwner();
    void queuedDeliverySurvivesOldOwnerDestruction_data();
    void queuedDeliverySurvivesOldOwnerDestruction();

private:
    QTemporaryDir files_;
    QString sourcePath_;
    QString largePath_;
    QString hugePath_;
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
    painter.setPen(QPen(Qt::black, 0.1));
    painter.drawLine(QLineF(12, 20.5, 28, 20.5));
    QVERIFY(painter.end());

    // A page larger than A4 with a 40 pt black square at its centre.
    largePath_ = files_.filePath("large.pdf");
    QPdfWriter large(largePath_);
    large.setResolution(72);
    large.setPageSize(QPageSize(QSizeF(1000, 1400), QPageSize::Point));
    large.setPageMargins(QMarginsF());
    QPainter marker(&large);
    QVERIFY(marker.isActive());
    marker.fillRect(QRectF(480, 680, 40, 40), Qt::black);
    QVERIFY(marker.end());

    // A 2.88 million point square page (UserUnit 200), whose raster even at 72 dpi is
    // beyond the 2^20 pixels MuPDF draws correctly.
    hugePath_ = files_.filePath("huge.pdf");
    QVERIFY(writePagePdf(hugePath_, "/MediaBox [0 0 14400 14400] /UserUnit 200",
                         "0.86 0.08 0.08 rg 0 0 14400 14400 re f 0 g 7100 7100 200 200 re f"));
}

void TstPrintOutput::cleanupTestCase()
{
    QVERIFY(mervin::appfiles::flush(10000));
    ConfigPaths::setOverrideDir({});
}

// Releases print stalls and settles background closes between output cases.
void TstPrintOutput::cleanup()
{
    job::testing::release();
    job::testing::failWhen({});
    TabPage::setSlowDelayForTesting(TabPage::kSlowMs);
    QVERIFY(background::settle(10000));
    QCoreApplication::processEvents();
    QVERIFY(Document::waitForCloses(10000));
    QVERIFY(appfiles::flush(10000));
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

// Checks asynchronous PDF output preserves placement and requested colour conversion.
void TstPrintOutput::applicationPrintPlacement()
{
    QFETCH(int, alignment);
    QFETCH(int, scaleMode);
    QFETCH(int, percent);
    QFETCH(int, colour);
    const QString output = files_.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));

    const QRectF printable = applicationPrintable();
    QVERIFY(!printable.isEmpty());
    if (QPrinterInfo::availablePrinters().isEmpty()) {
        QVERIFY(printable.left() > 0);
        QVERIFY(printable.top() > 0);
    }

    PlacementInteraction dialogs(window, output, {alignment, scaleMode, percent, colour, 72});
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy(), 10000);
    dialogs.stop();
    QVERIFY2(dialogs.errors.isEmpty(), qPrintable(dialogs.errors.join('\n')));
    QVERIFY(dialogs.configured);
    QVERIFY(dialogs.saved);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(output), 10000);
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
    {
        // Qt's PDF engine embeds an image at its own pixel size, and this 72 dpi
        // probe renders one pixel per printed point, whatever the page's scale.
        QPDF pdf;
        pdf.processFile(output.toUtf8().constData());
        auto images = QPDFPageDocumentHelper(pdf).getAllPages().front().getImages();
        QCOMPARE(images.size(), size_t(1));
        auto dict = images.begin()->second.getDict();
        const long long width = dict.getKey("/Width").getIntValue();
        const long long height = dict.getKey("/Height").getIntValue();
        QVERIFY2(std::abs(width - expectedSize.width()) <= 2
                     && std::abs(height - expectedSize.height()) <= 2,
                 qPrintable(QString("Embedded %1 x %2, printed %3 x %4 pt").arg(width)
                                .arg(height).arg(expectedSize.width())
                                .arg(expectedSize.height())));
    }
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
        // The 0.1 pt hairline across the light patch, 20 pt below the page top, prints
        // black. Antialiased, its light gray would fall above the threshold and vanish.
        QCOMPARE(images.size(), size_t(1));
        QPDFObjectHandle stream = images.begin()->second;
        QCOMPARE(stream.getDict().getKey("/BitsPerComponent").getIntValue(), 8);
        const long long width = stream.getDict().getKey("/Width").getIntValue();
        const auto data = stream.getStreamData(qpdf_dl_all);
        bool hairline = false;
        for (long long row = 18; row <= 22 && !hairline; ++row) {
            bool line = true;
            for (long long x = 14; x <= 26; ++x)
                line &= data->getBuffer()[row * width + x] == 0;
            hairline |= line;
        }
        QVERIFY(hairline);
    }
}

void TstPrintOutput::printedRasterGeometry_data()
{
    QTest::addColumn<bool>("large");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<int>("scaleMode");
    QTest::addColumn<int>("quality");
    // Only the middle of the page is visible, so the raster starts inside the page.
    QTest::newRow("oversized page") << true << 0 << 1 << 72;
    // More than 2^24 visible pixels, so the page prints in several bands.
    QTest::newRow("bands at 600 dpi") << true << 0 << 0 << 600;
    QTest::newRow("turned page") << false << 90 << 1 << 72;
}

// Checks the black and white raster of a printed page: its embedded bands, where its
// marker lands, and a page turned in the viewer.
void TstPrintOutput::printedRasterGeometry()
{
    QFETCH(bool, large);
    QFETCH(int, rotation);
    QFETCH(int, scaleMode);
    QFETCH(int, quality);
    const QString output =
        files_.filePath(QString::fromLatin1(QTest::currentDataTag()).replace(' ', '-') + ".pdf");
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, large ? largePath_ : sourcePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    tab->viewer()->setRotation(rotation);
    // The page size printPages reads: 1000 x 1400 pt or 100 x 160 pt, turned with the view.
    QSizeF page = tab->viewer()->document()->pageSize(0);
    QCOMPARE(page, large ? QSizeF(1000, 1400) : QSizeF(100, 160));
    if (rotation == 90)
        page.transpose();
    const QRectF printable = applicationPrintable(
        page.width() > page.height() ? QPageLayout::Landscape : QPageLayout::Portrait);
    QVERIFY(!printable.isEmpty());

    PlacementInteraction dialogs(window, output, {0, scaleMode, 100, 2, quality});
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy(), 20000);
    dialogs.stop();
    QVERIFY2(dialogs.errors.isEmpty(), qPrintable(dialogs.errors.join('\n')));
    QVERIFY(dialogs.saved);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(output), 10000);

    // Where the centred page lands, from its size alone.
    const double factor = scaleMode == 0
        ? std::min(printable.width() / page.width(), printable.height() / page.height())
        : 1.0;
    const QSizeF size = page * factor;
    const QRectF destination(printable.center() - QPointF(size.width(), size.height()) / 2,
                             size);

    // The bands, in drawing order, are the rows rasterBands splits the planned raster
    // into under the print path's 2^24-pixel budget. Every band but the last also
    // carries the next band's first row, and each lands exactly where its rows belong,
    // so the repeated row sits under the next band's first row.
    printing::Settings settings;
    settings.scaleMode = printing::ScaleMode(scaleMode);
    settings.colourMode = printing::ColourMode::BlackAndWhite;
    const printing::RasterPlan plan = printing::rasterPlan(page, printable, settings, quality);
    QVERIFY(!plan.isEmpty());
    const QList<QRect> expected = printing::rasterBands(plan.source, qint64(1) << 24);
    QVERIFY(quality == 600 ? expected.size() >= 2 : expected.size() == 1);
    QPDF pdf;
    pdf.processFile(output.toUtf8().constData());
    QPDFPageObjectHelper sheet = QPDFPageDocumentHelper(pdf).getAllPages().front();
    ImagePlacements placements(sheet.getMediaBox().getArrayItem(3).getNumericValue());
    sheet.parseContents(&placements);
    auto images = sheet.getImages();
    QCOMPARE(placements.rects.size(), expected.size());
    // Qt's PDF engine puts the printable area's origin on a whole 1200 dpi device pixel
    // (QPdfEnginePrivate::pageMatrix), which moves everything drawn by the same amount,
    // under one device pixel (0.06 pt). Each band is placed relative to that.
    QPointF shift;
    for (qsizetype index = 0; index < expected.size(); ++index) {
        QPDFObjectHandle band = images.at(placements.names[index]);
        QPDFObjectHandle dict = band.getDict();
        QCOMPARE(dict.getKey("/BitsPerComponent").getIntValue(), 8);
        const int width = int(dict.getKey("/Width").getIntValue());
        const int height = int(dict.getKey("/Height").getIntValue());
        const bool last = index + 1 == expected.size();
        QCOMPARE(width, plan.source.width());
        QCOMPARE(height, expected[index].height() + (last ? 0 : 1));
        const QRectF want = printing::rasterTarget(
            plan.destination, page, plan.scale,
            QRect(expected[index].topLeft(), QSize(width, height)));
        if (index == 0) {
            shift = placements.rects[0].topLeft() - want.topLeft();
            QVERIFY2(std::abs(shift.x()) < 0.06 && std::abs(shift.y()) < 0.06,
                     qPrintable(QStringLiteral("shifted by %1, %2").arg(shift.x())
                                    .arg(shift.y())));
        }
        const QRectF got = placements.rects[index].translated(-shift);
        QVERIFY2(std::abs(got.left() - want.left()) < 0.005
                     && std::abs(got.top() - want.top()) < 0.005
                     && std::abs(got.width() - want.width()) < 0.005
                     && std::abs(got.height() - want.height()) < 0.005,
                 qPrintable(QStringLiteral("band %1 drawn at %2, %3, %4 x %5 instead of %6, %7, "
                                           "%8 x %9")
                                .arg(index).arg(got.left()).arg(got.top()).arg(got.width())
                                .arg(got.height()).arg(want.left()).arg(want.top())
                                .arg(want.width()).arg(want.height())));
        if (!last) {
            const auto above = band.getStreamData(qpdf_dl_all);
            const auto below = images.at(placements.names[index + 1]).getStreamData(qpdf_dl_all);
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(above->getBuffer())
                                    + qsizetype(height - 1) * width, width),
                     QByteArray(reinterpret_cast<const char *>(below->getBuffer()), width));
        }
    }

    // The marker, the black square at the large page's centre or the light patch near
    // the small page's top-left corner, lands where the page puts it.
    auto printed = engine.openDocument(output);
    QVERIFY(printed);
    const QImage image = engine.renderPageImage(printed.get(), 0, 1.0, 0);
    QVERIFY(!image.isNull());
    if (large) {
        QRect marker;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128)
                    marker |= QRect(x, y, 1, 1);
        const QPointF centre = QRectF(marker).center();
        QVERIFY2(std::abs(centre.x() - printable.center().x()) <= 1.5
                     && std::abs(centre.y() - printable.center().y()) <= 1.5,
                 qPrintable(QStringLiteral("marker at %1, %2, printable centre %3, %4")
                                .arg(centre.x()).arg(centre.y())
                                .arg(printable.center().x()).arg(printable.center().y())));
        QVERIFY(std::abs(marker.width() - 40 * factor) <= 2);
        QVERIFY(std::abs(marker.height() - 40 * factor) <= 2);
    } else {
        // Turned clockwise, the patch from (10, 10) to (30, 30) of the 100 x 160 page
        // covers (130, 10) to (150, 30) of the 160 x 100 page. The hairline across it
        // becomes a vertical line at x 139.5, so sample the patch beside the line.
        const QPointF patch = destination.topLeft() + QPointF(146, 14);
        const QPointF unturned = destination.topLeft() + QPointF(14, 14);
        QCOMPARE(image.pixelColor(patch.toPoint()), QColor(Qt::white));
        QCOMPARE(image.pixelColor(unturned.toPoint()), QColor(Qt::black));
    }
}

// A page whose raster is beyond what MuPDF draws reports its page instead of printing
// a blank sheet.
void TstPrintOutput::unrenderablePageReportsFailure()
{
    const QString output = files_.filePath("unrenderable.pdf");
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, hugePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    PlacementInteraction dialogs(window, output, {0, 1, 100, 0, 72});
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_VERIFY_WITH_TIMEOUT(!dialogs.errors.isEmpty(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy(), 10000);
    dialogs.stop();
    QCOMPARE(dialogs.errors, QStringList{QStringLiteral("Could not render page 1.")});
    QVERIFY(!QFileInfo::exists(output));
}

// Checks the accepted print command returns while live document access is stalled.
void TstPrintOutput::printDoesNotWaitForPageAccess()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    const auto lifetime = tab->viewer()->document()->lifetime();
    const QString output = files_.filePath("held-access.pdf");
    job::testing::holdWhen([](job::Step step, const QString &) {
        return step == job::Step::Printing;
    });
    PrintInteraction dialogs(window, output);
    QElapsedTimer command;
    command.start();
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QVERIFY2(command.elapsed() < 1000, qPrintable(QString("Print took %1 ms").arg(command.elapsed())));
    QVERIFY(tab->isBusy());
    QTRY_COMPARE(job::testing::held(), 1);
    mervin::testing::Stall access(tab->viewer()->document()->accessMutex(), 10000);
    const int guiSteps = job::testing::stepsOnMainThread();
    const int tasks = background::running() - 1;
    job::testing::release();
    QTRY_VERIFY(lifetime->holds.load() > 0);
    tab->stopWaiting();
    QVERIFY(tab->isBusy());
    QVERIFY(tab->isOperationStopped());
    QCOMPARE(job::testing::stepsOnMainThread(), guiSteps);
    access.release();
    QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(dialogs.messages.isEmpty());
}

// Selects the source, its alias and another tab as protected print destinations.
void TstPrintOutput::printDestinationConflict_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("source") << 0;
    QTest::newRow("another open file") << 1;
#ifdef Q_OS_UNIX
    QTest::newRow("source alias") << 2;
#endif
}

// Checks protected destination refusal preserves original PDF bytes.
void TstPrintOutput::printDestinationConflict()
{
    QFETCH(int, kind);
    QTemporaryDir files;
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QString output = sourcePath_;
    if (kind == 1) {
        output = files.filePath("other.pdf");
        QVERIFY(QFile::copy(sourcePath_, output));
        QVERIFY(mervin::testing::openAndWait(window, output));
    } else if (kind == 2) {
        output = files.filePath("alias.pdf");
        QVERIFY(QFile::link(sourcePath_, output));
    }
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    const QByteArray before = bytes(output);
    QVERIFY(!before.isEmpty());
    PrintInteraction dialogs(window, output);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages, QStringList{"The destination is open in a tab."}, 10000);
    QCOMPARE(bytes(output), before);
    QCOMPARE(bytes(sourcePath_), before);
}

// Selects Stop during writing and before final print-to-file commit.
void TstPrintOutput::stoppedFileOutputKeepsDestination_data()
{
    QTest::addColumn<bool>("commit");
    QTest::newRow("destination writing") << false;
    QTest::newRow("before rename") << true;
}

// Checks Stop preserves earlier destination bytes and removes only private print output.
void TstPrintOutput::stoppedFileOutputKeepsDestination()
{
    QFETCH(bool, commit);
    QTemporaryDir files;
    const QString output = files.filePath("existing.pdf");
    const QByteArray before("Existing destination bytes");
    QFile destination(output);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write(before), before.size());
    destination.close();
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    const int tasks = background::running();
    job::testing::holdWhen([&, writes = 0](job::Step step, const QString &path) mutable {
        if (commit)
            return step == job::Step::Commit && path == output;
        return step == job::Step::Writing && path == files.path() && ++writes == 2;
    });
    PrintInteraction dialogs(window, output);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
    tab->stopWaiting();
    QVERIFY(tab->isUsable());
    QCOMPARE(bytes(output), before);
    job::testing::release();
    QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
    QCOMPARE(bytes(output), before);
    QCOMPARE(QDir(files.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"existing.pdf"});
    QVERIFY(dialogs.messages.isEmpty());
}

// Checks final printer submission failure is reported after the tab restores use.
void TstPrintOutput::printEndFailureIsReported()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    const QString output = files_.filePath("failed-end.pdf");
    job::testing::failWhen([prints = 0](job::Step step, const QString &) mutable {
        return step == job::Step::Printing && ++prints == 3;
    });
    PrintInteraction dialogs(window, output);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages, QStringList{"Could not finish the print job."}, 10000);
    QVERIFY(!QFileInfo::exists(output));
}

// Checks measured preparation returns while held and produces a real flattened preview.
void TstPrintOutput::measuredPreparationRunsInBackground()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    tab->viewer()->loadMeasurements(
        {{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}}, {}, MeasureUnit::Millimeter, 2, 2);
    const QString output = files_.filePath("measured-print.pdf");
    job::testing::holdWhen([](job::Step step, const QString &path) {
        return step == job::Step::Writing && path != QDir::tempPath();
    });
    bool previewReady = false;
    int previewInk = 0;
    QString previewDetails;
    PrintInteraction dialogs(window, output, [&](PrintDialog &dialog) {
        auto *preview = dialog.findChild<PrintPreviewWidget *>();
        previewReady = preview && preview->isReady();
        if (previewReady) {
            const QImage image = preview->grab().toImage();
            previewInk = blueInkOnRedPage(image);
            const int labelInk = darkInkOnRedPage(image);
            previewDetails = QStringLiteral(
                "Preview blue stroke %1, black label %2, colour mode %3, image %4x%5, DPR %6")
                .arg(previewInk).arg(labelInk)
                .arg(static_cast<int>(dialog.printSettings().colourMode))
                .arg(image.width()).arg(image.height()).arg(image.devicePixelRatio());
        }
    });
    QElapsedTimer command;
    command.start();
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QVERIFY2(command.elapsed() < 1000,
             qPrintable(QString("Measured preparation took %1 ms").arg(command.elapsed())));
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
    QVERIFY(tab->isBusy());
    QVERIFY(!previewReady);
    job::testing::release();
    QTRY_VERIFY_WITH_TIMEOUT(previewReady, 10000);
    QVERIFY2(previewInk > 20, qPrintable(previewDetails));
    QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy() && QFileInfo::exists(output), 10000);
    QVERIFY(dialogs.messages.isEmpty());
    auto document = engine.openDocument(output);
    QVERIFY(document);
    QVERIFY(!document->hasMervinMeasurements());
    const QImage image = document->renderPage(0, 1, 0);
    QVERIFY(!image.isNull());
    QVERIFY(darkInkOnRedPage(image) > 100);
    QVERIFY(tab->viewer()->hasMeasurements());
}

// Checks editor-driven owner deletion prevents Print from using a stale tab.
void TstPrintOutput::editorCommitCanDeletePrintOwner()
{
    RenderEngine engine;
    const QPointer<MainWindow> window(new MainWindow(&engine, nullptr));
    QVERIFY(mervin::testing::openAndWait(*window, sourcePath_));
    const QPointer<TabPage> tab(window->findChild<TabPage *>());
    QVERIFY(tab);
    auto *viewer = tab->viewer();
    const int note = viewer->annotModel()->addTextNote(
        0, QPointF(30, 60), QColor(Qt::yellow), {}, QStringLiteral("Initial comment"));
    QVERIFY(note >= 0);
    window->show();
    viewer->setCommentToolEnabled(true);
    viewer->setAnnotSubMode(AnnotSubMode::Note);
    viewer->revealAnnotation(0, note);
    auto *editor = viewer->findChild<QPlainTextEdit *>();
    QVERIFY(editor && editor->isVisible());
    editor->setPlainText(QStringLiteral("Typed before Print"));
    connect(viewer, &ViewerWidget::annotationsChanged, this, [window] { delete window.data(); });
    QVERIFY(QMetaObject::invokeMethod(window.data(), "printDocument"));
    QVERIFY(!window && !tab);
    QVERIFY(!qobject_cast<PrintDialog *>(QApplication::activeModalWidget()));
}

// Checks canceled measured preparation removes private documents and folders.
void TstPrintOutput::stoppedMeasuredPreparationRemovesItsFiles()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(mervin::testing::openAndWait(window, sourcePath_));
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    tab->viewer()->loadMeasurements(
        {{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}}, {}, MeasureUnit::Millimeter, 2, 2);
    const int tasks = background::running();
    QString stage;
    job::testing::holdWhen([&](job::Step step, const QString &path) {
        if (step != job::Step::Writing || path == QDir::tempPath())
            return false;
        stage = path;
        return true;
    });
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
    QVERIFY(!stage.isEmpty() && QFileInfo::exists(stage));
    tab->stopWaiting();
    QVERIFY(tab->isBusy());
    job::testing::release();
    QTRY_VERIFY_WITH_TIMEOUT(tab->isUsable(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(background::running() <= tasks, 10000);
    QVERIFY(!QFileInfo::exists(stage));
    QVERIFY(tab->viewer()->hasMeasurements());
    QVERIFY(!qobject_cast<PrintDialog *>(QApplication::activeModalWidget()));
}

// Selects prepared dialog and error delivery after an earlier queued owner move.
void TstPrintOutput::queuedDeliverySurvivesOldOwnerDestruction_data()
{
    QTest::addColumn<bool>("preparation");
    QTest::addColumn<bool>("normalMove");
    QTest::newRow("prepared dialog after last-tab merge") << true << true;
    QTest::newRow("prepared dialog after immediate deletion") << true << false;
    QTest::newRow("failure after last-tab merge") << false << true;
    QTest::newRow("failure after immediate deletion") << false << false;
}

// Checks queued print delivery follows the moved tab after its previous window closes.
void TstPrintOutput::queuedDeliverySurvivesOldOwnerDestruction()
{
    QFETCH(bool, preparation);
    QFETCH(bool, normalMove);
    Settings settings;
    settings.autoUpdate = false;
    settings.closeToTray = false;
    QVERIFY(settings.save());
    WindowManager manager;
    const QPointer<MainWindow> source(manager.createWindow());
    MainWindow *target = manager.createWindow();
    QVERIFY(source->openFile(sourcePath_));
    const QPointer<TabPage> tab(source->findChild<TabPage *>());
    QTRY_VERIFY_WITH_TIMEOUT(tab && tab->isUsable(), 10000);
    if (preparation) {
        tab->viewer()->loadMeasurements(
            {{0, MeasureKind::Distance, {{15, 80}, {85, 80}}}}, {}, MeasureUnit::Millimeter, 2, 2);
        job::testing::holdWhen([](job::Step step, const QString &path) {
            return step == job::Step::Writing && path != QDir::tempPath();
        });
    } else {
        job::testing::holdWhen([](job::Step step, const QString &) {
            return step == job::Step::Printing;
        });
        job::testing::failWhen([prints = 0](job::Step step, const QString &) mutable {
            return step == job::Step::Printing && ++prints == 3;
        });
    }
    const auto release = qScopeGuard([] { job::testing::release(); });
    QTemporaryDir files;
    const QString output = files.filePath("printed.pdf");
    bool previewReady = false;
    bool correctOwner = false;
    PrintInteraction dialogs(*target, output, preparation ? std::function<void(PrintDialog &)>(
        [&](PrintDialog &dialog) {
            previewReady = true;
            correctOwner = dialog.parentWidget() == target;
        }) : std::function<void(PrintDialog &)>());
    QVERIFY(QMetaObject::invokeMethod(source, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(job::testing::held(), 1, 10000);
    QVERIFY(tab->isBusy());
    QDialog modal;
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    if (normalMove)
        source->whenTabsCanChange([&manager, source, target] {
            manager.mergeTab(source, 0, target, 0);
        });
    job::testing::release();
    QTRY_VERIFY_WITH_TIMEOUT(tab && !tab->isBusy(), 10000);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QVERIFY(source && !source->canChangeTabs());
    if (!normalMove) {
        target->adoptTab(source->releaseTab(0));
        delete source.data();
    }
    modal.hide();
    if (normalMove)
        source->runTabChanges();
    QTRY_VERIFY_WITH_TIMEOUT(!source, 10000);
    QVERIFY(tab && tab->window() == target);
    if (preparation) {
        QTRY_VERIFY_WITH_TIMEOUT(previewReady, 10000);
        QVERIFY(correctOwner);
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy() && QFileInfo::exists(output), 10000);
        QVERIFY(dialogs.messages.isEmpty());
        auto document = manager.engine()->openDocument(output);
        QVERIFY(document);
        QCOMPARE(document->pageCount(), 1);
        QVERIFY(!document->hasMervinMeasurements());
        QVERIFY(darkInkOnRedPage(document->renderPage(0, 1, 0)) > 100);
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages,
                                 QStringList{"Could not finish the print job."}, 10000);
        QCOMPARE(dialogs.messageOwner.data(), static_cast<QWidget *>(target));
        QVERIFY(!QFileInfo::exists(output));
    }
}

QTEST_MAIN(TstPrintOutput)
#include "tst_print_output.moc"
