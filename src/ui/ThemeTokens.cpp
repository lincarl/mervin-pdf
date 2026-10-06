#include "ui/ThemeTokens.h"

#include "render/ComfortTransform.h"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>

namespace mervin {

namespace {

// A token colour from its CSS hex digits, opaque or at `alpha` (0-255).
QColor rgb(QRgb value, int alpha = 255)
{
    return QColor(qRed(value), qGreen(value), qBlue(value), alpha);
}

// Borders and washes are one tinted ink at different strengths, so they read
// correctly over whatever surface sits underneath: Nord's snow in dark, the
// Cool slate ink in light.
constexpr QRgb kNordWash = 0xeceff4;
constexpr QRgb kSlateWash = 0x1f2a37;

// The dark text a light accent carries in each theme.
constexpr QRgb kNordOnAccent = 0x232831;
constexpr QRgb kSlateOnAccent = 0x1a1f24;

// The dark chrome: "Nord". Surfaces step up from the well to the popover.
theme::Chrome nordChrome()
{
    theme::Chrome t;
    t.dark = true;
    t.well    = rgb(0x242933); // inset wells: inputs, search, page field
    t.status  = rgb(0x272c36); // title bar + status bar
    t.bar     = rgb(0x2a303b); // tab strip / secondary bars
    t.window  = rgb(0x2e3440); // main toolbar + window chrome
    t.fill    = rgb(0x353c4a); // active tab body
    t.popover = rgb(0x3b4252); // menu / popover surface
    t.canvas  = rgb(0x424853); // behind the pages

    t.inkPrimary    = rgb(0xe5e9f0);
    t.ink           = rgb(0xdbe0ea);
    t.inkBody       = rgb(0xc5ccd8);
    t.inkSoft       = rgb(0xa5afc1);
    t.inkFaint      = rgb(0x98a2b4);
    t.inkDisabled   = rgb(0x646e81);
    t.inkMenuHeader = rgb(0xa8b2c4);
    t.inkDanger     = rgb(0xec8f97);
    t.inkLink       = rgb(0x9dbde3);

    t.border               = rgb(kNordWash, 0x1c);
    t.borderStrong         = rgb(kNordWash, 0x40);
    t.borderBar            = rgb(kNordWash, 0x0f);
    t.borderPush           = rgb(kNordWash, 0x26);
    t.borderDisabled       = rgb(kNordWash, 0x14);
    t.borderPopover        = rgb(kNordWash, 0x2b);
    t.borderPopoverControl = rgb(kNordWash, 0x3d);
    t.hover                = rgb(kNordWash, 0x14);
    t.pressed              = rgb(kNordWash, 0x1f);
    t.rowHover             = rgb(kNordWash, 0x12);
    t.separator            = rgb(kNordWash, 0x14);
    t.hairline             = rgb(kNordWash, 0x1c);
    t.scrollThumb          = rgb(kNordWash, 0x42);
    t.scrollThumbHover     = rgb(kNordWash, 0x6b);
    return t;
}

// The light chrome: "Cool slate". White controls on cool blue-grey bands; the
// popover is a step below white so white page thumbnails keep an edge in lists.
theme::Chrome slateChrome()
{
    theme::Chrome t;
    t.dark = false;
    t.well    = rgb(0xebeef2);
    t.status  = rgb(0xdee3e9);
    t.bar     = rgb(0xe8ecf0);
    t.window  = rgb(0xf5f7f9);
    t.fill    = rgb(0xffffff);
    t.popover = rgb(0xf0f3f6);
    t.canvas  = rgb(0x3c3f44);

    t.inkPrimary    = rgb(0x1a1f24);
    t.ink           = rgb(0x1f2328);
    t.inkBody       = rgb(0x31373e);
    t.inkSoft       = rgb(0x59636e);
    t.inkFaint      = rgb(0x58616b);
    t.inkDisabled   = rgb(0xa3abb5);
    t.inkMenuHeader = rgb(0x5f6873);
    t.inkDanger     = rgb(0xc4232e);
    t.inkLink       = rgb(0x0969da);

    t.border               = rgb(kSlateWash, 0x29);
    t.borderStrong         = rgb(kSlateWash, 0x4c);
    t.borderBar            = rgb(kSlateWash, 0x1f);
    t.borderPush           = rgb(kSlateWash, 0x33);
    t.borderDisabled       = rgb(kSlateWash, 0x14);
    t.borderPopover        = rgb(kSlateWash, 0x59);
    t.borderPopoverControl = rgb(kSlateWash, 0x59);
    t.hover                = rgb(kSlateWash, 0x12);
    t.pressed              = rgb(kSlateWash, 0x21);
    t.rowHover             = rgb(kSlateWash, 0x0f);
    t.separator            = rgb(kSlateWash, 0x1f);
    t.hairline             = rgb(kSlateWash, 0x2e);
    t.scrollThumb          = rgb(kSlateWash, 0x47);
    t.scrollThumbHover     = rgb(kSlateWash, 0x73);
    return t;
}

QColor tint(const QColor &base, double alpha)
{
    QColor c = base;
    c.setAlphaF(alpha);
    return c;
}

double luminance(const QColor &c)
{
    const auto lin = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The text colour for `fill`: white or the theme's dark ink, whichever has more
// WCAG contrast. Deep blues get white; the light accents both themes use, and a
// pale custom accent, get dark text.
QColor inkOn(const QColor &fill, bool dark)
{
    const QColor darkInk = rgb(dark ? kNordOnAccent : kSlateOnAccent);
    const QColor white(Qt::white);
    return contrast(fill, darkInk) > contrast(fill, white) ? darkInk : white;
}

// A hover step for the accent: toward white in dark, where the accent is light
// and carries dark text, and darker in light, where it carries white.
QColor accentHoverFor(const QColor &accent, bool dark)
{
    if (!dark)
        return accent.darker(110);
    const auto mix = [](int v) { return v + qRound((255 - v) * 0.18); };
    return QColor(mix(accent.red()), mix(accent.green()), mix(accent.blue()));
}

// The palette both themes install for painters outside QSS. An invalid accent
// leaves the accent roles to the platform (see lightPalette).
QPalette paletteFor(const theme::Chrome &t, const QColor &accent)
{
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.ink);
    // Base paints item views the sheet leaves alone: the thumbnail and Recent
    // lists, check box interiors and tooltips. Dark keeps them in the well; light
    // uses the popover tone, a step below white, so white thumbnails keep an edge.
    p.setColor(QPalette::Base, t.dark ? t.well : t.popover);
    p.setColor(QPalette::AlternateBase, t.bar);
    p.setColor(QPalette::Text, t.inkPrimary);
    p.setColor(QPalette::Button, t.window);
    p.setColor(QPalette::ButtonText, t.ink);
    p.setColor(QPalette::BrightText, t.dark ? QColor(Qt::white) : QColor(Qt::black));
    p.setColor(QPalette::PlaceholderText, t.inkFaint);
    p.setColor(QPalette::ToolTipBase, t.popover);
    p.setColor(QPalette::ToolTipText, t.ink);
    if (accent.isValid()) {
        p.setColor(QPalette::Highlight, accent);
        p.setColor(QPalette::HighlightedText, inkOn(accent, t.dark));
        p.setColor(QPalette::Accent, accent);
    }
    p.setColor(QPalette::Link, t.inkLink);
    p.setColor(QPalette::LinkVisited, t.inkLink);
    // 3D bevel roles: rarely painted under QSS, but keep them on the ramp. Light
    // matters more than it looks - QWidget::foregroundRole() turns a Dark/Shadow
    // background role into Light ink, which is how the floating tool panels used
    // to draw invisible labels - so it stays a surface tone, never a text colour.
    if (t.dark) {
        p.setColor(QPalette::Light, t.popover);
        p.setColor(QPalette::Midlight, t.fill);
        p.setColor(QPalette::Mid, t.well);
        p.setColor(QPalette::Dark, rgb(0x1e222a));
    } else {
        p.setColor(QPalette::Light, QColor(Qt::white));
        p.setColor(QPalette::Midlight, t.window.darker(105));
        p.setColor(QPalette::Mid, t.window.darker(130));
        p.setColor(QPalette::Dark, t.window.darker(160));
    }
    p.setColor(QPalette::Shadow, Qt::black);
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                                  QPalette::HighlightedText, QPalette::PlaceholderText})
        p.setColor(QPalette::Disabled, r, t.inkDisabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.popover);
    p.setColor(QPalette::Disabled, QPalette::Base, t.status);
    return p;
}

} // namespace

