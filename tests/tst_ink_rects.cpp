// End-to-end coverage for the Comfort theme's image handling: the render
// worker walks the display list for raster-image bboxes and
// applyComfortTransform switches treatment at their edges - per-image modes
// inside (PhotoOnWhite for a picture with a white/transparent backdrop,
// KeepAuthored when there is no backdrop to remove, Split for ink on white
// and for scanned pages), ink outside (halo-free coloured text).
// Renders real pages through the worker pool, so the ImageRectDevice walk,
// the embedded-image probe, the page-coverage test, the overlay demotion, the
// rect offset maths and the row-splitting all run.
//
// The picture cases below deliberately probe DARK image content: a dark pixel
// that stays dark is the one thing the old photo-first classifier could not
// produce - it inverted every image it did not recognise as a photo, so these
// assertions fail against it rather than passing either way.
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "render/RenderTypes.h"

#include <QImage>
#include <QtTest>

class TstInkRects : public QObject
{
    Q_OBJECT

private slots:
    void yellowPhotoKeptOnDarkPage();
    void tintedPhotoKeptAuthored();
    void greyPhotoShownAuthoredWithoutBackdrop();
    void darkSubjectPhotosKeepTheirColours();
    void textOverAPictureReadsAsPageText();
    void scannedPageStillInverts();

private:
    mervin::RenderResult renderComfort(const QString &pdf, int pageNo);
};

