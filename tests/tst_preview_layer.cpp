#include "render/PreviewLayer.h"

#include <QImage>
#include <QtTest>

// Pins PreviewLayer, the frozen last-good page images the viewer stretches over
// the gap while a zoom re-renders (see PreviewLayer.h and
// ViewerWidget::drawPreview). Every case here is written to FAIL for one of the
// plausible wrong implementations:
//  - storing the covered region in pixels instead of as a fraction of the page
//    rect (the tile would be drawn at its old size on the new layout);
//  - clamping a region that overhangs the page while keeping the whole image
//    (image and fraction would disagree, squashing the draw);
//  - storing the render cache's RGB888 image as-is, which the raster engine has
//    no fast transformed-blend path for (3-4x slower per frame), or keeping a
//    deep-zoom tile at full resolution;
//  - a tile for a neighbouring page taking the budget the current page needs;
//  - handing the raster engine the full target rect at deep zoom instead of the
//    visible slice, or deriving the source slice in device-independent pixels.
class TstPreviewLayer : public QObject
{
    Q_OBJECT

private slots:
    void wholePageTileFillsTheRescaledRect();
    void bandTileKeepsItsPlaceOnTheRescaledPage();
    void regionOutsideThePageIsRefused();
    void degenerateInputsAreIgnored();
    void tilesAreReEncodedForRepeatedBlitting();
    void theNewestTileWins();
    void replacingATileDoesNotDoubleCountBytes();
    void budgetRefusesExtraTilesButNeverTheFirst();
    void freedBudgetIsSpendableAgain();
    void eraseRetainAndClear();
    void clipToViewportDerivesTheMatchingSourceSlice();
    void clipToViewportUsesRawImagePixels();
    void clipToViewportSurvivesAnExtremeStretch();
    void clipToViewportRejectsWhatIsOffScreen();
};

namespace {

// A stand-in for a rendered page image; only its size matters here.
QImage tileImage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB888);
    img.fill(Qt::white);
    return img;
}

} // namespace

// A normal (non-clipped) render covers the whole page, so it must land exactly
// on the page's rect at any later scale - no drift, no old-size remnant.
void TstPreviewLayer::wholePageTileFillsTheRescaledRect()
{
    const QRect atOne(16, 16, 600, 800); // page rect when the image was rendered
    mervin::PreviewLayer layer;
    QVERIFY(layer.add(3, tileImage(600, 800), atOne, atOne));

    const mervin::PreviewLayer::Tile *t = layer.tile(3);
    QVERIFY(t != nullptr);
    QCOMPARE(t->frac, QRectF(0.0, 0.0, 1.0, 1.0));

    // Zoomed in 1.25x, then out to 0.5x: the stretch tracks the page rect.
    QCOMPARE(mervin::PreviewLayer::targetRect(*t, QRect(16, 16, 750, 1000)),
             QRectF(16, 16, 750, 1000));
    QCOMPARE(mervin::PreviewLayer::targetRect(*t, QRect(16, 16, 300, 400)),
             QRectF(16, 16, 300, 400));
    // A page whose x shifts because the widest page in the layout changed still
    // maps onto its own rect.
    QCOMPARE(mervin::PreviewLayer::targetRect(*t, QRect(120, 940, 600, 800)),
             QRectF(120, 940, 600, 800));

    QVERIFY(!layer.isEmpty());
    QCOMPARE(layer.tileCount(), 1);
    QVERIFY(layer.tile(4) == nullptr);
}

// A deep-zoom render covers only a band of the page. The band has to stay over
// the same page content after the zoom, which is what the fractional
// bookkeeping buys: at 2x the band's offset and height double with the page.
void TstPreviewLayer::bandTileKeepsItsPlaceOnTheRescaledPage()
{
    const QRect page(16, 16, 600, 800);
    const QRect band(16, 216, 600, 400); // 200 px into the page, 400 tall

    mervin::PreviewLayer layer;
    QVERIFY(layer.add(0, tileImage(600, 400), band, page));
    const mervin::PreviewLayer::Tile *t = layer.tile(0);
    QVERIFY(t != nullptr);
    QCOMPARE(t->frac, QRectF(0.0, 0.25, 1.0, 0.5));

    // Doubling the scale doubles the band's offset from the page top (200 -> 400)
    // and its height (400 -> 800). Storing pixels instead of fractions would
    // leave it at y=216 with height 400.
    QCOMPARE(mervin::PreviewLayer::targetRect(*t, QRect(16, 16, 1200, 1600)),
             QRectF(16, 416, 1200, 800));
}