bool theme::isDark(const QPalette &pal)
{
    const QColor window = pal.color(QPalette::Window);
    if (window.alpha() == 0 && qGuiApp) {
        const QPalette app = QGuiApplication::palette();
        if (app.color(QPalette::Window).alpha() != 0)
            return isDark(app);
    }
    return window.lightness() < 128;
}

theme::Chrome theme::chrome(const QPalette &pal, const QColor &accent)
{
    Chrome t = isDark(pal) ? nordChrome() : slateChrome();
    t.accent      = accent.isValid() ? accent : defaultAccent(t.dark);
    t.accentHover = accentHoverFor(t.accent, t.dark);
    t.onAccent    = inkOn(t.accent, t.dark);
    t.accentWash  = tint(t.accent, 0.15);
    return t;
}

QPalette theme::darkPalette(const QColor &accent)
{
    return paletteFor(nordChrome(), accent);
}

QPalette theme::lightPalette(const QColor &accent)
{
    return paletteFor(slateChrome(), accent);
}

theme::Chrome theme::chrome(const QPalette &pal)
{
    QColor accent = pal.color(QPalette::Accent);
    if (!accent.isValid())
        accent = pal.color(QPalette::Highlight);
    return chrome(pal, accent);
}

const theme::Doc &theme::doc()
{
    static const Doc d = [] {
        Doc v;
        v.paperNormal   = QColor(Qt::white);
        v.paperInverted = QColor(Qt::black);
        v.paperComfort  = QColor(comfort::kRampBg[0], comfort::kRampBg[1], comfort::kRampBg[2]);
        v.pageBorder    = QColor(0x20, 0x20, 0x20);

        v.findMatch        = QColor(255, 230, 0, 96);
        v.findMatchCurrent = QColor(255, 140, 0, 150);
        v.textSelection    = QColor(70, 130, 230, 80);

        v.formField         = QColor(70, 130, 230, 38);
        v.formFieldRequired = QColor(230, 90, 70, 45);
        v.formFieldBorder   = QColor(70, 130, 230, 140);
        v.formFieldFocus    = QColor(255, 140, 0, 220);

        v.formEditorSurface      = QColor(0xdb, 0xea, 0xfe);
        v.formEditorInk          = QColor(0x14, 0x14, 0x14);
        v.formEditorBorder       = QColor(70, 130, 230);
        v.formEditorSelection    = QColor(0x33, 0x99, 0xff);
        v.formEditorSelectionInk = QColor(Qt::white);

        v.noteCard             = QColor(0xff, 0xfe, 0xf0);
        v.noteCardBorder       = QColor(0xb9, 0xb9, 0xb9);
        v.noteCardEditor       = QColor(Qt::white);
        v.noteCardEditorBorder = QColor(0xcf, 0xcf, 0xcf);
        v.noteCardInk          = QColor(0x1a, 0x1a, 0x1a);
        v.noteCardLabelInk     = QColor(0x3a, 0x3a, 0x3a);

        v.swatchRingDark  = QColor(0xe6, 0xed, 0xf3);
        v.swatchRingLight = QColor(0x1a, 0x1a, 0x1a);
        v.swatchRingRest  = QColor(255, 255, 255, 89); // rgba(255,255,255,0.35)
        v.chipBorder      = QColor(0x8a, 0x8a, 0x8a);
        v.colorChipEdge   = QColor(0, 0, 0, 89);       // rgba(0,0,0,0.35)

        v.measureHandle         = QColor(Qt::white);
        v.measureAreaAlpha      = 40;
        v.measureLabelAlpha     = 235;
        v.measureLabelEdgeAlpha = 60;
        return v;
    }();
    return d;
}

