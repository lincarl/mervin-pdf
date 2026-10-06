// Guard rails for application artwork and the Lucide icon set (src/ui/Icons.cpp).
//
// The compiler already catches an unhandled enum value, so these cases go after
// what it cannot see: a glyph whose SVG is missing from the resource (it renders
// nothing), one mapped to the wrong file (it collides with a sibling), one drawn
// off-centre or clipped, or one that ignores the requested ink. A "does it draw
// any pixels" check would pass most of those, so the cases below are deliberately
// discriminating: they compare glyphs against each other and assert the
// symmetries the drawings are built on.
//
// Note this test paints QPixmaps, so it needs a GUI application and a platform
// plugin (on a headless Linux box: QT_QPA_PLATFORM=offscreen). No window is ever
// created or shown.

#include "ui/IconList.h"
#include "ui/Icons.h"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QSet>
#include <QTest>

#include <cmath>

using mervin::icons::Glyph;

namespace {

// Always ask at device-pixel-ratio 1. QIcon::pixmap(int, int) honours the
// screen's DPR, so on a scaled display it hands back a pixmap of a different
// size than requested (and caps at the icon's largest available pixmap), which
// makes every size-dependent assertion below machine-specific.
QImage render(Glyph id, int sz, const QColor &ink = QColor(0, 0, 0))
{
    return mervin::icons::glyph(id, ink).pixmap(QSize(sz, sz), 1.0).toImage()
        .convertToFormat(QImage::Format_ARGB32);
}

// Every pixel with meaningful coverage. Antialiasing leaves a wide skirt of very
// low alpha, so the threshold is what a human would call "inked".
int inkCount(const QImage &img, int minAlpha = 40)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) >= minAlpha)
                ++n;
    return n;
}

// Glyphs drawn as one straight stroke, where the "short side" of the ink is the
// stroke width itself and nothing more. ZoomOut is Lucide's "minus" - the
// toolbar's counterpart to the ZoomIn plus - so no
// correct drawing of it can reach the short-side floor the two-dimensional glyphs
// are held to. Keep this list to glyphs that really are one stroke: a glyph that
// merely came out thin is the bug this file exists to catch.
bool isSingleStroke(Glyph id)
{
    return id == Glyph::ZoomOut;
}

// The bounding box of everything inked.
QRect inkBounds(const QImage &img, int minAlpha = 40)
{
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) < minAlpha)
                continue;
            x0 = qMin(x0, x); y0 = qMin(y0, y);
            x1 = qMax(x1, x); y1 = qMax(y1, y);
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

// Fraction of inked pixels the two images agree on, over their union.
double agreement(const QImage &a, const QImage &b, int minAlpha = 40)
{
    int both = 0, either = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const bool ia = qAlpha(a.pixel(x, y)) >= minAlpha;
            const bool ib = qAlpha(b.pixel(x, y)) >= minAlpha;
            if (ia || ib)
                ++either;
            if (ia && ib)
                ++both;
        }
    }
    return either == 0 ? 0.0 : double(both) / double(either);
}

} // namespace

class TestIcons : public QObject
{
    Q_OBJECT

private slots:
    void applicationIconPreservesApprovedFrames();
    void applicationIconKeepsFoldAndRaisedP();
    void applicationIconSelectsScaledFrames();
    void rosterCoversTheEnum();
    void everyGlyphPaints_data();
    void everyGlyphPaints();
    void inkColourIsHonoured();
    void relatedGlyphsAreDistinct_data();
    void relatedGlyphsAreDistinct();
    void rotateArrowsMirrorEachOther();
    void badgeAddsInkToTheCorner();
    void strokeOverrideThickensTheLine();
    void smallRendersKeepTheirDots();
    void spinChevronsPointOppositeWays();
};

