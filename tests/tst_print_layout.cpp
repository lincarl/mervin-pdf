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
    void rasterFollowsPrintedSize_data();
    void rasterFollowsPrintedSize();
    void invalidRasterPlansAreEmpty();
    void rasterBandsSplitWholeRows_data();
    void rasterBandsSplitWholeRows();
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

void TstPrintLayout::rasterFollowsPrintedSize_data()
{
    QTest::addColumn<QSizeF>("page");
    QTest::addColumn<QRectF>("printable");
    QTest::addColumn<ScaleMode>("scaleMode");
    QTest::addColumn<int>("percent");
    QTest::addColumn<QSize>("expectedSize");

    const QSizeF a1(1684, 2384);
    const QSizeF a4(595, 842);
    const QRectF fullSheet(0, 0, 595, 842);
    const QRectF margins(20, 30, 555, 782);
    // 300 dpi of the A4 sheet, which the A1 page fills in its height.
    QTest::newRow("fit A1 to A4") << a1 << fullSheet << ScaleMode::FitToPage << 100
        << QSize(2479, 3509);
    QTest::newRow("actual size on a matching sheet") << a4 << fullSheet << ScaleMode::ActualSize
        << 100 << QSize(2480, 3509);
    // Only the printable area of an oversized page reaches the raster.
    QTest::newRow("actual size of A1 on A4") << a1 << margins << ScaleMode::ActualSize << 100
        << QSize(2313, 3259);
    QTest::newRow("custom 400% on A4") << a4 << margins << ScaleMode::Custom << 400
        << QSize(2313, 3259);
    QTest::newRow("custom 50% on A4") << a4 << margins << ScaleMode::Custom << 50
        << QSize(1240, 1755);
}

// Checks print rasters keep the quality on paper and cover only the visible page.
void TstPrintLayout::rasterFollowsPrintedSize()
{
    QFETCH(QSizeF, page);
    QFETCH(QRectF, printable);
    QFETCH(ScaleMode, scaleMode);
    QFETCH(int, percent);
    QFETCH(QSize, expectedSize);
    Settings settings;
    settings.scaleMode = scaleMode;
    settings.scalePercent = percent;
    constexpr double dpi = 300;
    const RasterPlan plan = rasterPlan(page, printable, settings, dpi);
    const QRectF destination = destinationRect(page, printable, settings);
    QCOMPARE(plan.destination, destination);
    // Raster pixels per page point give `dpi` on paper whatever the page's scale.
    QVERIFY(qFuzzyCompare(plan.scale * page.width() / destination.width(), dpi / 72));
    QVERIFY2(std::abs(plan.source.width() - expectedSize.width()) <= 1
                 && std::abs(plan.source.height() - expectedSize.height()) <= 1,
             qPrintable(QString("Raster %1 x %2").arg(plan.source.width())
                            .arg(plan.source.height())));

    // The source rounds the visible page out to whole pixels, which land on it.
    const QRectF visible = destination & printable;
    const QRectF target = rasterTarget(plan.destination, page, plan.scale, plan.source);
    const double pixel = 72 / dpi;
    QVERIFY(target.left() <= visible.left() + 1e-9 && target.left() > visible.left() - pixel);
    QVERIFY(target.top() <= visible.top() + 1e-9 && target.top() > visible.top() - pixel);
    QVERIFY(target.right() >= visible.right() - 1e-9 && target.right() < visible.right() + pixel);
    QVERIFY(target.bottom() >= visible.bottom() - 1e-9
            && target.bottom() < visible.bottom() + pixel);

    // A centred page that overflows the sheet keeps the middle of its raster.
    const QSizeF full = destination.size() * dpi / 72;
    QVERIFY(plan.source.left() >= 0 && plan.source.top() >= 0);
    QVERIFY(plan.source.right() < std::ceil(full.width()));
    QVERIFY(plan.source.bottom() < std::ceil(full.height()));
    QVERIFY(std::abs(QRectF(plan.source).center().x() - full.width() / 2) <= 1);
    QVERIFY(std::abs(QRectF(plan.source).center().y() - full.height() / 2) <= 1);
}

// Checks invalid geometry and resolution give no raster to render.
void TstPrintLayout::invalidRasterPlansAreEmpty()
{
    const QRectF printable(10, 20, 200, 100);
    Settings settings;
    QVERIFY(!rasterPlan(QSizeF(100, 100), printable, settings, 300).isEmpty());
    QVERIFY(rasterPlan({}, printable, settings, 300).isEmpty());
    QVERIFY(rasterPlan(QSizeF(100, 100), {}, settings, 300).isEmpty());
    QVERIFY(rasterPlan(QSizeF(100, 100), printable, settings, 0).isEmpty());
    QVERIFY(rasterPlan(QSizeF(100, 100), printable, settings, -300).isEmpty());
    QVERIFY(rasterPlan(QSizeF(100, 100), printable, settings,
                       std::numeric_limits<double>::infinity()).isEmpty());
    // MuPDF draws a page raster correctly only up to 2^20 pixels on a side, so a page
    // that would reach one pixel further has no plan, even with a small visible part.
    settings.scaleMode = ScaleMode::Custom;
    settings.scalePercent = 400;
    QVERIFY(!rasterPlan(QSizeF(262144, 100), printable, settings, 72).isEmpty());
    QVERIFY(rasterPlan(QSizeF(262144.25, 100), printable, settings, 72).isEmpty());
    QVERIFY(rasterPlan(QSizeF(100, 262144.25), printable, settings, 72).isEmpty());
    // The largest page without a UserUnit at 400% and 1200 dpi still plans its visible
    // part.
    const RasterPlan large = rasterPlan(QSizeF(14400, 14400), printable, settings, 1200);
    QCOMPARE(large.source.size(), QSize(3334, 1668));
}

