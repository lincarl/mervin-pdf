#include "print/PrintLayout.h"

#include <QPainter>
#include <QTest>

#include <limits>

using namespace mervin::printing;

class TstPrintLayout : public QObject
{
    Q_OBJECT

private slots:
    void pagePlacement_data();
    void pagePlacement();
    void oversizedPageKeepsPhysicalSize();
    void invalidDimensionsAreEmpty();
    void colourAndGrayscaleRemainDistinct();
    void blackAndWhiteThresholdAndTransparency();
    void painterClipsAndPreservesState();
    void blackAndWhiteScalingKeepsBinaryPixels();
    void reducedPreviewRetainsFineLines();
};

void TstPrintLayout::pagePlacement_data()
{
    QTest::addColumn<ScaleMode>("scaleMode");
    QTest::addColumn<Alignment>("alignment");
    QTest::addColumn<QSizeF>("pageSize");
    QTest::addColumn<QRectF>("expected");

    const QSizeF widePage(100, 50);
    const QSizeF tallPage(50, 100);
    QTest::newRow("fit centered") << ScaleMode::FitToPage << Alignment::Centered
        << widePage << QRectF(20, 60, 240, 120);
    QTest::newRow("fit vertical") << ScaleMode::FitToPage << Alignment::CenteredVertically
        << widePage << QRectF(20, 60, 240, 120);
    QTest::newRow("fit horizontal") << ScaleMode::FitToPage << Alignment::CenteredHorizontally
        << widePage << QRectF(20, 30, 240, 120);
    QTest::newRow("fit top left") << ScaleMode::FitToPage << Alignment::TopLeft
        << widePage << QRectF(20, 30, 240, 120);
    QTest::newRow("tall fit centered") << ScaleMode::FitToPage << Alignment::Centered
        << tallPage << QRectF(95, 30, 90, 180);
    QTest::newRow("tall fit vertical") << ScaleMode::FitToPage << Alignment::CenteredVertically
        << tallPage << QRectF(20, 30, 90, 180);
    QTest::newRow("tall fit horizontal") << ScaleMode::FitToPage << Alignment::CenteredHorizontally
        << tallPage << QRectF(95, 30, 90, 180);
    QTest::newRow("actual centered") << ScaleMode::ActualSize << Alignment::Centered
        << widePage << QRectF(90, 95, 100, 50);
    QTest::newRow("actual vertical") << ScaleMode::ActualSize << Alignment::CenteredVertically
        << widePage << QRectF(20, 95, 100, 50);
    QTest::newRow("actual horizontal") << ScaleMode::ActualSize << Alignment::CenteredHorizontally
        << widePage << QRectF(90, 30, 100, 50);
    QTest::newRow("actual top left") << ScaleMode::ActualSize << Alignment::TopLeft
        << widePage << QRectF(20, 30, 100, 50);
    QTest::newRow("custom centered") << ScaleMode::Custom << Alignment::Centered
        << widePage << QRectF(65, 82.5, 150, 75);
    QTest::newRow("custom vertical") << ScaleMode::Custom << Alignment::CenteredVertically
        << widePage << QRectF(20, 82.5, 150, 75);
    QTest::newRow("custom horizontal") << ScaleMode::Custom << Alignment::CenteredHorizontally
        << widePage << QRectF(65, 30, 150, 75);
    QTest::newRow("custom top left") << ScaleMode::Custom << Alignment::TopLeft
        << widePage << QRectF(20, 30, 150, 75);
}

void TstPrintLayout::pagePlacement()
{
    QFETCH(ScaleMode, scaleMode);
    QFETCH(Alignment, alignment);
    QFETCH(QSizeF, pageSize);
    QFETCH(QRectF, expected);
    Settings settings;
    settings.scaleMode = scaleMode;
    settings.scalePercent = 150;
    settings.alignment = alignment;

    // A paper origin with asymmetric printable margins must be counted once.
    QCOMPARE(destinationRect(pageSize, QRectF(20, 30, 240, 180), settings), expected);
}

