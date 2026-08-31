// Guard rails for the house icon set (src/ui/Icons.cpp).
//
// The compiler already catches an unhandled enum value, so these cases go after
// what it cannot see: a glyph that compiles but paints nothing, paints outside
// its box, ignores the requested ink, or - the failure that actually shipped
// during the v1.45.0 unification - paints a shape whose arrowhead is detached
// from the stroke it is supposed to continue. A "does it draw any pixels" check
// would have passed that happily, so the cases below are deliberately
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
// stroke width itself and nothing more. ZoomOut is `line(5, 12, 19, 12)` in
// Icons.cpp - a minus, and the toolbar's counterpart to the ZoomIn plus - so no
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
    void rosterCoversTheEnum();
    void everyGlyphPaints_data();
    void everyGlyphPaints();
    void inkColourIsHonoured();
    void relatedGlyphsAreDistinct_data();
    void relatedGlyphsAreDistinct();
    void rotateArrowsMirrorEachOther();
    void rotateArrowHeadIsAttached();
    void badgeAddsInkToTheCorner();
    void ocrWordmarkIsWide();
};

// The roster in IconList.h drives the contact-sheet tool and the cases below, so
// a Glyph added to the enum without a roster entry must fail loudly rather than
// quietly go unrendered and untested. The enum is contiguous from 0, so its size
// is the last enumerator plus one - keep this naming whichever glyph is last.
void TestIcons::rosterCoversTheEnum()
{
    const auto &roster = mervin::icons::allGlyphs();
    QCOMPARE(int(roster.size()), int(Glyph::DragHandle) + 1);

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

void TestIcons::ocrWordmarkIsWide()
{
    const QImage img = mervin::icons::ocrWordmark(QColor(0, 0, 0))
                           .pixmap(QSize(34, 20), 1.0)
                           .toImage()
                           .convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(img.size(), QSize(34, 20));

    const QRect box = inkBounds(img);
    QVERIFY2(box.width() >= 29 && box.height() >= 13,
             qPrintable(QStringLiteral("wide OCR wordmark spans only %1x%2 px")
                            .arg(box.width()).arg(box.height())));
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
        {"page vs split page", Glyph::FitPage, Glyph::DocumentTheme, 0.95},
        {"page vs page with text", Glyph::FitPage, Glyph::SinglePage, 0.90},
        {"moon vs sun", Glyph::UiTheme, Glyph::Sun, 0.90},
        {"extract vs merge", Glyph::ExtractPages, Glyph::MergePages, 0.90},
        {"zoom in vs out", Glyph::ZoomIn, Glyph::ZoomOut, 0.96},
        {"search vs zoom out", Glyph::Search, Glyph::ZoomOut, 0.95},
        {"prev vs next", Glyph::PrevPage, Glyph::NextPage, 0.90},
        {"copy vs show all windows", Glyph::Copy, Glyph::ShowAllWindows, 0.90},
        {"document vs fit page", Glyph::Document, Glyph::FitPage, 0.90},
        {"select all vs thumbnails", Glyph::SelectAll, Glyph::Thumbnails, 0.90},
        // Both are "a stack of marks in the middle of the box". If the grip ever
        // gets redrawn as stacked lines it becomes the hamburger, and the merge
        // dialog's drag affordance stops reading as one.
        {"grip vs hamburger", Glyph::DragHandle, Glyph::Menu, 0.75},
        // Highlight Form Fields is two washed boxes; drop the wash or the second
        // box and it turns into one of these instead.
        {"fields vs split page", Glyph::HighlightFields, Glyph::DocumentTheme, 0.75},
        {"fields vs thumbnails", Glyph::HighlightFields, Glyph::Thumbnails, 0.75},
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

// RotateLeft is built as RotateRight's mirror image (the arc starts on the other
// side of the top opening and the head travels the other way), so flipping one
// must land on the other. This is the case that catches a mis-oriented or
// mis-placed arrowhead: both glyphs stay non-empty and stay distinct when the
// head is wrong, but the symmetry breaks immediately.
void TestIcons::rotateArrowsMirrorEachOther()
{
    const QImage cw = render(Glyph::RotateRight, 48);
    const QImage ccw = render(Glyph::RotateLeft, 48).mirrored(true, false);
    const double same = agreement(cw, ccw);
    QVERIFY2(same > 0.82, qPrintable(QStringLiteral("mirrored rotate arrows agree only %1%%")
                                         .arg(same * 100, 0, 'f', 1)));
}

// The arrowhead is a solid triangle continuing the arc, so the rotate glyph must
// carry noticeably more ink than the bare circle it is drawn on - and that extra
// ink must sit in the top half, where the opening and the head are. The v1.45.0
// regression drew the head detached and pointing out of the circle; it showed up
// as ink in the wrong half.
void TestIcons::rotateArrowHeadIsAttached()
{
    for (Glyph g : {Glyph::RotateLeft, Glyph::RotateRight}) {
        const QImage img = render(g, 48);
        const QImage top = img.copy(0, 0, 48, 24);
        const QImage bottom = img.copy(0, 24, 48, 24);
        const int t = inkCount(top), b = inkCount(bottom);
        QVERIFY2(t > b, qPrintable(QStringLiteral("head is not in the top half: %1 vs %2")
                                       .arg(t).arg(b)));

        // A solid head is a mass of adjacent opaque pixels; a stroke never is.
        // Look for a 4x4 fully opaque block in the top half.
        bool solidBlock = false;
        for (int y = 0; y + 4 <= top.height() && !solidBlock; ++y) {
            for (int x = 0; x + 4 <= top.width(); ++x) {
                bool all = true;
                for (int dy = 0; dy < 4 && all; ++dy)
                    for (int dx = 0; dx < 4; ++dx)
                        if (qAlpha(top.pixel(x + dx, y + dy)) < 250) { all = false; break; }
                if (all) { solidBlock = true; break; }
            }
        }
        QVERIFY2(solidBlock, "no solid arrowhead found in the top half");
    }
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

QTEST_MAIN(TestIcons)
#include "tst_icons.moc"