// The stored fraction stands for the whole image, so a region reaching outside
// the page must be refused rather than clamped - clamping the rect while keeping
// the full image would draw it squashed.
void TstPreviewLayer::regionOutsideThePageIsRefused()
{
    const QRect page(16, 16, 600, 800);
    mervin::PreviewLayer layer;

    QVERIFY(!layer.add(0, tileImage(600, 400), QRect(16, 616, 600, 400), page)); // past the bottom
    QVERIFY(!layer.add(0, tileImage(600, 400), QRect(16, -184, 600, 400), page)); // above the top
    QVERIFY(!layer.add(0, tileImage(700, 800), QRect(16, 16, 700, 800), page));   // wider than the page
    QVERIFY(layer.isEmpty());
}

void TstPreviewLayer::degenerateInputsAreIgnored()
{
    const QRect page(16, 16, 600, 800);
    mervin::PreviewLayer layer;

    QVERIFY(!layer.add(0, QImage(), page, page));               // no image
    QVERIFY(!layer.add(1, tileImage(600, 800), page, QRect())); // page not laid out
    QVERIFY(!layer.add(2, tileImage(600, 800), QRect(), page)); // nothing covered
    QVERIFY(layer.isEmpty());
    QCOMPARE(layer.bytes(), 0);

    // targetRect must survive a page that is not laid out (Single page mode
    // leaves every other page with an empty rect).
    mervin::PreviewLayer::Tile t;
    t.frac = QRectF(0, 0, 1, 1);
    QVERIFY(mervin::PreviewLayer::targetRect(t, QRect()).isNull());
}

// A stored tile is re-blitted on every frame until the sharp render lands, so it
// is converted to the format the raster engine can stretch quickly and capped in
// size. The page fraction must survive both untouched.
void TstPreviewLayer::tilesAreReEncodedForRepeatedBlitting()
{
    const QRect page(0, 0, 600, 800);
    mervin::PreviewLayer layer;

    // A modest render keeps its resolution but changes format.
    QVERIFY(layer.add(0, tileImage(600, 800), page, page));
    QCOMPARE(layer.tile(0)->image.format(), QImage::Format_RGB32);
    QCOMPARE(layer.tile(0)->image.size(), QSize(600, 800));

    // A deep-zoom render (here 4000x3000 == 12 MPx, over the 8 MPx cap) is
    // downscaled, keeping its aspect ratio to within a pixel.
    mervin::PreviewLayer big;
    QVERIFY(big.add(0, tileImage(4000, 3000), page, page));
    const QImage &t = big.tile(0)->image;
    QCOMPARE(t.format(), QImage::Format_RGB32);
    QVERIFY2(qint64(t.width()) * t.height() <= mervin::PreviewLayer::kMaxTilePixels,
             "tile was stored above the pixel cap");
    QVERIFY(t.width() > 3000); // not downscaled more than needed
    QVERIFY(qAbs(double(t.width()) / t.height() - 4000.0 / 3000.0) < 0.01);
    QCOMPARE(big.tile(0)->frac, QRectF(0.0, 0.0, 1.0, 1.0));
    QCOMPARE(big.bytes(), qint64(t.sizeInBytes()));
}

