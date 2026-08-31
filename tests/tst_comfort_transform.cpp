#include "render/ComfortTransform.h"

#include <QImage>
#include <QtTest>

// Pins the Comfort theme's pixel rules (see ComfortTransform.h). Without
// image rects every pixel gets the ink treatment:
// - neutral pixels take the offset negative (white -> #181A1E, black ->
//   #D6D9DE), bit-identical to every previous Comfort's grey rendering;
// - colourful pixels keep their authored hue instead of flipping to a
//   complement: dark inks are re-composited over the dark background (which
//   removes the white anti-alias halo around coloured glyphs), light inks
//   keep the authored pixel.
// With image rects, the photo treatment / keepAuthored rules apply inside
// them - covered end to end by tst_ink_rects.
class TstComfortTransform : public QObject
{
    Q_OBJECT

private slots:
    void neutralPixelsOffsetInverted();
    void colouredPixelsKeepHue();
    void keepAuthoredRectUntouched();
    void pageInkRegionOutranksImageModes();
    void photoOnWhiteRampsBackdrop();
    void backdropRampChoice();
    void imageModeClassifier();
};

namespace {

const QColor kComfortBg(24, 26, 30);    // comfortPixel(#FFFFFF), exact
const QColor kComfortFg(214, 217, 222); // comfortPixel(#000000), exact

} // namespace

void TstComfortTransform::neutralPixelsOffsetInverted()
{
    QImage page(100, 80, QImage::Format_RGB888);
    page.fill(QColor(255, 255, 255));
    page.setPixelColor(1, 1, QColor(0, 0, 0));       // black ink
    page.setPixelColor(2, 1, QColor(128, 128, 128)); // mid grey
    page.setPixelColor(3, 1, QColor(251, 251, 251)); // near-white JPEG noise

    mervin::applyComfortTransform(page);

    // Endpoints: white paper -> dark background, black ink -> light text.
    QCOMPARE(page.pixelColor(0, 0), kComfortBg);
    QCOMPARE(page.pixelColor(1, 1), kComfortFg);

    // Greys stay on the historical ramp, bit-exact:
    // tone_c(v) = kBg_c + round((255-v)*(kFg_c-kBg_c)/255).
    QCOMPARE(page.pixelColor(2, 1), QColor(119, 121, 126));

    // Near-white noise lands next to the background - invisible on the dark
    // page, exactly as it is invisible on white in the Light theme.
    QCOMPARE(page.pixelColor(3, 1), QColor(27, 29, 33));
}

void TstComfortTransform::colouredPixelsKeepHue()
{
    QImage page(100, 80, QImage::Format_RGB888);
    page.fill(QColor(255, 255, 255));
    page.setPixelColor(1, 1, QColor(0, 0, 200));     // saturated dark blue text
    page.setPixelColor(2, 1, QColor(200, 40, 60));   // saturated red
    page.setPixelColor(3, 1, QColor(255, 255, 194)); // pale yellow fill (light ink)
    page.setPixelColor(4, 1, QColor(128, 128, 229)); // anti-alias fringe of blue on white

    mervin::applyComfortTransform(page);

    // Fully saturated dark inks have paper coverage 1: the composite equals
    // the authored colour exactly - no complement flip (the old uniform
    // Comfort rendered this pixel yellow-ish).
    QCOMPARE(page.pixelColor(1, 1), QColor(0, 0, 200));

    // Saturated red keeps its red dominance (previously flipped to cyan).
    const QColor red = page.pixelColor(2, 1);
    QVERIFY2(red.red() > 130 && red.green() < 70 && red.red() - red.blue() > 60,
             qPrintable(QStringLiteral("red not kept red: %1,%2,%3")
                            .arg(red.red()).arg(red.green()).arg(red.blue())));

    // A pale fill unmixes to a LIGHT ink (pure yellow), so the authored
    // pixel is kept exactly - component boxes stay bright.
    QCOMPARE(page.pixelColor(3, 1), QColor(255, 255, 194));

    // The anti-alias fringe of dark ink is re-composited over the dark
    // background: dark and blue-dominant, never a light halo pixel.
    const QColor aa = page.pixelColor(4, 1);
    QVERIFY2(aa.lightness() < 150 && aa.blue() - aa.red() > 60,
             qPrintable(QStringLiteral("AA fringe not dark blue: %1,%2,%3")
                            .arg(aa.red()).arg(aa.green()).arg(aa.blue())));
}