void TstPrintLayout::oversizedPageKeepsPhysicalSize()
{
    Settings settings;
    settings.scaleMode = ScaleMode::ActualSize;
    QCOMPARE(destinationRect(QSizeF(400, 200), QRectF(10, 20, 200, 100), settings),
             QRectF(-90, -30, 400, 200));

    settings.scaleMode = ScaleMode::Custom;
    settings.scalePercent = 200;
    QCOMPARE(destinationRect(QSizeF(400, 200), QRectF(10, 20, 200, 100), settings),
             QRectF(-290, -130, 800, 400));

    settings.scaleMode = ScaleMode::FitToPage;
    QCOMPARE(destinationRect(QSizeF(400, 200), QRectF(10, 20, 200, 100), settings),
             QRectF(10, 20, 200, 100));
}

void TstPrintLayout::invalidDimensionsAreEmpty()
{
    const QRectF printable(10, 20, 200, 100);
    Settings settings;
    QVERIFY(destinationRect({}, printable, settings).isEmpty());
    QVERIFY(destinationRect(QSizeF(-10, 20), printable, settings).isEmpty());
    QVERIFY(destinationRect(QSizeF(100, 100), {}, settings).isEmpty());
    QVERIFY(destinationRect(QSizeF(std::numeric_limits<qreal>::infinity(), 100),
                            printable, settings).isEmpty());
    settings.scaleMode = ScaleMode::Custom;
    settings.scalePercent = 0;
    QVERIFY(destinationRect(QSizeF(100, 100), printable, settings).isEmpty());
    settings.scalePercent = -50;
    QVERIFY(destinationRect(QSizeF(100, 100), printable, settings).isEmpty());
}

void TstPrintLayout::colourAndGrayscaleRemainDistinct()
{
    QImage source(4, 1, QImage::Format_RGB32);
    source.setPixelColor(0, 0, Qt::red);
    source.setPixelColor(1, 0, Qt::green);
    source.setPixelColor(2, 0, Qt::blue);
    source.setPixelColor(3, 0, QColor(127, 127, 127));
    QCOMPARE(convertColour(source, ColourMode::Colour), source);

    const QImage grayscale = convertColour(source, ColourMode::Grayscale);
    QCOMPARE(grayscale.size(), source.size());
    for (int x = 0; x < source.width(); ++x) {
        const QColor pixel = grayscale.pixelColor(x, 0);
        QCOMPARE(pixel.red(), pixel.green());
        QCOMPARE(pixel.green(), pixel.blue());
        QCOMPARE(pixel.alpha(), 255);
        QVERIFY(pixel.red() > 0);
        QVERIFY(pixel.red() < 255);
    }
    QCOMPARE(grayscale.pixelColor(3, 0), QColor(127, 127, 127));
    QCOMPARE(source.pixelColor(0, 0), QColor(Qt::red));
    QVERIFY(convertColour({}, ColourMode::Grayscale).isNull());
}

void TstPrintLayout::blackAndWhiteThresholdAndTransparency()
{
    QImage source(6, 1, QImage::Format_ARGB32);
    source.setPixelColor(0, 0, QColor(127, 127, 127));
    source.setPixelColor(1, 0, QColor(128, 128, 128));
    source.setPixelColor(2, 0, QColor(0, 0, 0, 0));
    source.setPixelColor(3, 0, QColor(0, 0, 0, 127));
    source.setPixelColor(4, 0, QColor(0, 0, 0, 128));
    source.setPixelColor(5, 0, QColor(240, 20, 80, 64));
    const QImage grayscale = convertColour(source, ColourMode::Grayscale);
    const QImage binary = convertColour(source, ColourMode::BlackAndWhite);
    QCOMPARE(grayscale.pixelColor(2, 0), QColor(Qt::white));
    QCOMPARE(grayscale.pixelColor(3, 0), QColor(128, 128, 128));
    QCOMPARE(grayscale.pixelColor(4, 0), QColor(127, 127, 127));
    const int expected[] = {0, 255, 255, 255, 0, 255};
    for (int x = 0; x < source.width(); ++x)
        QCOMPARE(binary.pixelColor(x, 0), QColor(expected[x], expected[x], expected[x]));

    // Renderer images can have premultiplied alpha or byte-ordered RGBA storage.
    const QImage premultiplied = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QCOMPARE(convertColour(premultiplied, ColourMode::BlackAndWhite), binary);
    QCOMPARE(convertColour(source.convertToFormat(QImage::Format_RGBA8888),
                           ColourMode::BlackAndWhite), binary);
}