mervin::RenderResult TstInkRects::renderComfort(const QString &pdf, int pageNo)
{
    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(pdf, QString(), &err);
    if (!doc) {
        qWarning("openDocument failed: %s", qPrintable(err));
        return {};
    }

    mervin::RenderResult got;
    bool received = false;
    connect(
        &engine, &mervin::RenderEngine::resultReady, this,
        [&](const mervin::RenderResult &r) {
            got = r;
            received = true;
        },
        Qt::QueuedConnection);

    mervin::RenderRequest req;
    req.document = doc.get();
    req.requester = 1;
    req.pageNo = pageNo;
    req.scale = 150.0 / 72.0; // device px == the 150 dpi coordinates below
    req.theme = mervin::PageTheme::Comfort;
    req.token = 1;
    engine.submit(req);

    // QTRY_* macros only work in void test slots; wait manually. A timeout
    // leaves got.ok == false, which the callers assert on.
    QElapsedTimer t;
    t.start();
    while (!received && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    engine.shutdown();
    return got;
}

void TstInkRects::yellowPhotoKeptOnDarkPage()
{
    const QString pdf = QStringLiteral(MERVIN_IMAGES_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/images.pdf not present");

    // Page 5 (index 4) at 150 dpi: a distinctly yellow 3D product photo sits
    // at device rect x 919-1143, y 222-358 on an otherwise white page.
    const mervin::RenderResult got = renderComfort(pdf, 4);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());
    QVERIFY(got.image.width() > 1100 && got.image.height() > 1600);

    // White page background outside every image rect: the ink treatment is
    // bit-exact with the baseline comfort negative on neutral pixels, so
    // white paper must land exactly on the comfort background.
    QCOMPARE(got.image.pixelColor(100, 100), QColor(24, 26, 30));

    // Inside the photo's rect the soft-oklab treatment keeps the yellow body
    // at its authored colour (~(216,216,156)). Without the rect switch the
    // ink treatment would render this pixel dark olive (~(77,79,21)), and the
    // baseline negative would render it purple-blue - both fail these bounds.
    const QColor body = got.image.pixelColor(1030, 290);
    QVERIFY2(body.red() > 180 && body.green() > 180,
             qPrintable(QStringLiteral("photo body not kept bright: %1,%2,%3")
                            .arg(body.red()).arg(body.green()).arg(body.blue())));
    QVERIFY2(body.blue() < 180 && body.red() - body.blue() > 40,
             qPrintable(QStringLiteral("photo body lost its yellow chroma: %1,%2,%3")
                            .arg(body.red()).arg(body.green()).arg(body.blue())));

    // A pixel between the two photos - outside both image rects, on the
    // white page - must invert to the dark background as always.
    const QColor backdrop = got.image.pixelColor(1000, 420);
    QVERIFY2(backdrop.lightness() < 80,
             qPrintable(QStringLiteral("neutral content did not invert: %1,%2,%3")
                            .arg(backdrop.red()).arg(backdrop.green()).arg(backdrop.blue())));
}

void TstInkRects::tintedPhotoKeptAuthored()
{
    const QString pdf = QStringLiteral(MERVIN_IMAGES2_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/images2.pdf not present");

    // Page 1 at 150 dpi: a product photo on a TINTED blue-grey backdrop at
    // device rect x 94-555, y 212-520. Its pixels sit largely in the chroma
    // transition band, so the per-pixel gate would tear it into kept and
    // inverted blotches. There is no white backdrop to remove either, so the
    // per-image decision must leave it completely untouched.
    const mervin::RenderResult got = renderComfort(pdf, 0);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());
    QVERIFY(got.image.width() > 1200 && got.image.height() > 1700);

    // The photo's blue-grey backdrop renders exactly as authored
    // ((106,123,149) at 150 dpi). Split treatment would invert or blotch it.
    const QColor bd = got.image.pixelColor(150, 250);
    QVERIFY2(qAbs(bd.red() - 106) <= 3 && qAbs(bd.green() - 123) <= 3
                 && qAbs(bd.blue() - 149) <= 3,
             qPrintable(QStringLiteral("tinted photo not kept authored: %1,%2,%3")
                            .arg(bd.red()).arg(bd.green()).arg(bd.blue())));

    // The page margin outside every image still inverts to the comfort
    // background.
    QCOMPARE(got.image.pixelColor(60, 600), QColor(24, 26, 30));

    // The greyscale title-banner image above it (x 86-759, y 41-133) is a
    // picture on a white backdrop, so it gets the other half of the rule: its
    // white vanishes into the page while its lettering keeps the authored
    // tone. Both probes discriminate - Split, which this image used to get,
    // inverts the lettering to light grey and the backdrop lands on the page
    // either way.
    // Accepted trade-off, pinned so a future change to it is a decision and not
    // a surprise: this banner is a picture OF text (soft-shadowed lettering on
    // white), so showing it authored reads embossed rather than clean. It stays
    // legible, and no feature separates it from a product photo without
    // overfitting - its shading score is higher than any real photo's.
    QCOMPARE(got.image.pixelColor(171, 48), QColor(24, 26, 30)); // backdrop gone
    const QColor letter = got.image.pixelColor(700, 60);         // authored (148,144,141)
    QVERIFY2(letter.lightness() > 130,
             qPrintable(QStringLiteral("banner lettering not kept authored: %1,%2,%3")
                            .arg(letter.red()).arg(letter.green()).arg(letter.blue())));
}

void TstInkRects::greyPhotoShownAuthoredWithoutBackdrop()
{
    const QString pdf = QStringLiteral(MERVIN_IMAGES_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/images.pdf not present");

    // Page 1 at 150 dpi: greyscale 3D product renders on white backdrops
    // (e.g. device rect x 581-767, y 307-469). The PhotoOnWhite mode must
    // show the photo authored - no inversion - while its white backdrop
    // disappears into the dark page instead of glaring as a box.
    const mervin::RenderResult got = renderComfort(pdf, 0);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());

    // Housing body: authored light grey ((230,230,230) at 150 dpi). The old
    // split treatment inverted this pixel to ~(43,45,49).
    const QColor body = got.image.pixelColor(650, 380);
    QVERIFY2(qAbs(body.red() - 230) <= 3 && qAbs(body.green() - 230) <= 3
                 && qAbs(body.blue() - 230) <= 3,
             qPrintable(QStringLiteral("photo not shown authored: %1,%2,%3")
                            .arg(body.red()).arg(body.green()).arg(body.blue())));

    // Pure-white backdrop INSIDE the photo rect lands exactly on the page
    // background - the white box is gone.
    QCOMPARE(got.image.pixelColor(680, 320), QColor(24, 26, 30));
    // And the page margin outside every rect still inverts as always.
    QCOMPARE(got.image.pixelColor(100, 100), QColor(24, 26, 30));

    // The third photo on this page (device rect x 921-1151, y 324-456) is a
    // CMYK photo of a grey stamped strip, and it is the case that proves the
    // ramp has to be chosen per image: lossy compression left a band of
    // near-white pixels hugging the part, and with this page's gentle ramp -
    // correct for the white housings above - that band survived as a ragged
    // bright halo around the whole silhouette. Its own near-white content is
    // 0.5% of the rect, so the aggressive dead zone costs it nothing.
    int bright22 = 0, total22 = 0;
    for (int y = 324; y < 456; ++y)
        for (int x = 921; x < 1151; ++x) {
            const QColor c = got.image.pixelColor(x, y);
            bright22 += std::min({c.red(), c.green(), c.blue()}) >= 235;
            ++total22;
        }
    const double halo = double(bright22) / total22;
    QVERIFY2(halo < 0.005,
             qPrintable(QStringLiteral("halo around the CMYK photo: %1 bright").arg(halo)));
    // ...and the strip itself is still the authored photo, not a negative.
    const QColor strip = got.image.pixelColor(1001, 338);
    QVERIFY2(qAbs(strip.red() - 146) <= 3 && qAbs(strip.green() - 151) <= 3
                 && qAbs(strip.blue() - 145) <= 3,
             qPrintable(QStringLiteral("CMYK photo not kept authored: %1,%2,%3")
                            .arg(strip.red()).arg(strip.green()).arg(strip.blue())));

    // The calibrated ramp keeps the housing's brightest faces (values
    // 246-253, the "bleed" report): with these clean backdrops the ramp is
    // (1, 5), leaving ~1.4% of the rect bright; the fixed dead-zone ramps
    // erased virtually all of it (0.0%). Statistical, so it is robust to
    // sub-pixel rendering drift.
    int bright = 0, total = 0;
    for (int y = 307; y < 469; ++y)
        for (int x = 581; x < 767; ++x) {
            const QColor c = got.image.pixelColor(x, y);
            const int mn = std::min({c.red(), c.green(), c.blue()});
            bright += mn >= 235;
            ++total;
        }
    const double frac = double(bright) / total;
    QVERIFY2(frac > 0.008 && frac < 0.10,
             qPrintable(QStringLiteral("bright-face fraction %1 outside [0.008, 0.10]")
                            .arg(frac)));
}

void TstInkRects::darkSubjectPhotosKeepTheirColours()
{
    const QString pdf = QStringLiteral(MERVIN_PCBA_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/pcba.pdf not present");

    // Page 1 at 150 dpi. This assembly drawing is the document that exposed
    // the photo-first classifier: wide crops of a BLACK moulded part (device
    // rect x 1781-2339, y 435-631) and flat-shaded CAD renders of a green PCB
    // (x 268-1025, y 985-1362). Every one of them used to come out as a
    // negative - the part turned near-white, the board's dark areas glowed.
    const mervin::RenderResult got = renderComfort(pdf, 0);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());
    QVERIFY(got.image.width() > 2400 && got.image.height() > 1700);

    // The part's body: authored near-black ((9,10,15) at 150 dpi). The Split
    // treatment inverted this pixel to ~(207,210,215).
    const QColor body = got.image.pixelColor(2237, 456);
    QVERIFY2(body.lightness() < 60,
             qPrintable(QStringLiteral("black part not kept authored: %1,%2,%3")
                            .arg(body.red()).arg(body.green()).arg(body.blue())));

    // ...while the photo's white backdrop still disappears into the page, so
    // the picture reads as a picture and not as a white box.
    QCOMPARE(got.image.pixelColor(1791, 445), QColor(24, 26, 30));

    // The PCB render has no white backdrop at all (its board runs edge to
    // edge), so it is KeepAuthored - including its solidly black areas.
    const QColor board = got.image.pixelColor(929, 1303);
    QVERIFY2(board.lightness() < 40,
             qPrintable(QStringLiteral("PCB render not kept authored: %1,%2,%3")
                            .arg(board.red()).arg(board.green()).arg(board.blue())));

    // The page around the pictures inverts as always.
    QCOMPARE(got.image.pixelColor(60, 60), QColor(24, 26, 30));
}

void TstInkRects::textOverAPictureReadsAsPageText()
{
    const QString pdf = QStringLiteral(MERVIN_PCBA_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/pcba.pdf not present");

    // Page 2 at 150 dpi: a callout label - a white vector rectangle with black
    // rotated text on it - is drawn over the product photo at x 1804-2260,
    // y 412-643. The label's fill is indistinguishable from the picture's own
    // white backdrop to a pixel rule, so sinking the backdrop would leave the
    // label's text on a black hole. The text's own region therefore takes the
    // page treatment: the fill goes dark and the lettering comes up light,
    // exactly like a line of body text.
    const mervin::RenderResult got = renderComfort(pdf, 1);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());

    // Statistical, because the rotated lettering is a couple of pixels wide at
    // this scale and no single pixel survives sub-pixel drift. Inside the label
    // (x 2065-2185, y 410-640) the glyph strokes come up light: 4.5% of the
    // region measures min > 150 where the authored label has none - keeping the
    // label authored, or sinking it with the backdrop, both leave 0%.
    auto fractionLighterThan = [&](QRect r, int minChannel) {
        int hits = 0, total = 0;
        for (int y = r.top(); y < r.bottom(); ++y)
            for (int x = r.left(); x < r.right(); ++x) {
                const QColor c = got.image.pixelColor(x, y);
                hits += std::min({c.red(), c.green(), c.blue()}) > minChannel;
                ++total;
            }
        return total ? double(hits) / total : 0.0;
    };
    const double lit = fractionLighterThan(QRect(2065, 410, 120, 230), 150);
    QVERIFY2(lit > 0.02,
             qPrintable(QStringLiteral("label text did not take the page treatment: %1 lit")
                            .arg(lit)));
    // The label's white fill went down with the rest of the backdrop, so no
    // white box is left behind (0.1% measured, all of it glyph anti-aliasing).
    QVERIFY2(fractionLighterThan(QRect(2065, 410, 120, 230), 234) < 0.01,
             "the label is still a white box on the page");

    // Around it the picture is still a picture: its backdrop is gone and the
    // moulded part keeps its authored near-black.
    QCOMPARE(got.image.pixelColor(1804, 412), QColor(24, 26, 30));
    const QColor body = got.image.pixelColor(1950, 500);
    QVERIFY2(body.lightness() < 70,
             qPrintable(QStringLiteral("photo body not kept authored: %1,%2,%3")
                            .arg(body.red()).arg(body.green()).arg(body.blue())));
}

void TstInkRects::scannedPageStillInverts()
{
    const QString pdf = QStringLiteral(MERVIN_HOUSE_PDF);
    if (!QFileInfo::exists(pdf))
        QSKIP("examples/house-drawing.pdf not present");

    // The regression that deleted image handling once already (v1.36.0): this
    // page IS a scan - eight raster strips tiling it wall to wall - so its
    // images must keep inverting even though "leave pictures alone" is now the
    // default. The page-coverage test is what decides it.
    const mervin::RenderResult got = renderComfort(pdf, 0);
    QVERIFY2(got.ok, qPrintable(got.error));
    QVERIFY(!got.image.isNull());
    QVERIFY(got.image.width() > 2400 && got.image.height() > 1700);

    // A drawing line inside a strip: authored (21,21,21), rendered light.
    const QColor line = got.image.pixelColor(213, 55);
    QVERIFY2(line.lightness() > 150,
             qPrintable(QStringLiteral("scanned drawing line did not invert: %1,%2,%3")
                            .arg(line.red()).arg(line.green()).arg(line.blue())));
    // And the scan's white paper lands on the page background, not on a white
    // box - it is the same pixel rule as the rest of the page.
    const QColor paper = got.image.pixelColor(213, 49);
    QVERIFY2(paper.lightness() < 60,
             qPrintable(QStringLiteral("scanned paper did not darken: %1,%2,%3")
                            .arg(paper.red()).arg(paper.green()).arg(paper.blue())));
}

QTEST_GUILESS_MAIN(TstInkRects)
#include "tst_ink_rects.moc"