void TestIcons::applicationIconPreservesApprovedFrames()
{
    const QIcon icon = mervin::icons::applicationIcon();
    QVERIFY(!icon.isNull());
    const QList<QSize> sizes = icon.availableSizes();
    QCOMPARE(sizes.size(), 15);
    for (int size : {16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256}) {
        QVERIFY2(sizes.contains(QSize(size, size)),
                 qPrintable(QStringLiteral("missing native %1 px frame").arg(size)));
        const QString path = size == 256
            ? QStringLiteral(":/icons/mervin-icon.png")
            : QStringLiteral(":/icons/mervin-icon/%1.png").arg(size);
        const QImage approved(path);
        QVERIFY(!approved.isNull());
        QCOMPARE(approved.size(), QSize(size, size));
        const QImage actual = icon.pixmap(QSize(size, size), 1.0).toImage();
        QCOMPARE(actual.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                 approved.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
}

void TestIcons::applicationIconKeepsFoldAndRaisedP()
{
    const QImage image = mervin::icons::applicationIcon().pixmap(QSize(256, 256), 1.0).toImage();
    QCOMPARE(image.size(), QSize(256, 256));
    // The cut-away top-right corner, blue fold and raised white stem distinguish
    // the approved page artwork from the old square and the alternative mockups.
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(image.pixelColor(200, 30).alpha(), 0);
    QCOMPARE(image.pixelColor(170, 50), QColor(0x91, 0xd9, 0xf5));
    QCOMPARE(image.pixelColor(95, 85), QColor(Qt::white));
    QCOMPARE(image.pixelColor(95, 180), QColor(Qt::white));
    QVERIFY(image.pixelColor(95, 200) != QColor(Qt::white));
    QVERIFY(image.pixelColor(45, 100).blue() > image.pixelColor(45, 100).red());
}

void TestIcons::applicationIconSelectsScaledFrames()
{
    const QIcon icon = mervin::icons::applicationIcon();
    // A 16 logical-pixel icon must select the original physical frame for the
    // common display scales, without resampling a smaller raster.
    for (qreal ratio : {1.0, 1.25, 1.5, 2.0, 2.5, 3.0}) {
        const int pixels = qRound(16 * ratio);
        const QPixmap scaled = icon.pixmap(QSize(16, 16), ratio);
        QCOMPARE(scaled.size(), QSize(pixels, pixels));
        QCOMPARE(scaled.devicePixelRatio(), ratio);
        QImage actual = scaled.toImage();
        actual.setDevicePixelRatio(1.0);
        const QImage native = icon.pixmap(QSize(pixels, pixels), 1.0).toImage();
        QCOMPARE(actual, native);
    }
}

// The roster in IconList.h drives the contact-sheet tool and the cases below, so
// a Glyph added to the enum without a roster entry must fail loudly rather than
// quietly go unrendered and untested. The enum is contiguous from 0, so its size
// is the last enumerator plus one - keep this naming whichever glyph is last.
void TestIcons::rosterCoversTheEnum()
{
    const auto &roster = mervin::icons::allGlyphs();
    QCOMPARE(int(roster.size()), int(Glyph::Star) + 1);

    QSet<int> seen;
    for (const auto &e : roster) {
        QVERIFY2(!seen.contains(int(e.id)),
                 qPrintable(QStringLiteral("duplicate roster entry: %1").arg(e.name)));
        seen.insert(int(e.id));
        QVERIFY2(e.name != nullptr && *e.name != '\0', "roster entry has no name");
    }
}

void TestIcons::everyGlyphPaints_data()
{
    QTest::addColumn<int>("id");
    QTest::addColumn<int>("size");
    for (const auto &e : mervin::icons::allGlyphs())
        for (int sz : {16, 20, 24, 32, 48})
            QTest::newRow(qPrintable(QStringLiteral("%1@%2").arg(e.name).arg(sz)))
                << int(e.id) << sz;
}

void TestIcons::everyGlyphPaints()
{
    QFETCH(int, id);
    QFETCH(int, size);
    const QImage img = render(Glyph(id), size);
    QCOMPARE(img.width(), size);

    // Enough ink to be a pictograph, not so much that the glyph is a filled blob.
    const int ink = inkCount(img);
    const int total = size * size;
    QVERIFY2(ink >= total / 40, qPrintable(QStringLiteral("almost no ink: %1 of %2 px")
                                              .arg(ink).arg(total)));
    QVERIFY2(ink <= total * 3 / 4, qPrintable(QStringLiteral("glyph is a blob: %1 of %2 px")
                                                  .arg(ink).arg(total)));

    // The drawing must fill and sit centred in its box. Testing for ink in the
    // middle would be wrong - FitPage, FullScreen and SelectAll are hollow by
    // design - so this checks the ink's extent and where it is balanced instead.
    // That still catches a glyph drawn off in a corner or mostly clipped away.
    // Elongated glyphs are legitimate (a chevron is about 6x12 in a 16 px box, a
    // double arrow 14x6), so the long axis carries the "fills its box" duty and
    // the short one only has to be more than a hairline.
    // A single-stroke glyph is the limit case of "elongated": its short side is the
    // stroke width, so it answers to a visible-stroke floor instead of the 28% one.
    // That still catches a hairline or a drawing clipped to nothing, and it gives up
    // no coverage on the pair it could hide - ZoomIn losing its vertical stroke would
    // collide with ZoomOut and trip the ceiling in relatedGlyphsAreDistinct().
    const QRect box = inkBounds(img);
    const int longSide = qMax(box.width(), box.height());
    const int shortSide = qMin(box.width(), box.height());
    // 8% with a 1px allowance, not the measured width: the stroke renders 2px wide at
    // 16-24px and 4px at 32-48px here, and pinning that exactly would be the kind of
    // rasteriser-detail assertion v1.54.1 already had to unpick once.
    const int shortFloor = isSingleStroke(Glyph(id)) ? qMax(1, size * 8 / 100)
                                                     : size * 28 / 100;
    QVERIFY2(longSide >= size * 55 / 100 && shortSide >= shortFloor,
             qPrintable(QStringLiteral("glyph only spans %1x%2 of %3 px")
                            .arg(box.width()).arg(box.height()).arg(size)));
    const double offX = std::abs(box.center().x() + 0.5 - size / 2.0) / size;
    const double offY = std::abs(box.center().y() + 0.5 - size / 2.0) / size;
    QVERIFY2(offX < 0.12 && offY < 0.12,
             qPrintable(QStringLiteral("glyph is off centre by (%1, %2) of its box")
                            .arg(offX, 0, 'f', 3).arg(offY, 0, 'f', 3)));
}

void TestIcons::inkColourIsHonoured()
{
    // A tint must reach the pixels: the same glyph in two inks may not agree on
    // colour anywhere it is opaque.
    for (const auto &e : mervin::icons::allGlyphs()) {
        const QImage red = render(e.id, 32, QColor(255, 0, 0));
        const QImage blue = render(e.id, 32, QColor(0, 0, 255));
        bool sawRed = false, sawBlue = false;
        for (int y = 0; y < 32 && !(sawRed && sawBlue); ++y) {
            for (int x = 0; x < 32; ++x) {
                if (qAlpha(red.pixel(x, y)) < 200)
                    continue;
                const QRgb r = red.pixel(x, y), b = blue.pixel(x, y);
                if (qRed(r) > 150 && qBlue(r) < 80)
                    sawRed = true;
                if (qBlue(b) > 150 && qRed(b) < 80)
                    sawBlue = true;
                if (sawRed && sawBlue)
                    break;
            }
        }
        QVERIFY2(sawRed && sawBlue,
                 qPrintable(QStringLiteral("%1 does not take the requested ink").arg(e.name)));
    }
}

void TestIcons::relatedGlyphsAreDistinct_data()
{
    QTest::addColumn<int>("a");
    QTest::addColumn<int>("b");
    QTest::addColumn<double>("maxAgreement");
    // Pairs a careless edit could collapse into one drawing. Each shares a motif
    // with the other, so "they both paint something" says nothing useful. The
    // ceiling is per pair because some are meant to be near-twins: zoom in and
    // out are one magnifier apart from each other by a single 6-unit stroke, so
    // they legitimately agree on ~91% of their ink and only a collapse to
    // identical is a bug.
    const struct { const char *name; Glyph a, b; double max; } pairs[] = {
        {"rotate directions", Glyph::RotateLeft, Glyph::RotateRight, 0.90},
        {"prev vs next", Glyph::PrevPage, Glyph::NextPage, 0.90},
        // A minus is the plus without its vertical stroke, so the pair shares
        // about half its ink by design; only a collapse to identical is a bug.
        {"zoom in vs out", Glyph::ZoomIn, Glyph::ZoomOut, 0.96},
        {"zoom in vs close", Glyph::ZoomIn, Glyph::Close, 0.90},
        {"moon vs sun", Glyph::UiTheme, Glyph::Sun, 0.90},
        {"split vs merge", Glyph::SplitPages, Glyph::MergePages, 0.90},
        {"copy vs show all windows", Glyph::Copy, Glyph::ShowAllWindows, 0.90},
        // The same folded page; only the text lines tell the Extract strip's
        // folded pages apart from a plain document.
        {"file text vs document", Glyph::FileText, Glyph::Document, 0.90},
        // Lucide draws both as corner brackets around the box ("scan" and
        // "maximize" are near-identical), which is why FitMode is "fullscreen":
        // the toolbar's fit toggle must not look like the menu's full screen.
        {"fit mode vs full screen", Glyph::FitMode, Glyph::FullScreen, 0.75},
        {"select all vs single page", Glyph::SelectAll, Glyph::SinglePage, 0.90},
        // Both are "a stack of marks in the middle of the box". If the grip ever
        // gets redrawn as stacked lines it becomes the hamburger, and the merge
        // dialog's drag affordance stops reading as one.
        {"grip vs hamburger", Glyph::DragHandle, Glyph::Menu, 0.75},
    };
    for (const auto &p : pairs)
        QTest::newRow(p.name) << int(p.a) << int(p.b) << p.max;
}

void TestIcons::relatedGlyphsAreDistinct()
{
    QFETCH(int, a);
    QFETCH(int, b);
    QFETCH(double, maxAgreement);
    const double same = agreement(render(Glyph(a), 48), render(Glyph(b), 48));
    QVERIFY2(same < maxAgreement,
             qPrintable(QStringLiteral("glyphs are %1% identical (ceiling %2%)")
                            .arg(same * 100, 0, 'f', 1).arg(maxAgreement * 100, 0, 'f', 0)));
}

// Lucide draws rotate-ccw as rotate-cw's mirror image, so flipping one must land
// on the other. This catches the two being mapped to the same file, or to files
// that point the same way: both stay non-empty and distinct from other glyphs,
// but the symmetry breaks immediately.
void TestIcons::rotateArrowsMirrorEachOther()
{
    const QImage cw = render(Glyph::RotateRight, 48);
    const QImage ccw = render(Glyph::RotateLeft, 48).mirrored(true, false);
    const double same = agreement(cw, ccw);
    QVERIFY2(same > 0.82, qPrintable(QStringLiteral("mirrored rotate arrows agree only %1%%")
                                         .arg(same * 100, 0, 'f', 1)));
}

// glyphBadged() composes its pixmaps through QIcon::pixmap(), so which sizes the
// composite ends up carrying depends on the screen's device pixel ratio - and a
// QIcon never upscales past its largest pixmap. Asking for 48 can therefore hand
// back something smaller on a scaled display. So take whatever size it gives,
// paint the bare glyph at that same size, and derive the quadrant from it: the
// assertion is about where the badge puts its ink, not about pixel counts.
void TestIcons::badgeAddsInkToTheCorner()
{
    const QImage badged = mervin::icons::glyphBadged(Glyph::Open, Glyph::Copy, QColor(0, 0, 0))
                              .pixmap(QSize(48, 48), 1.0).toImage()
                              .convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(badged.width() >= 16, qPrintable(QStringLiteral("badged icon came back at %1 px")
                                                  .arg(badged.width())));
    QCOMPARE(badged.height(), badged.width());

    const QImage plain = render(Glyph::Open, badged.width());
    QCOMPARE(badged.size(), plain.size());

    // The badge occupies the bottom-right; that quadrant must gain ink, and the
    // composite must differ from the base glyph.
    const int half = badged.width() / 2;
    const QRect corner(badged.width() - half, badged.height() - half, half, half);
    QVERIFY(inkCount(badged.copy(corner)) > inkCount(plain.copy(corner)));
    QVERIFY(agreement(plain, badged) < 0.95);
}

// The set draws a 1.5-unit stroke, lighter than the 2 units in Lucide's files,
// and the stylesheet's indicator images (Theme.cpp) ask for a heavier one still.
// Both rewrites have to reach the SVG: at 48 px, where the small-size floor does
// not apply and half a unit is a whole pixel, the set's own width must match an
// explicit 1.5, Lucide's untouched 2 must carry more ink, and a heavier override
// more again.
void TestIcons::strokeOverrideThickensTheLine()
{
    const auto ink = [](double stroke) {
        return inkCount(mervin::icons::glyphPixmap(Glyph::Check, QColor(0, 0, 0), 48, stroke)
                            .toImage().convertToFormat(QImage::Format_ARGB32));
    };
    const int plain = ink(0);
    QVERIFY(plain > 0);
    QCOMPARE(ink(1.5), plain); // the set's own stroke: the same drawing
    QVERIFY2(ink(2.0) > plain,
             qPrintable(QStringLiteral("Lucide's stroke 2 inks %1 px, the set's 1.5 inks %2 px")
                            .arg(ink(2.0)).arg(plain)));
    QVERIFY2(ink(3.0) > plain * 5 / 4,
             qPrintable(QStringLiteral("stroke 3 inks %1 px, stroke 1.5 inks %2 px")
                            .arg(ink(3.0)).arg(plain)));
}

// At 1.5 units a 16 px render is exactly one device pixel wide, where Qt's raster
// engine switches to its hairline stroker and drops round caps: the info "i" lost
// its dot and the keyboard its keys at 100% scaling. Small renders are thickened to
// stay above a pixel, both in glyph() and in glyphPixmap()'s own sizes, such as the
// 12 px toolbar dropdown arrow, which is rendered natively rather than shrunk.
void TestIcons::smallRendersKeepTheirDots()
{
    const QColor black(0, 0, 0);
    const auto image = [](const QPixmap &pm) {
        return pm.toImage().convertToFormat(QImage::Format_ARGB32);
    };
    const QImage info = render(Glyph::About, 16);
    // Lucide's dot is "M12 8h.01": (8, 5.3) at 16 px, clear of the ring and the stem.
    const QRect dot(7, 4, 3, 3);
    QVERIFY2(inkCount(info.copy(dot)) > 0, "the info dot is missing at 16 px");

    // The floor, not luck: the 16 px render carries more ink than the bare set stroke.
    const QImage bare = image(mervin::icons::glyphPixmap(Glyph::About, black, 16, 1.5));
    QVERIFY2(inkCount(info) > inkCount(bare),
             qPrintable(QStringLiteral("16 px render inks %1 px, the bare 1.5 stroke %2 px")
                            .arg(inkCount(info)).arg(inkCount(bare))));
    // Pinned to DPR 1: the default glyphPixmap() path otherwise renders at the
    // screen's ratio, and these pixel positions assume a 16 / 12 px image.
    const auto native = [&black](Glyph id, int size) {
        return mervin::icons::glyphPixmap(id, black, size, 0, QColor(), 1.0)
            .toImage().convertToFormat(QImage::Format_ARGB32);
    };
    QVERIFY(inkCount(native(Glyph::About, 16).copy(dot)) > 0);

    // The 12 px arrow is drawn natively at 12 px. Shrinking glyph()'s 16 px render
    // instead keeps the outline but halves the solid line, so compare solid ink.
    const QImage arrow = native(Glyph::ChevronDown, 12);
    const QImage shrunk = render(Glyph::ChevronDown, 12);
    QCOMPARE(arrow.size(), QSize(12, 12));
    QVERIFY2(inkCount(arrow, 128) > inkCount(shrunk, 128),
             qPrintable(QStringLiteral("12 px arrow has %1 solid px, the shrunk 16 px render %2")
                            .arg(inkCount(arrow, 128)).arg(inkCount(shrunk, 128))));
    // A ratio the caller passes is honoured: the arrow for a 2x widget is 24 device px.
    const QPixmap sharp = mervin::icons::glyphPixmap(Glyph::ChevronDown, black, 12, 0, QColor(), 2.0);
    QCOMPARE(sharp.size(), QSize(24, 24));
    QCOMPARE(sharp.devicePixelRatio(), 2.0);
}

// The spin-box steppers and the combo arrow are Lucide chevron-up / chevron-down,
// which mirror each other top to bottom. A swap (or both mapped to one file)
// leaves each image plausible on its own, so compare them.
void TestIcons::spinChevronsPointOppositeWays()
{
    const QImage up = mervin::icons::spinChevron(false, QColor(0, 0, 0), 32).toImage()
                          .convertToFormat(QImage::Format_ARGB32);
    const QImage down = mervin::icons::spinChevron(true, QColor(0, 0, 0), 32).toImage()
                            .convertToFormat(QImage::Format_ARGB32);
    QVERIFY(inkCount(up) > 0);
    QVERIFY2(agreement(up, down) < 0.5, "the up and down chevrons are the same drawing");
    QVERIFY2(agreement(up, down.mirrored(false, true)) > 0.85,
             "the down chevron is not the up chevron flipped");
    // The "down" one must actually point down. Both span the same rows, so look
    // where the arms meet: in the middle column the down chevron's ink sits in
    // the lower half of its box, the up chevron's in the upper half.
    const auto apexY = [](const QImage &img) {
        const QRect box = inkBounds(img);
        const int x = box.center().x();
        int sum = 0, n = 0;
        for (int y = box.top(); y <= box.bottom(); ++y)
            if (qAlpha(img.pixel(x, y)) >= 40) {
                sum += y;
                ++n;
            }
        return n ? double(sum) / n - box.center().y() : 0.0;
    };
    QVERIFY2(apexY(down) > 0 && apexY(up) < 0,
             qPrintable(QStringLiteral("apex offsets: down %1, up %2")
                            .arg(apexY(down)).arg(apexY(up))));
}

QTEST_MAIN(TestIcons)
#include "tst_icons.moc"