void TstPrintLayout::painterClipsAndPreservesState()
{
    QImage canvas(80, 80, QImage::Format_RGB32);
    canvas.fill(Qt::magenta);
    QImage source(2, 2, QImage::Format_RGB32);
    source.fill(Qt::red);
    Settings settings;
    settings.scaleMode = ScaleMode::ActualSize;

    QPainter painter(&canvas);
    painter.setClipRect(QRectF(30, 10, 40, 60));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QRectF oldClip = painter.clipBoundingRect();
    paintPage(painter, source, QSizeF(100, 80), QRectF(20, 30, 40, 20), settings);
    QCOMPARE(painter.clipBoundingRect(), oldClip);
    QVERIFY(!painter.testRenderHint(QPainter::SmoothPixmapTransform));
    painter.end();

    // The oversized page is clipped to both the printable area and caller clip.
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const bool painted = x >= 30 && x < 60 && y >= 30 && y < 50;
            QCOMPARE(canvas.pixelColor(x, y), QColor(painted ? Qt::red : Qt::magenta));
        }
    }
}

void TstPrintLayout::blackAndWhiteScalingKeepsBinaryPixels()
{
    QImage source(2, 1, QImage::Format_RGB32);
    source.setPixelColor(0, 0, QColor(50, 50, 50));
    source.setPixelColor(1, 0, QColor(200, 200, 200));
    QImage canvas(41, 21, QImage::Format_RGB32);
    canvas.fill(Qt::white);
    Settings settings;
    settings.colourMode = ColourMode::BlackAndWhite;
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    paintPage(painter, source, QSizeF(2, 1), QRectF(canvas.rect()), settings);
    QVERIFY(painter.testRenderHint(QPainter::SmoothPixmapTransform));
    painter.end();

    bool hasBlack = false;
    bool hasWhite = false;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const QColor pixel = canvas.pixelColor(x, y);
            QVERIFY(pixel == Qt::black || pixel == Qt::white);
            hasBlack |= pixel == Qt::black;
            hasWhite |= pixel == Qt::white;
        }
    }
    QVERIFY(hasBlack);
    QVERIFY(hasWhite);

    // The fitted screen preview may smooth the already thresholded image. This
    // changes only display sampling, while the print path above remains binary.
    QImage preview(canvas.size(), QImage::Format_RGB32);
    preview.fill(Qt::white);
    QPainter screen(&preview);
    paintPage(screen, source, QSizeF(2, 1), QRectF(preview.rect()), settings, true);
    screen.end();
    bool hasGray = false;
    for (int x = 0; x < preview.width(); ++x) {
        const int value = preview.pixelColor(x, preview.height() / 2).red();
        hasGray |= value > 0 && value < 255;
    }
    QVERIFY(hasGray);
    QCOMPARE(preview.pixelColor(0, preview.height() / 2), QColor(Qt::black));
    QCOMPARE(preview.pixelColor(preview.width() - 1, preview.height() / 2), QColor(Qt::white));
}

void TstPrintLayout::reducedPreviewRetainsFineLines()
{
    QImage source(120, 120, QImage::Format_RGB32);
    source.fill(Qt::white);
    for (int y = 0; y < source.height(); ++y)
        source.setPixelColor(1, y, Qt::black);
    QImage preview(12, 12, QImage::Format_RGB32);
    preview.fill(Qt::white);
    Settings settings;
    settings.colourMode = ColourMode::BlackAndWhite;
    QPainter painter(&preview);
    paintPage(painter, source, QSizeF(120, 120), QRectF(preview.rect()), settings, true);
    painter.end();
    // A one-pixel source stroke occupies a tenth of the first preview pixel.
    // It must remain visible instead of vanishing between bilinear samples.
    const int stroke = preview.pixelColor(0, 6).red();
    QVERIFY(stroke > 200 && stroke < 250);
    QCOMPARE(preview.pixelColor(1, 6), QColor(Qt::white));
}

QTEST_GUILESS_MAIN(TstPrintLayout)
#include "tst_print_layout.moc"
