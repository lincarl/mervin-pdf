#include "ui/Icons.h"

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QSize>

#include <array>
#include <cmath>
#include <initializer_list>

namespace mervin::icons {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- the Fluent Outline house style ----------------------------------------
// Three numbers define the whole language. Change them here and every icon
// follows; see the style notes in Icons.h.
constexpr qreal kStroke  = 1.45; // nominal stroke, in grid units
constexpr qreal kOptical = 1.09; // shapes sit this much larger in the 24 box
constexpr qreal kRadius  = 2.1;  // multiplier on every corner radius

// Render `paint` into a transparent QIcon at the sizes Qt commonly requests.
// Each size is painted natively (not scaled from one pixmap) so thin strokes
// stay sharp. The callback draws on a 24-unit grid; `s` scales it to `sz` px.
// The optical enlargement is applied here, around the grid's centre, so no
// pictograph has to know about it.
template <typename Paint>
void paintOne(QPixmap &pm, int sz, Paint paint)
{
    pm = QPixmap(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = sz / 24.0;
    p.translate(12 * s, 12 * s);
    p.scale(kOptical, kOptical);
    p.translate(-12 * s, -12 * s);
    paint(p, s);
}

template <typename Paint>
QIcon painted(Paint paint)
{
    QIcon icon;
    for (int sz : {16, 20, 24, 32, 48}) {
        QPixmap pm;
        paintOne(pm, sz, paint);
        icon.addPixmap(pm);
    }
    return icon;
}

// The outline pen: round caps and joins, no fill. `width` is in grid units; the
// optical scale is divided back out so the stroke still renders at `width`.
QPen stroke(const QColor &c, qreal s, qreal width = kStroke)
{
    QPen pen(c);
    pen.setWidthF(width * s / kOptical);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

} // namespace

QIcon glyph(Glyph id, const QColor &color)
{
    return painted([id, color](QPainter &p, qreal s) {
        p.setPen(stroke(color, s));
        p.setBrush(Qt::NoBrush);

        // ---- shorthands on the 24-unit grid --------------------------------
        auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
            p.drawLine(QPointF(x1 * s, y1 * s), QPointF(x2 * s, y2 * s));
        };
        auto poly = [&](std::initializer_list<QPointF> pts) {
            QPolygonF pl;
            for (const QPointF &pt : pts)
                pl << QPointF(pt.x() * s, pt.y() * s);
            p.drawPolyline(pl);
        };
        // Rounded rectangle. `r` is the base radius; the house style multiplies
        // it, clamped so a small shape cannot round past a capsule.
        auto rrect = [&](qreal x, qreal y, qreal w, qreal h, qreal r) {
            const qreal rad = qMin(r * kRadius, qMin(w, h) / 2.0);
            p.drawRoundedRect(QRectF(x * s, y * s, w * s, h * s), rad * s, rad * s);
        };
        auto dot = [&](qreal cx, qreal cy, qreal r) {
            const QPen saved = p.pen();
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(cx * s, cy * s), r * s, r * s);
            p.setBrush(Qt::NoBrush);
            p.setPen(saved);
        };
        auto path = [&](const QPainterPath &pp) { p.drawPath(pp); };
        // A grid-unit path builder, so shapes read as coordinates not products.
        struct Pen {
            QPainterPath pp;
            qreal s;
            Pen &to(qreal x, qreal y) { pp.moveTo(x * s, y * s); return *this; }
            Pen &l(qreal x, qreal y) { pp.lineTo(x * s, y * s); return *this; }
            Pen &q(qreal cx, qreal cy, qreal x, qreal y)
            { pp.quadTo(cx * s, cy * s, x * s, y * s); return *this; }
            Pen &shut() { pp.closeSubpath(); return *this; }
        };
        auto pen = [&] { return Pen{QPainterPath(), s}; };

        // Fluent's solid arrow terminal: a filled triangle, tip at (tx,ty),
        // pointing along the unit vector (dx,dy).
        auto arrow = [&](qreal tx, qreal ty, qreal dx, qreal dy,
                         qreal len = 4.0, qreal spread = 3.3) {
            const qreal bx = -dx, by = -dy;   // backwards from the tip
            const qreal nx = -by, ny = bx;    // across
            QPolygonF tri;
            tri << QPointF(tx * s, ty * s)
                << QPointF((tx + bx * len + nx * spread) * s, (ty + by * len + ny * spread) * s)
                << QPointF((tx + bx * len - nx * spread) * s, (ty + by * len - ny * spread) * s);
            const QPen saved = p.pen();
            p.setPen(stroke(color, s, kStroke * 0.5));
            p.setBrush(color);
            p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
            p.setPen(saved);
        };

        // Shared magnifier (circle + handle) for zoom and search.
        auto magnifier = [&] {
            p.drawEllipse(QPointF(10 * s, 10 * s), 6 * s, 6 * s);
            line(14.4, 14.4, 20, 20);
        };

        // A rotation arrow: an almost-closed circle whose opening sits at the top,
        // with a solid arrowhead continuing the stroke into that opening - the
        // conventional rotate mark (head at the top left going clockwise, top
        // right going counter-clockwise).
        //
        // Angles here are SCREEN degrees: 0 at 3 o'clock, growing clockwise
        // because y points down. Qt's arc API measures counter-clockwise, so
        // every angle handed to arcMoveTo/arcTo is negated. The arc stops short of
        // the opening by kHeadSpan so the arrowhead - whose base sits exactly on
        // the arc's end and whose tip reaches on along the tangent - reads as the
        // stroke's continuation rather than a blob stuck to a circle.
        auto rotateArrow = [&](bool clockwise) {
            constexpr qreal cx = 12, cy = 12, r = 7.3;
            constexpr qreal kGapHalf = 35.0;  // half the opening at the top
            constexpr qreal kHeadSpan = 18.0; // arc given up to the arrowhead
            constexpr qreal kHeadLen = 3.8;
            const qreal a0 = clockwise ? 305.0 : 235.0;
            const qreal span = 360.0 - 2 * kGapHalf - kHeadSpan;
            const qreal a1 = a0 + (clockwise ? span : -span);

            const QRectF box((cx - r) * s, (cy - r) * s, 2 * r * s, 2 * r * s);
            QPainterPath arc;
            arc.arcMoveTo(box, -a0);
            arc.arcTo(box, -a0, clockwise ? -span : span);
            p.drawPath(arc);

            const qreal ar = a1 * kPi / 180.0;
            const QPointF base(cx + r * std::cos(ar), cy + r * std::sin(ar));
            // Unit tangent in the direction of travel.
            qreal tx = -std::sin(ar), ty = std::cos(ar);
            if (!clockwise) { tx = -tx; ty = -ty; }
            arrow(base.x() + tx * kHeadLen, base.y() + ty * kHeadLen, tx, ty, kHeadLen, 2.5);
        };

        // A tray the extract/merge arrows drop into or rise out of.
        auto tray = [&](qreal top) {
            path(pen().to(5, top).l(5, 18.6).q(5, 20, 6.4, 20)
                      .l(17.6, 20).q(19, 20, 19, 18.6).l(19, top).pp);
        };

        // Folded-corner page: body plus the little triangular flap.
        auto page = [&](qreal x, qreal y, qreal w, qreal h, qreal fold) {
            const qreal r = x + w, b = y + h;
            path(pen().to(x, y).l(r - fold, y).l(r, y + fold).l(r, b).l(x, b).shut().pp);
            path(pen().to(r - fold, y).l(r - fold, y + fold).l(r, y + fold).pp);
        };

        switch (id) {
        // ---- toolbar ------------------------------------------------------
        case Glyph::Open:
            // Fluent's two-part folder: a rounded body with the back flap above.
            rrect(3, 8, 18, 10.5, 1.6);
            path(pen().to(3.7, 8).l(3.7, 6.3).l(9, 6.3).l(10.9, 8).pp);
            break;
        case Glyph::PrevPage:
            p.setPen(stroke(color, s, kStroke * 1.15));
            poly({{14.5, 5}, {8.5, 12}, {14.5, 19}});
            break;
        case Glyph::NextPage:
            p.setPen(stroke(color, s, kStroke * 1.15));
            poly({{9.5, 5}, {15.5, 12}, {9.5, 19}});
            break;
        case Glyph::ChevronDown:
            p.setPen(stroke(color, s, kStroke * 1.15));
            poly({{6, 9.5}, {12, 15.5}, {18, 9.5}});
            break;
        case Glyph::Search:
            magnifier();
            break;
        // Bare arithmetic signs rather than a magnifier: the zoom buttons sit
        // either side of the zoom box, so the box already says "zoom" and the
        // buttons only have to say "more" and "less". A touch heavier than the
        // nominal stroke because a one- or two-rule glyph carries no other mass
        // to hold its own against the drawn icons beside it.
        case Glyph::ZoomOut:
            p.setPen(stroke(color, s, kStroke * 1.3));
            line(5, 12, 19, 12);
            break;
        case Glyph::ZoomIn:
            p.setPen(stroke(color, s, kStroke * 1.3));
            line(5, 12, 19, 12);
            line(12, 5, 12, 19);
            break;
        case Glyph::FitMode: // page centred in a frame
            rrect(3, 4, 18, 16, 1.5);
            rrect(8.5, 7.5, 7, 9, 0.6);
            break;
        case Glyph::RotateLeft:
            rotateArrow(false);
            break;
        case Glyph::RotateRight: // also Document > Rotate pages
            rotateArrow(true);
            break;
        case Glyph::Print:
            rrect(6.5, 2.5, 11, 5.5, 0.6);   // paper (top)
            rrect(3.5, 7.5, 17, 8.5, 1.2);   // body
            rrect(6.5, 13, 11, 7.5, 0.6);    // output (bottom)
            dot(6.6, 11, 0.8);               // LED
            break;
        case Glyph::Copy:
            rrect(8, 4.5, 11, 13, 1);
            rrect(5, 7.5, 11, 13, 1);
            break;
        case Glyph::Save: // floppy disk
            path(pen().to(4.5, 4.5).l(15.5, 4.5).l(19.5, 8.5).l(19.5, 19.5)
                      .l(4.5, 19.5).shut().pp);
            rrect(8, 4.5, 6, 4, 0.5);    // shutter
            rrect(7, 12.5, 10, 7, 0.5);  // label
            break;
        case Glyph::FillForm: { // pencil
            QPolygonF body;
            for (const QPointF &pt : {QPointF(4.6, 19.4), QPointF(8.2, 18.4),
                                      QPointF(19.3, 7.3), QPointF(16.6, 4.6),
                                      QPointF(5.5, 15.7)})
                body << QPointF(pt.x() * s, pt.y() * s);
            p.drawPolygon(body);
            line(5.5, 15.7, 8.2, 18.4); // nib
            break;
        }
        case Glyph::Ocr: {
            // Spell out the otherwise rather abstract operation inside the capture
            // frame. These are purpose-drawn letterforms rather than font text, so
            // their weight and spacing stay stable at every native icon size.
            const qreal arm = 3.4;
            for (const auto &c : {std::array<qreal, 4>{2.8, 3.2, 1, 1},
                                  std::array<qreal, 4>{21.2, 3.2, -1, 1},
                                  std::array<qreal, 4>{2.8, 20.8, 1, -1},
                                  std::array<qreal, 4>{21.2, 20.8, -1, -1}}) {
                line(c[0], c[1], c[0] + arm * c[2], c[1]);
                line(c[0], c[1], c[0], c[1] + arm * c[3]);
            }

            // The wordmark owns most of the interior height. At the toolbar's
            // real 16 px size this yields seven useful letter pixels instead of
            // the cramped five-pixel knot produced by the first implementation.
            p.setPen(stroke(color, s, kStroke * 1.48));
            rrect(4.3, 7.4, 4.2, 9.2, 0.85); // O

            // C: leave a generous right-side aperture so it cannot close up at 16 px.
            p.drawArc(QRectF(9.3 * s, 7.4 * s, 4.2 * s, 9.2 * s),
                      55 * 16, 250 * 16);

            // R: stem, bowl and diagonal leg. The open bowl keeps the three compact
            // letters from becoming a single dark knot at toolbar size.
            line(14.6, 16.6, 14.6, 7.4);
            path(pen().to(14.6, 7.4).l(17.2, 7.4).q(19.5, 7.4, 19.5, 10.0)
                      .q(19.5, 12.6, 17.2, 12.6).l(14.6, 12.6).pp);
            line(17.0, 12.6, 19.7, 16.6);
            break;
        }
        case Glyph::Measure:
            // |<->| extent marker: two upright end bars, a double arrow between.
            p.setPen(stroke(color, s, kStroke * 1.45));
            line(2.8, 3.5, 2.8, 20.5);
            line(21.2, 3.5, 21.2, 20.5);
            p.setPen(stroke(color, s));
            line(5.8, 12, 18.2, 12);
            arrow(5.8, 12, -1, 0, 3.0, 2.6);
            arrow(18.2, 12, 1, 0, 3.0, 2.6);
            break;
        case Glyph::Document: // Document button, tab glyph, generic page
            page(6, 3, 12, 18, 4);
            break;
        case Glyph::Menu:
            p.setPen(stroke(color, s, kStroke * 1.06));
            for (qreal y : {7.5, 12.0, 16.5})
                line(4.5, y, 19.5, y);
            break;

        // ---- hamburger menu ------------------------------------------------
        case Glyph::FitPage:
            rrect(5, 3, 14, 18, 1.5);
            break;
        case Glyph::FitWidth:
            line(3, 12, 21, 12);
            arrow(3, 12, -1, 0, 4.0, 3.6);
            arrow(21, 12, 1, 0, 4.0, 3.6);
            break;
        case Glyph::FullScreen:
            poly({{4, 9}, {4, 4}, {9, 4}});
            poly({{20, 9}, {20, 4}, {15, 4}});
            poly({{4, 15}, {4, 20}, {9, 20}});
            poly({{20, 15}, {20, 20}, {15, 20}});
            break;
        case Glyph::ContinuousScroll:
            // A vertical strip of pages: the middle page whole, the neighbours
            // running past the icon's top and bottom edges (open-ended shapes),
            // so the strip reads as scrolling on endlessly.
            poly({{6.5, 0.6}, {6.5, 4.2}, {17.5, 4.2}, {17.5, 0.6}});
            rrect(6.5, 7.6, 11, 8.8, 1);
            poly({{6.5, 23.4}, {6.5, 19.8}, {17.5, 19.8}, {17.5, 23.4}});
            break;
        case Glyph::SinglePage:
            // One page with text lines (distinct from FitPage's bare outline).
            rrect(6.5, 3, 11, 18, 1.5);
            line(9.2, 8.2, 14.8, 8.2);
            line(9.2, 12, 14.8, 12);
            line(9.2, 15.8, 14.8, 15.8);
            break;
        case Glyph::TwoPageSpread:
            // An open book: two facing pages meeting at a centre spine, with the
            // soft outer curve Fluent gives its book shapes.
            path(pen().to(12, 6.2).q(9, 4.6, 4.2, 4.6).l(4.2, 18.4)
                      .q(9, 18.4, 12, 20).q(15, 18.4, 19.8, 18.4)
                      .l(19.8, 4.6).q(15, 4.6, 12, 6.2).shut().pp);
            line(12, 6.2, 12, 20); // spine
            break;
        case Glyph::Outline:
            for (qreal y : {6.0, 12.0, 18.0}) {
                line(8, y, 21, y);
                dot(4.2, y, 1.0);
            }
            break;
        case Glyph::Thumbnails:
            rrect(4, 4, 7, 7, 1);
            rrect(13, 4, 7, 7, 1);
            rrect(4, 13, 7, 7, 1);
            rrect(13, 13, 7, 7, 1);
            break;
        case Glyph::Comments: // toolbar Comment and the Comments panel
            rrect(3.5, 4.5, 17, 11, 3.5);            // bubble body
            poly({{8, 15.3}, {6.5, 20}, {11.5, 15.3}}); // tail (bottom left)
            break;
        case Glyph::SelectAll: {
            QPen dash = stroke(color, s);
            dash.setStyle(Qt::CustomDashLine);
            dash.setDashPattern({3.0, 2.4});
            p.setPen(dash);
            rrect(4, 4, 16, 16, 2);
            break;
        }
        case Glyph::HighlightFields: {
            // Two form fields wearing the wash the viewer paints over them: the
            // outlines say "fields", the tint inside says "highlighted". The
            // pencil that would otherwise be the obvious mark is already Fill
            // Forms, which sits directly above this item in the menu.
            QColor wash(color);
            wash.setAlphaF(0.32f);
            p.setBrush(wash);
            // Barely rounded: at a capsule radius the pair reads as two pills
            // stacked, not as the boxes a form draws its fields in.
            rrect(3, 4.6, 18, 5.4, 0.5);
            rrect(3, 14.0, 18, 5.4, 0.5);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case Glyph::UiTheme: {
            // Filled crescent: a disc with an offset disc subtracted out.
            QPainterPath outer;
            outer.addEllipse(QPointF(12 * s, 12 * s), 8.6 * s, 8.6 * s);
            QPainterPath cut;
            cut.addEllipse(QPointF(15.6 * s, 9.4 * s), 7.1 * s, 7.1 * s);
            p.fillPath(outer.subtracted(cut), color);
            break;
        }
        case Glyph::Sun:
            // Stroked disc with eight rays. Same visual weight as the crescent so
            // the comfort toggle does not jump when it swaps.
            p.drawEllipse(QPointF(12 * s, 12 * s), 4.4 * s, 4.4 * s);
            for (int i = 0; i < 8; ++i) {
                const qreal a = i * kPi / 4;
                const qreal c = std::cos(a), sn = std::sin(a);
                line(12 + 6.6 * c, 12 + 6.6 * sn, 12 + 8.8 * c, 12 + 8.8 * sn);
            }
            break;
        case Glyph::DocumentTheme:
            rrect(5, 3, 14, 18, 1.5);
            line(5, 12, 19, 12);
            break;
        case Glyph::AlwaysOnTop: {
            // Regular 5-point star, first point at the top.
            QPolygonF star;
            const qreal cx = 12, cy = 12.4, rO = 8.7, rI = 3.7;
            for (int i = 0; i < 10; ++i) {
                const qreal a = -kPi / 2 + i * kPi / 5;
                const qreal r = (i % 2 == 0) ? rO : rI;
                star << QPointF((cx + r * std::cos(a)) * s, (cy + r * std::sin(a)) * s);
            }
            star << star.first();
            p.drawPolyline(star);
            break;
        }
        case Glyph::Settings:
            // Cog: a centre hole ringed by eight radial teeth.
            p.drawEllipse(QPointF(12 * s, 12 * s), 3.0 * s, 3.0 * s);
            p.setPen(stroke(color, s, kStroke * 1.18));
            for (int i = 0; i < 8; ++i) {
                const qreal a = i * kPi / 4;
                const qreal c = std::cos(a), sn = std::sin(a);
                line(12 + 5.4 * c, 12 + 5.4 * sn, 12 + 8.0 * c, 12 + 8.0 * sn);
            }
            break;
        case Glyph::Keyboard:
            rrect(3, 6, 18, 12, 2);
            dot(7, 10.5, 0.9);
            dot(11, 10.5, 0.9);
            dot(15, 10.5, 0.9);
            line(7, 14.2, 17, 14.2); // spacebar
            break;
        case Glyph::About:
            p.drawEllipse(QPointF(12 * s, 12 * s), 9 * s, 9 * s);
            line(12, 11, 12, 16.2);
            dot(12, 8, 0.95);
            break;

        // ---- Document popover ---------------------------------------------
        case Glyph::ExtractPages:
            line(12, 3, 12, 13);
            arrow(12, 13, 0, 1);
            tray(17);
            break;
        case Glyph::SplitPages:
            rrect(3, 5, 7, 14, 1);
            rrect(14, 5, 7, 14, 1);
            break;
        case Glyph::MergePages:
            line(12, 3, 12, 13);
            arrow(12, 3, 0, -1);
            tray(14);
            break;
        case Glyph::Security: // padlock
            rrect(5, 10, 14, 10, 1.5);
            {
                QPainterPath shackle;
                shackle.moveTo(8 * s, 10 * s);
                shackle.lineTo(8 * s, 7 * s);
                shackle.arcTo(QRectF(8 * s, 3 * s, 8 * s, 8 * s), 180, -180);
                shackle.lineTo(16 * s, 10 * s);
                path(shackle);
            }
            break;
        case Glyph::Delete: // trash can
            line(4.5, 6.5, 19.5, 6.5); // lid
            poly({{9.5, 6.5}, {9.5, 4.5}, {14.5, 4.5}, {14.5, 6.5}}); // handle
            path(pen().to(6.5, 6.5).l(7.5, 20).l(16.5, 20).l(17.5, 6.5).pp);
            for (qreal x : {10.0, 12.0, 14.0})
                line(x, 9.5, x, 17.0); // ribs
            break;

        // ---- context menus and panels --------------------------------------
        case Glyph::OpenInNewWindow:
            path(pen().to(13, 5).l(6.2, 5).q(4.5, 5, 4.5, 6.7).l(4.5, 17.3)
                      .q(4.5, 19, 6.2, 19).l(16.8, 19).q(18.5, 19, 18.5, 17.3)
                      .l(18.5, 11).pp);
            line(12.5, 11, 19.7, 3.8);
            poly({{15.2, 3.6}, {19.9, 3.6}, {19.9, 8.3}});
            break;
        case Glyph::ShowAllWindows:
            path(pen().to(8.5, 6.2).q(8.5, 4.5, 10.2, 4.5).l(17.8, 4.5)
                      .q(19.5, 4.5, 19.5, 6.2).l(19.5, 13.8)
                      .q(19.5, 15.5, 17.8, 15.5).pp);
            rrect(4, 8.5, 11.5, 11, 1.6);
            break;
        case Glyph::Close:
            p.setPen(stroke(color, s, kStroke * 1.1));
            line(6.8, 6.8, 17.2, 17.2);
            line(17.2, 6.8, 6.8, 17.2);
            break;
        case Glyph::Broom:
            line(19.7, 4.3, 12.6, 11.4);  // handle
            line(11.3, 12.7, 14.2, 15.6); // head top edge
            line(11.3, 12.7, 4.8, 16.4);  // bristles
            line(12.8, 14.2, 6.8, 19.2);
            line(14.2, 15.6, 9.6, 20.3);
            break;
        case Glyph::DragHandle:
            // Six dots in two columns - the universal "grip and drag" mark. Dots
            // rather than the stacked lines the same idea is sometimes drawn with:
            // Menu is already three stacked lines, and at 16 px the two would be
            // the same picture.
            for (qreal x : {9.0, 15.0})
                for (qreal y : {5.5, 12.0, 18.5})
                    dot(x, y, 1.5);
            break;
        }
    });
}

QIcon ocrWordmark(const QColor &color)
{
    QIcon icon;
    for (int height : {16, 20, 24, 32, 48}) {
        const int width = qRound(height * 1.7);
        QPixmap pm(width, height);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = height / 20.0;
        // Unlike square Fluent pictographs, this compact wordmark must not be
        // optically enlarged: the extra scale crowds its letters into the frame.
        p.setPen(stroke(color, s, kStroke * 0.78));
        p.setBrush(Qt::NoBrush);

        auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
            p.drawLine(QPointF(x1 * s, y1 * s), QPointF(x2 * s, y2 * s));
        };

        // Four scan corners around a wide, dominant OCR wordmark.
        constexpr qreal arm = 3.7;
        for (const auto &c : {std::array<qreal, 4>{2.5, 3.2, 1, 1},
                              std::array<qreal, 4>{31.5, 3.2, -1, 1},
                              std::array<qreal, 4>{2.5, 16.8, 1, -1},
                              std::array<qreal, 4>{31.5, 16.8, -1, -1}}) {
            line(c[0], c[1], c[0] + arm * c[2], c[1]);
            line(c[0], c[1], c[0], c[1] + arm * c[3]);
        }

        p.setPen(stroke(color, s, kStroke * 0.86));
        p.drawRoundedRect(QRectF(8.3 * s, 6.3 * s, 4.2 * s, 7.4 * s),
                          1.15 * s, 1.15 * s); // O
        p.drawArc(QRectF(14.3 * s, 6.3 * s, 4.2 * s, 7.4 * s),
                  55 * 16, 250 * 16); // C
        line(20.4, 13.7, 20.4, 6.3); // R stem
        QPainterPath bowl;
        bowl.moveTo(20.4 * s, 6.3 * s);
        bowl.lineTo(23.0 * s, 6.3 * s);
        bowl.quadTo(25.1 * s, 6.3 * s, 25.1 * s, 8.3 * s);
        bowl.quadTo(25.1 * s, 10.3 * s, 23.0 * s, 10.3 * s);
        bowl.lineTo(20.4 * s, 10.3 * s);
        p.drawPath(bowl);
        line(23.0, 10.3, 25.7, 13.7); // R leg

        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon glyphBadged(Glyph base, Glyph badge, const QColor &color)
{
    // Compose per rendered size: the base pictograph, then the badge at ~60%
    // over the bottom-right corner. The margin behind the badge is erased - not
    // filled with the menu surface colour - so the composite stays readable over
    // the hover wash and any other row state.
    const QIcon baseIcon  = glyph(base, color);
    const QIcon badgeIcon = glyph(badge, color);
    QIcon icon;
    for (const QSize &sq : baseIcon.availableSizes()) {
        const int sz = sq.width();
        QPixmap pm = baseIcon.pixmap(sz, sz); // QPainter::begin() detaches the copy
        QPainter p(&pm);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const int bs = sz * 3 / 5;
        const QRect badgeBox(sz - bs, sz - bs, bs, bs);
        const int margin = qMax(2, sz / 8);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(badgeBox.adjusted(-margin, -margin, 0, 0), Qt::black);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawPixmap(badgeBox, badgeIcon.pixmap(sz, sz));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx)
{
    return glyph(id, color).pixmap(qMax(1, sizePx), qMax(1, sizePx));
}

QPixmap spinChevron(bool down, const QColor &color, int sizePx)
{
    QPixmap pm(sizePx, sizePx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(sizePx * 0.12); // ~1.45 px at 12 px, matching the house stroke
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    const qreal c = sizePx / 2.0;
    const qreal hw = sizePx * 0.27; // chevron half-width
    const qreal hh = sizePx * 0.17; // chevron half-height
    // "v" points down (down step), "^" points up (up step).
    const qreal yNear = down ? c - hh : c + hh; // the two outer tips
    const qreal yApex = down ? c + hh : c - hh; // the centre point
    p.drawPolyline(QPolygonF{{c - hw, yNear}, {c, yApex}, {c + hw, yNear}});
    p.end();
    return pm;
}

} // namespace mervin::icons