// The newest render is always the better stand-in: it comes from the scale we are
// leaving, so it is drawn at about the magnification of one zoom step. Keeping an
// older tile because it covers more of the page would pin a stand-in that grows
// more magnified with every step and can never be replaced.
void TstPreviewLayer::theNewestTileWins()
{
    const QRect page(0, 0, 600, 800);
    mervin::PreviewLayer layer;
    QVERIFY(layer.add(2, tileImage(600, 800), page, page));
    QCOMPARE(layer.tile(2)->frac, QRectF(0.0, 0.0, 1.0, 1.0));

    // A deep-zoom band replaces the whole-page tile, and brings its own geometry.
    QVERIFY(layer.add(2, tileImage(600, 100), QRect(0, 300, 600, 100), page));
    QCOMPARE(layer.tileCount(), 1);
    QCOMPARE(layer.tile(2)->frac, QRectF(0.0, 0.375, 1.0, 0.125));
}

void TstPreviewLayer::replacingATileDoesNotDoubleCountBytes()
{
    const QRect page(0, 0, 100, 100);

    mervin::PreviewLayer layer;
    QVERIFY(layer.add(7, tileImage(100, 100), page, page));
    const qint64 first = layer.bytes();
    QVERIFY(first > 0);
    QVERIFY(layer.add(7, tileImage(50, 50), page, page)); // same coverage, fewer pixels
    QCOMPARE(layer.tileCount(), 1);
    QCOMPARE(layer.bytes(), qint64(layer.tile(7)->image.sizeInBytes()));
    QVERIFY(layer.bytes() < first);
}

void TstPreviewLayer::budgetRefusesExtraTilesButNeverTheFirst()
{
    const QRect page(0, 0, 40, 40);
    const QImage img = tileImage(40, 40);
    const qint64 n = 40 * 40 * 4; // stored as RGB32

    // Room for two tiles.
    mervin::PreviewLayer layer(2 * n);
    QVERIFY(layer.add(0, img, page, page));
    QVERIFY(layer.add(1, img, page, page));
    QVERIFY(!layer.add(2, img, page, page)); // over budget: dropped, evicts nothing
    QCOMPARE(layer.tileCount(), 2);
    QCOMPARE(layer.bytes(), 2 * n);
    QVERIFY(layer.tile(0) != nullptr);
    QVERIFY(layer.tile(2) == nullptr);

    // The page the user is looking at must always get a preview, even where one
    // deep-zoom tile is larger than the whole budget on its own. The viewer
    // relies on this: it rebuilds the layer with that page first, so a
    // neighbour's tile can never be the one holding the budget.
    mervin::PreviewLayer tiny(1);
    QVERIFY(tiny.add(5, img, page, page));
    QCOMPARE(tiny.tileCount(), 1);
    QCOMPARE(tiny.bytes(), n);
}

void TstPreviewLayer::freedBudgetIsSpendableAgain()
{
    const QRect page(0, 0, 40, 40);
    const QImage img = tileImage(40, 40);
    const qint64 n = 40 * 40 * 4;

    mervin::PreviewLayer layer(2 * n);
    QVERIFY(layer.add(0, img, page, page));
    QVERIFY(layer.add(1, img, page, page));
    QVERIFY(!layer.add(2, img, page, page));
    layer.erase(0);
    QVERIFY2(layer.add(2, img, page, page), "bytes freed by erase() were not reusable");
    QCOMPARE(layer.bytes(), 2 * n);
}

void TstPreviewLayer::eraseRetainAndClear()
{
    const QRect page(0, 0, 40, 40);
    const QImage img = tileImage(40, 40);
    const qint64 n = 40 * 40 * 4;

    mervin::PreviewLayer layer;
    for (int p : {0, 1, 2, 3})
        QVERIFY(layer.add(p, img, page, page));
    QCOMPARE(layer.bytes(), 4 * n);

    layer.erase(2);
    layer.erase(2); // idempotent
    QCOMPARE(layer.tileCount(), 3);
    QCOMPARE(layer.bytes(), 3 * n);
    QVERIFY(layer.tile(2) == nullptr);

    // Pruning to the pages still worth keeping (the viewer does this every paint)
    // must give the bytes back, or a page that scrolled away would hold the
    // budget for good.
    layer.retain({1, 3, 9}); // 9 is not held; 0 is dropped
    QCOMPARE(layer.tileCount(), 2);
    QCOMPARE(layer.bytes(), 2 * n);
    QVERIFY(layer.tile(0) == nullptr);
    QVERIFY(layer.tile(1) != nullptr);
    QVERIFY(layer.tile(3) != nullptr);

    layer.clear();
    QVERIFY(layer.isEmpty());
    QCOMPARE(layer.bytes(), 0);
}