QColor theme::pageAccent(const QColor &accent, const QColor &paper)
{
    if (accent.isValid() && contrast(accent, paper) >= 3.0)
        return accent;
    return defaultAccent(paper.lightness() < 128);
}

QColor theme::legibleAccent(const QColor &accent, bool dark)
{
    const Chrome t = dark ? nordChrome() : slateChrome();
    const auto reads = [&t](const QColor &c) {
        return contrast(c, t.window) >= 3.0 && contrast(c, t.well) >= 3.0;
    };
    if (reads(accent))
        return accent;
    QColor c = accent.toHsl();
    for (int l = c.lightness(); !reads(c) && (dark ? l < 255 : l > 0);) {
        l = std::clamp(l + (dark ? 2 : -2), 0, 255);
        c.setHsl(c.hslHue(), c.hslSaturation(), l);
    }
    return QColor(c.rgb());
}

QColor theme::defaultAccent(bool dark)
{
    return dark ? rgb(0x88c0d0)  // Nord's frost blue
                : rgb(0x0969da); // the Cool slate blue
}

const theme::Brand &theme::brand()
{
    static const Brand b = [] {
        Brand v;
        v.starFill      = QColor(0xf5, 0xc4, 0x00);
        v.starEdge      = QColor(0xd4, 0xa5, 0x00);
        v.starEmptyEdge = QColor(0xb8, 0xb8, 0xb8);
        v.searchMatch   = QColor(255, 230, 0, 150);
        return v;
    }();
    return b;
}

QString theme::swatchStyle(const QColor &c, bool checked, bool dark)
{
    const Doc &d = doc();
    const QColor ring = checked ? (dark ? d.swatchRingDark : d.swatchRingLight)
                                : (dark ? d.swatchRingRest : d.chipBorder);
    return QStringLiteral("QToolButton{background:%1;border:%2px solid %3;border-radius:4px;}")
        .arg(css(c))
        .arg(checked ? 2 : 1)
        .arg(css(ring));
}

QString theme::css(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alphaF(), 0, 'f', 3);
}

} // namespace mervin