void TstComfortTransform::keepAuthoredRectUntouched()
{
    QImage page(100, 80, QImage::Format_RGB888);
    page.fill(QColor(255, 255, 255));
    // A tinted-photo stand-in occupying x 10-29, y 10-29.
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x)
            page.setPixelColor(x, y, QColor(106, 123, 149));

    QVector<mervin::ComfortImageRect> rects;
    rects.append(
        mervin::ComfortImageRect{QRect(10, 10, 20, 20), mervin::ComfortImageMode::KeepAuthored});
    mervin::applyComfortTransform(page, rects);

    // Inside the keepAuthored rect: bit-exact authored pixels.
    QCOMPARE(page.pixelColor(15, 15), QColor(106, 123, 149));
    QCOMPARE(page.pixelColor(29, 29), QColor(106, 123, 149));
    // Outside it the page inverts normally.
    QCOMPARE(page.pixelColor(50, 50), kComfortBg);
    QCOMPARE(page.pixelColor(30, 15), kComfortBg);
}

void TstComfortTransform::pageInkRegionOutranksImageModes()
{
    QImage page(100, 80, QImage::Format_RGB888);
    page.fill(QColor(255, 255, 255));
    // A picture with a white backdrop, and a white callout label carrying dark
    // text dropped on it - the label's fill is white like the backdrop, so
    // without the PageInk region the ramp would sink it and hide the text.
    for (int y = 10; y < 40; ++y)
        for (int x = 10; x < 40; ++x)
            page.setPixelColor(x, y, x < 20 ? QColor(90, 90, 90) : QColor(255, 255, 255));
    page.setPixelColor(30, 30, QColor(0, 0, 0)); // a glyph pixel on the label

    QVector<mervin::ComfortImageRect> rects;
    rects.append(
        mervin::ComfortImageRect{QRect(10, 10, 30, 30), mervin::ComfortImageMode::PhotoOnWhite});
    rects.append(
        mervin::ComfortImageRect{QRect(25, 25, 10, 10), mervin::ComfortImageMode::PageInk});
    mervin::applyComfortTransform(page, rects);

    // Inside the region: the page treatment, so the glyph comes up light and
    // the label's fill goes down to the page background.
    QCOMPARE(page.pixelColor(30, 30), kComfortFg);
    QCOMPARE(page.pixelColor(28, 28), kComfortBg);
    // Outside the region the picture is untouched by it: the subject stays
    // authored and only the backdrop sinks.
    QCOMPARE(page.pixelColor(15, 15), QColor(90, 90, 90));
    QCOMPARE(page.pixelColor(22, 15), kComfortBg);
    // And the page outside the image rect is unaffected.
    QCOMPARE(page.pixelColor(60, 60), kComfortBg);
}

void TstComfortTransform::photoOnWhiteRampsBackdrop()
{
    QImage page(100, 80, QImage::Format_RGB888);
    page.fill(QColor(255, 255, 255));
    // A photo stand-in: light-grey subject on the white backdrop.
    for (int y = 12; y < 26; ++y)
        for (int x = 12; x < 26; ++x)
            page.setPixelColor(x, y, QColor(230, 230, 230));
    page.setPixelColor(13, 13, QColor(250, 250, 250)); // backdrop JPEG noise/ringing

    QVector<mervin::ComfortImageRect> rects;
    rects.append(
        mervin::ComfortImageRect{QRect(10, 10, 20, 20), mervin::ComfortImageMode::PhotoOnWhite});
    mervin::applyComfortTransform(page, rects);

    // Subject pixels (distance from white >= 24) are fully authored - the
    // photo is shown without inversion.
    QCOMPARE(page.pixelColor(15, 15), QColor(230, 230, 230));
    // The white backdrop inside the rect becomes exactly the page background.
    QCOMPARE(page.pixelColor(11, 11), kComfortBg);
    // Noise and ringing up to distance 8 sit in the ramp's dead zone and
    // disappear into the background completely - no speckle halo.
    QCOMPARE(page.pixelColor(13, 13), kComfortBg);
    // Outside the rect the page inverts normally.
    QCOMPARE(page.pixelColor(50, 50), kComfortBg);

    // The calibrated gentle ramp (near-neutral photo on a measured-clean
    // backdrop -> (1, 5)): white subject faces just a few values from white
    // stay fully authored - the "bleed" case - while exact white still
    // vanishes and near-white noise is softly ramped.
    QImage page2(100, 80, QImage::Format_RGB888);
    page2.fill(QColor(255, 255, 255));
    page2.setPixelColor(15, 15, QColor(250, 250, 250)); // bright subject face
    page2.setPixelColor(16, 15, QColor(253, 253, 253)); // backdrop noise
    QVector<mervin::ComfortImageRect> rects2;
    rects2.append(mervin::ComfortImageRect{QRect(10, 10, 20, 20),
                                           mervin::ComfortImageMode::PhotoOnWhite, 1, 5});
    mervin::applyComfortTransform(page2, rects2);
    QCOMPARE(page2.pixelColor(15, 15), QColor(250, 250, 250)); // authored at distance >= 5
    QCOMPARE(page2.pixelColor(16, 15), QColor(60, 61, 65));    // ramped towards the page
    QCOMPARE(page2.pixelColor(11, 11), kComfortBg);            // white backdrop still vanishes
}

