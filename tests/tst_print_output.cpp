#include "TestWaits.h"
#include "config/ConfigPaths.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "io/JobContext.h"
#include "dialogs/PrintDialog.h"
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
                messages.append(box->text());
                box->accept();
            } else if (auto *dialog = qobject_cast<PrintDialog *>(modal)) {
                auto *print = dialog->findChild<QPushButton *>("printConfirm");
                auto *toFile = dialog->findChild<QCheckBox *>("printToFile");
                auto *quality = dialog->findChild<QComboBox *>("printQuality");
                if (!print || !toFile || !quality) {
                    messages.append(QStringLiteral("Print controls were missing"));
                    dialog->reject();
                    return;
                }
                if (!configured_) {
                    configured_ = true;
                    toFile->setChecked(true);
                    quality->setItemData(quality->currentIndex(), 72);
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
    bool saved = false;

private:
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool configured_ = false;
    bool queued_ = false;
    bool saveQueued_ = false;
};

QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int darkInkOnRedPage(const QImage &image)
{
    QRect red;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 170 && pixel.green() < 90 && pixel.blue() < 90)
                red |= QRect(x, y, 1, 1);
        }
    int ink = 0;
    for (int y = red.top(); !red.isEmpty() && y <= red.bottom(); ++y)
        for (int x = red.left(); x <= red.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            ink += pixel.red() < 80 && pixel.green() < 80 && pixel.blue() < 80;
        }
    return ink;
}

} // namespace

class TstPrintOutput : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void applicationPrintPlacement_data();
    void applicationPrintPlacement();
    void printDoesNotWaitForPageAccess();
    void printDestinationConflict_data();
    void printDestinationConflict();
    void stoppedFileOutputKeepsDestination_data();
    void stoppedFileOutputKeepsDestination();
    void printEndFailureIsReported();
    void measuredPreparationRunsInBackground();
    void stoppedMeasuredPreparationRemovesItsFiles();
    void editorCommitCanDeletePrintOwner();

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
    QVERIFY(mervin::appfiles::flush(10000));
    ConfigPaths::setOverrideDir({});
}

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
    auto *tab = window.findChild<TabPage *>();
    QVERIFY(tab);
    QTRY_VERIFY_WITH_TIMEOUT(!tab->isBusy(), 10000);
    dialogs.stop();
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(configured);
    QVERIFY(saved);
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

void TstPrintOutput::printDoesNotWaitForPageAccess()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(window.openFile(sourcePath_));
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

void TstPrintOutput::printDestinationConflict_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("source") << 0;
    QTest::newRow("another open file") << 1;
#ifdef Q_OS_UNIX
    QTest::newRow("source alias") << 2;
#endif
}

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
        QVERIFY(window.openFile(output));
    } else if (kind == 2) {
        output = files.filePath("alias.pdf");
        QVERIFY(QFile::link(sourcePath_, output));
    }
    QVERIFY(window.openFile(sourcePath_));
    const QByteArray before = bytes(output);
    QVERIFY(!before.isEmpty());
    PrintInteraction dialogs(window, output);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages, QStringList{"The destination is open in a tab."}, 10000);
    QCOMPARE(bytes(output), before);
    QCOMPARE(bytes(sourcePath_), before);
}

void TstPrintOutput::stoppedFileOutputKeepsDestination_data()
{
    QTest::addColumn<bool>("commit");
    QTest::newRow("destination writing") << false;
    QTest::newRow("before rename") << true;
}

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
    QVERIFY(window.openFile(sourcePath_));
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

void TstPrintOutput::printEndFailureIsReported()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(window.openFile(sourcePath_));
    const QString output = files_.filePath("failed-end.pdf");
    job::testing::failWhen([prints = 0](job::Step step, const QString &) mutable {
        return step == job::Step::Printing && ++prints == 3;
    });
    PrintInteraction dialogs(window, output);
    QVERIFY(QMetaObject::invokeMethod(&window, "printDocument"));
    QTRY_COMPARE_WITH_TIMEOUT(dialogs.messages, QStringList{"Could not finish the print job."}, 10000);
    QVERIFY(!QFileInfo::exists(output));
}

void TstPrintOutput::measuredPreparationRunsInBackground()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(window.openFile(sourcePath_));
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
    PrintInteraction dialogs(window, output, [&](PrintDialog &dialog) {
        auto *preview = dialog.findChild<PrintPreviewWidget *>();
        previewReady = preview && preview->isReady();
        if (previewReady)
            previewInk = darkInkOnRedPage(preview->grab().toImage());
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
    QVERIFY(previewInk > 20);
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

void TstPrintOutput::editorCommitCanDeletePrintOwner()
{
    RenderEngine engine;
    const QPointer<MainWindow> window(new MainWindow(&engine, nullptr));
    QVERIFY(window->openFile(sourcePath_));
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

void TstPrintOutput::stoppedMeasuredPreparationRemovesItsFiles()
{
    RenderEngine engine;
    MainWindow window(&engine, nullptr);
    QVERIFY(window.openFile(sourcePath_));
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

QTEST_MAIN(TstPrintOutput)
#include "tst_print_output.moc"