void TstPrintLayout::rasterBandsSplitWholeRows_data()
{
    QTest::addColumn<QRect>("source");
    QTest::addColumn<qint64>("maxPixels");
    QTest::addColumn<int>("count");

    QTest::newRow("fits one band") << QRect(10, 20, 100, 50) << qint64(5000) << 1;
    QTest::newRow("exact budget") << QRect(0, 0, 100, 100) << qint64(2500) << 4;
    QTest::newRow("uneven rows") << QRect(5, 7, 100, 101) << qint64(2500) << 5;
    QTest::newRow("row wider than budget") << QRect(0, 0, 1000, 3) << qint64(10) << 3;
    // A0 at 600 dpi under the print path's 2^24-pixel band budget.
    QTest::newRow("A0 at 600 dpi") << QRect(0, 0, 19867, 28087) << (qint64(1) << 24) << 34;
    QTest::newRow("empty") << QRect() << qint64(5000) << 0;
}

// Checks bands tile the source exactly, without gaps, overlap or excess pixels.
void TstPrintLayout::rasterBandsSplitWholeRows()
{
    QFETCH(QRect, source);
    QFETCH(qint64, maxPixels);
    QFETCH(int, count);
    const QList<QRect> bands = rasterBands(source, maxPixels);
    QCOMPARE(bands.size(), count);
    int top = source.top();
    int shortest = std::numeric_limits<int>::max();
    int tallest = 0;
    for (const QRect &band : bands) {
        QCOMPARE(band.left(), source.left());
        QCOMPARE(band.width(), source.width());
        QCOMPARE(band.top(), top);
        QVERIFY(band.height() > 0);
        QVERIFY(band.height() == 1 || qint64(band.width()) * band.height() <= maxPixels);
        shortest = std::min(shortest, band.height());
        tallest = std::max(tallest, band.height());
        top = band.bottom() + 1;
    }
    if (!bands.isEmpty()) {
        QCOMPARE(top, source.bottom() + 1);
        QVERIFY(tallest - shortest <= 1);
    }
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

    // Renders arrive as RGB888, whose padded rows must convert like the same pixels
    // in RGB32.
    QImage rows(5, 3, QImage::Format_RGB32);
    for (int y = 0; y < rows.height(); ++y)
        for (int x = 0; x < rows.width(); ++x)
            rows.setPixelColor(x, y, QColor(40 * x, 90 * y, 200 - 30 * x));
    const QImage rgb = rows.convertToFormat(QImage::Format_RGB888);
    QVERIFY(rgb.bytesPerLine() > rgb.width() * 3);
    for (ColourMode mode : {ColourMode::Grayscale, ColourMode::BlackAndWhite})
        QCOMPARE(convertColour(rgb, mode), convertColour(rows, mode));
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
    const QRectF printable(20, 30, 40, 20);
    drawImage(painter, source, destinationRect(QSizeF(100, 80), printable, settings), printable,
              settings.colourMode);
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
    const QImage binary = convertColour(source, settings.colourMode);
    const QRectF destination = destinationRect(QSizeF(2, 1), QRectF(canvas.rect()), settings);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    drawImage(painter, binary, destination, QRectF(canvas.rect()), settings.colourMode);
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
    drawImage(screen, binary, destination, QRectF(preview.rect()), settings.colourMode, true);
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
    const QImage reduced =
        reduceToDevice(convertColour(source, ColourMode::BlackAndWhite), preview.size());
    QCOMPARE(reduced.size(), preview.size());
    QPainter painter(&preview);
    drawImage(painter, reduced, QRectF(preview.rect()), QRectF(preview.rect()),
              ColourMode::BlackAndWhite, true);
    painter.end();
    // A one-pixel source stroke occupies a tenth of the first preview pixel.
    // It must remain visible instead of vanishing between bilinear samples.
    const int stroke = preview.pixelColor(0, 6).red();
    QVERIFY(stroke > 200 && stroke < 250);
    QCOMPARE(preview.pixelColor(1, 6), QColor(Qt::white));
}

QTEST_GUILESS_MAIN(TstPrintLayout)
#include "tst_print_layout.moc"