void TstComfortTransform::backdropRampChoice()
{
    // Measurements from the render worker over examples/ (nearWhite, ringing,
    // border-ring p99). The aggressive dead zone must go exactly to the images
    // whose near-white pixels are compression ringing, and never to a photo of
    // a white object - its faces sit in the same value range.
    using Ramp = QPair<int, int>; // named so the commas survive QCOMPARE
    auto ramp = [](float nearWhite, float ringing, int ringP99) {
        quint8 lo = 0, hi = 0;
        mervin::comfortBackdropRamp(nearWhite, ringing, ringP99, &lo, &hi);
        return Ramp(lo, hi);
    };
    const Ramp aggressive(24, 40);

    // Ringing-dominated: a CMYK photo of a grey part (the halo this rule
    // exists for), tinted product shots, a photo of a black part.
    QCOMPARE(ramp(0.041f, 0.0123f, 0), aggressive);
    QCOMPARE(ramp(0.015f, 0.0102f, 0), aggressive);
    QCOMPARE(ramp(0.003f, 0.0029f, 0), aggressive);
    QCOMPARE(ramp(0.0024f, 0.0020f, 44), aggressive);
    // No near-white pixels at all: nothing to lose, no division by zero.
    QCOMPARE(ramp(0.0f, 0.0f, 203), aggressive);

    // Photos of white housings: their faces must survive, so the ramp stays
    // calibrated to the border ring - (1, 5) on a clean backdrop.
    QCOMPARE(ramp(0.218f, 0.0004f, 0), Ramp(1, 5));
    QCOMPARE(ramp(0.222f, 0.0013f, 0), Ramp(1, 5));
    QCOMPARE(ramp(0.066f, 0.0055f, 0), Ramp(1, 5)); // the closest call
    QCOMPARE(ramp(0.106f, 0.0091f, 0), Ramp(1, 5));
    // A noisy border ring degrades the gentle ramp towards the dead zone, and
    // is capped so a subject touching the border cannot open it wide.
    QCOMPARE(ramp(0.180f, 0.0043f, 3), Ramp(4, 8));
    QCOMPARE(ramp(0.180f, 0.0043f, 200), Ramp(8, 12));
}