// The deep-zoom safety net: the stretch target can be far larger than the
// window, so only the part inside it is drawn - with the source slice that
// matches.
void TstPreviewLayer::clipToViewportDerivesTheMatchingSourceSlice()
{
    const QRectF target(-1000, -2000, 10000, 20000); // page rect, scrolled off
    const QImage image = tileImage(500, 1000);
    const QRectF clip(0, 0, 800, 600);

    QRectF vis;
    QRectF src;
    QVERIFY(mervin::PreviewLayer::clipToViewport(target, image, clip, &vis, &src));
    QCOMPARE(vis, QRectF(0, 0, 800, 600));
    // 500/10000 == 1/20 image px per target px, so 1000 px of skipped target is
    // 50 image px, and the 800x600 window samples a 40x30 slice.
    QCOMPARE(src, QRectF(50, 100, 40, 30));

    // Fully visible: the whole image is used, un-clipped.
    QVERIFY(mervin::PreviewLayer::clipToViewport(QRectF(10, 20, 200, 400), image, clip, &vis, &src));
    QCOMPARE(vis, QRectF(10, 20, 200, 400));
    QCOMPARE(src, QRectF(0, 0, 500, 1000));
}

// QPainter's explicit-source drawImage overload does not apply the image's
// devicePixelRatio, so the slice must be in raw pixels. Deriving it from
// deviceIndependentSize() would sample a quarter of a 2x tile.
void TstPreviewLayer::clipToViewportUsesRawImagePixels()
{
    QImage hidpi = tileImage(1600, 2000);
    hidpi.setDevicePixelRatio(2.0);

    QRectF vis;
    QRectF src;
    QVERIFY(mervin::PreviewLayer::clipToViewport(QRectF(0, 0, 800, 1000), hidpi,
                                                 QRectF(0, 0, 400, 500), &vis, &src));
    QCOMPARE(vis, QRectF(0, 0, 400, 500));
    QCOMPARE(src, QRectF(0, 0, 800, 1000)); // not 400x500
}

// Arithmetic robustness at the extreme end (beyond what the viewer will actually
// draw, which caps the magnification): the slice may come out well under a pixel
// and must not collapse to nothing.
void TstPreviewLayer::clipToViewportSurvivesAnExtremeStretch()
{
    QRectF vis;
    QRectF src;
    QVERIFY(mervin::PreviewLayer::clipToViewport(QRectF(16, 16, 512000, 640000), tileImage(600, 800),
                                                 QRectF(0, 0, 1000, 800), &vis, &src));
    QCOMPARE(vis, QRectF(16, 16, 984, 784));
    QCOMPARE(src, QRectF(0.0, 0.0, 1.153125, 0.98)); // 984*600/512000, 784*800/640000
    QVERIFY2(src.width() > 0.0 && src.height() > 0.0,
             "sub-pixel source slice collapsed - the preview would vanish");
}

void TstPreviewLayer::clipToViewportRejectsWhatIsOffScreen()
{
    QRectF vis;
    QRectF src;
    const QImage image = tileImage(100, 100);
    const QRectF clip(0, 0, 800, 600);

    QVERIFY(!mervin::PreviewLayer::clipToViewport(QRectF(900, 0, 100, 100), image, clip, &vis, &src));
    QVERIFY(!mervin::PreviewLayer::clipToViewport(QRectF(0, 0, 0, 100), image, clip, &vis, &src));
    QVERIFY(!mervin::PreviewLayer::clipToViewport(QRectF(0, 0, 100, 100), QImage(), clip, &vis, &src));
}

QTEST_GUILESS_MAIN(TstPreviewLayer)
#include "tst_preview_layer.moc"