void TstComfortTransform::imageModeClassifier()
{
    using Mode = mervin::ComfortImageMode;
    // Every case below is a real measurement taken from the example corpus by
    // the render worker (RenderEngine's embeddedImageProbe), except the two
    // marked synthetic. Fields: white, stroke, grad, ring, coverage.
    auto mode = [](float white, float stroke, float grad, float ring, float coverage) {
        mervin::ComfortImageFeatures f;
        f.whiteFrac = white;
        f.strokeFrac = stroke;
        f.gradFrac = grad;
        f.ringWhiteFrac = ring;
        f.pageCoverage = coverage;
        return mervin::comfortImageMode(f);
    };

    // A picture on a white or transparent backdrop: authored, backdrop sunk
    // into the page. These are the cases the previous photo-first classifier
    // rejected and inverted - the bug this rule exists to fix.
    QCOMPARE(mode(0.51f, 0.016f, 0.1372f, 0.81f, 0.19f), Mode::PhotoOnWhite); // wide crop, black
    QCOMPARE(mode(0.25f, 0.007f, 0.1072f, 0.94f, 0.19f), Mode::PhotoOnWhite); // black part photo
    QCOMPARE(mode(0.63f, 0.048f, 0.0258f, 0.51f, 0.19f), Mode::PhotoOnWhite); // PCB, edge to edge
    QCOMPARE(mode(0.07f, 0.010f, 0.2933f, 0.90f, 0.19f), Mode::PhotoOnWhite); // tight crop, ring
    QCOMPARE(mode(0.68f, 0.046f, 0.0644f, 1.00f, 0.04f), Mode::PhotoOnWhite); // grey product render
    QCOMPARE(mode(0.74f, 0.150f, 0.0638f, 1.00f, 0.04f), Mode::PhotoOnWhite); // crisp-edged photo
    QCOMPARE(mode(0.47f, 0.145f, 0.8031f, 0.56f, 0.11f), Mode::PhotoOnWhite); // soft grey banner

    // No white backdrop to remove: left completely alone.
    QCOMPARE(mode(0.00f, 0.000f, 0.1517f, 0.00f, 0.11f), Mode::KeepAuthored); // tinted photo
    QCOMPARE(mode(0.07f, 0.042f, 0.0053f, 0.26f, 0.19f), Mode::KeepAuthored); // full-bleed PCB
    QCOMPARE(mode(0.05f, 0.024f, 0.0022f, 0.17f, 0.19f), Mode::KeepAuthored); // flat CAD render

    // Ink on white: inverted, because keeping it authored would leave dark
    // marks on the dark page - the v1.36.0 failure.
    QCOMPARE(mode(0.99f, 0.986f, 0.0000f, 0.99f, 1.00f), Mode::Split); // scan strip, scanned page
    QCOMPARE(mode(0.92f, 0.514f, 0.0045f, 0.93f, 1.00f), Mode::Split); // scan strip, shaded areas
    QCOMPARE(mode(0.99f, 0.078f, 0.0160f, 0.97f, 1.00f), Mode::Split); // thin rule, scanned page
    QCOMPARE(mode(0.84f, 0.378f, 0.0056f, 1.00f, 0.05f), Mode::Split); // line-art plot figure
    QCOMPARE(mode(0.52f, 0.330f, 0.0082f, 0.84f, 0.05f), Mode::Split); // logo on white
    QCOMPARE(mode(0.57f, 0.052f, 0.0000f, 0.97f, 0.03f), Mode::Split); // flat wordmark artwork
    QCOMPARE(mode(0.61f, 0.121f, 0.0005f, 0.86f, 0.01f), Mode::Split); // flat colour logo
    QCOMPARE(mode(0.48f, 0.644f, 0.0034f, 0.75f, 0.19f), Mode::Split); // small logo, soft mask
    // Synthetic: dithered and grainy scans are all isolated marks, so they
    // score as stroke however smooth their tone histogram looks.
    QCOMPARE(mode(0.60f, 0.900f, 0.0500f, 0.95f, 0.10f), Mode::Split); // halftone/newspaper scan
    QCOMPARE(mode(0.59f, 0.400f, 0.0300f, 0.90f, 0.10f), Mode::Split); // grainy photocopy

    // The scanned-page override outranks everything: a photo-like image that
    // tiles the page is a scan of a page, not a picture on one.
    QCOMPARE(mode(0.68f, 0.046f, 0.0644f, 1.00f, 0.60f), Mode::Split);
    QCOMPARE(mode(0.68f, 0.046f, 0.0644f, 1.00f, 0.59f), Mode::PhotoOnWhite);

    // The two thresholds that decide "is there a backdrop to remove" and
    // "is this ink" - pinned so a retune has to be deliberate.
    QCOMPARE(mode(0.20f, 0.010f, 0.0500f, 0.40f, 0.00f), Mode::PhotoOnWhite);
    QCOMPARE(mode(0.20f, 0.010f, 0.0500f, 0.39f, 0.00f), Mode::KeepAuthored);
    QCOMPARE(mode(0.30f, 0.300f, 0.0500f, 1.00f, 0.00f), Mode::Split);
    QCOMPARE(mode(0.29f, 0.300f, 0.0500f, 1.00f, 0.00f), Mode::PhotoOnWhite);
    QCOMPARE(mode(0.30f, 0.290f, 0.0040f, 1.00f, 0.00f), Mode::PhotoOnWhite);
    QCOMPARE(mode(0.30f, 0.290f, 0.0039f, 1.00f, 0.00f), Mode::Split);
}

QTEST_GUILESS_MAIN(TstComfortTransform)
#include "tst_comfort_transform.moc"
