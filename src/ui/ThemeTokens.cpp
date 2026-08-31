#include "ui/ThemeTokens.h"

#include "render/ComfortTransform.h"

#include <cmath>

namespace mervin {

namespace {

// ---- Compact Slate (dark chrome) -------------------------------------------
// Verbatim from the design project's tokens/colors.css ("Compact Slate" spec).
constexpr const char *kSlateWell    = "#10141d"; // inset wells: inputs, search, page field
constexpr const char *kSlateStatus  = "#161a23"; // title bar + status bar
constexpr const char *kSlateBar     = "#191e27"; // tab strip / secondary bars
constexpr const char *kSlateChrome  = "#1c222e"; // main toolbar + window chrome
constexpr const char *kSlateTabFill = "#1b2030"; // active tab body
constexpr const char *kSlateMenu    = "#212834"; // menu / popover surface

constexpr const char *kInkPrimary   = "#e6edf3"; // primary labels, active values
constexpr const char *kInkSecondary = "#d5dbe4"; // menu / dialog body text
constexpr const char *kInkBody      = "#c2c9d6"; // toolbar labels + icons
constexpr const char *kInkSoft      = "#8b94a5"; // icons at rest, secondary copy
constexpr const char *kInkFaint     = "#7c8494"; // meta, counters, status bar
constexpr const char *kInkDisabled  = "#5b626c"; // disabled controls, empty checkboxes

// A translucent grey: white over the dark chrome, black over the light chrome.
// Borders and washes built this way read correctly whatever surface is beneath.
QColor wash(bool dark, double alpha)
{
    QColor c(dark ? Qt::white : Qt::black);
    c.setAlphaF(alpha);
    return c;
}

QColor tint(const QColor &base, double alpha)
{
    QColor c = base;
    c.setAlphaF(alpha);
    return c;
}

// Black or white text, whichever stays legible on `fill` (WCAG relative
// luminance). Most blues land on white; a pale custom accent gets dark text.
QColor inkOn(const QColor &fill)
{
    const auto lin = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const double L = 0.2126 * lin(fill.red()) + 0.7152 * lin(fill.green())
                     + 0.0722 * lin(fill.blue());
    return L > 0.5 ? QColor(0x1a, 0x1a, 0x1a) : QColor(Qt::white);
}

} // namespace

bool theme::isDark(const QPalette &pal)
{
    return pal.color(QPalette::Window).lightness() < 128;
}

theme::Chrome theme::chrome(const QPalette &pal, const QColor &accent)
{
    const bool dark = isDark(pal);
    Chrome t;
    t.dark = dark;

    // Surfaces. The light well is a fixed step below the surface rather than the
    // palette's Base: Base is #ffffff, the same as `window`/`popover`/`fill`, so
    // light mode had no well/surface layering at all - every input and list was
    // white on white behind a 1.38:1 hairline, while dark had a clear inset.
    t.well    = dark ? QColor(kSlateWell) : QColor(0xf3, 0xf3, 0xf3);
    t.status  = dark ? QColor(kSlateStatus) : QColor(0xf3, 0xf3, 0xf3);
    t.bar     = dark ? QColor(kSlateBar) : QColor(0xf3, 0xf3, 0xf3);
    t.window  = dark ? QColor(kSlateChrome) : QColor(Qt::white);
    t.fill    = dark ? QColor(kSlateTabFill) : QColor(Qt::white);
    t.popover = dark ? QColor(kSlateMenu) : QColor(Qt::white);
    // The canvas behind the pages is dark in both themes - a light surround
    // would wash out the page it frames - but takes a step of the slate's warmth
    // under the dark chrome.
    t.canvas = dark ? QColor(0x3c, 0x3f, 0x44) : QColor(0x3a, 0x3a, 0x3c);

    // Ink. In light mode the body/primary roles come from the palette so the
    // app keeps following the OS text colour and high-contrast themes.
    t.ink           = pal.color(QPalette::WindowText);
    t.inkPrimary    = dark ? QColor(kInkPrimary) : pal.color(QPalette::Text);
    t.inkBody       = dark ? QColor(kInkBody) : t.ink;
    t.inkSoft       = dark ? QColor(kInkSoft) : wash(false, 0.55);
    t.inkFaint      = dark ? QColor(kInkFaint) : wash(false, 0.55);
    t.inkDisabled   = dark ? QColor(kInkDisabled) : wash(false, 0.30);
    t.inkMenuHeader = dark ? QColor(0x6b, 0x76, 0x86) : QColor(0x6b, 0x72, 0x80);
    t.inkDanger     = dark ? QColor(0xff, 0x8f, 0x8f) : QColor(0xb3, 0x26, 0x1e);
    t.inkLink       = dark ? QColor(0x58, 0xa6, 0xff) : QColor(0x00, 0x5a, 0x9e);

    // Borders and states.
    t.border           = wash(dark, dark ? 0.10 : 0.14);
    t.borderStrong     = wash(dark, dark ? 0.24 : 0.28);
    t.borderBar        = wash(dark, dark ? 0.05 : 0.14);
    t.borderPush       = wash(dark, dark ? 0.14 : 0.14);
    t.borderDisabled   = wash(dark, 0.08);
    t.borderPopover        = wash(dark, dark ? 0.24 : 0.45);
    t.borderPopoverControl = wash(dark, dark ? 0.30 : 0.45);
    t.hover            = wash(dark, dark ? 0.07 : 0.06);
    t.pressed          = wash(dark, dark ? 0.10 : 0.11);
    t.rowHover         = wash(dark, dark ? 0.06 : 0.05);
    t.separator        = wash(dark, dark ? 0.07 : 0.14);
    t.hairline         = wash(dark, dark ? 0.10 : 0.20);
    t.scrollThumb      = wash(dark, dark ? 0.28 : 0.26);
    t.scrollThumbHover = wash(dark, dark ? 0.44 : 0.42);

    // Accent.
    t.accent      = accent.isValid() ? accent : defaultAccent(dark);
    t.accentHover = dark ? t.accent.lighter(115) : t.accent.darker(110);
    t.onAccent    = inkOn(t.accent);
    t.accentWash  = tint(t.accent, 0.15);

    return t;
}

QPalette theme::darkPalette(const QColor &accent)
{
    const QColor window(kSlateChrome);
    const QColor text(kInkSecondary);
    const QColor disabled(kInkDisabled);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, QColor(kSlateWell));
    p.setColor(QPalette::AlternateBase, QColor(kSlateBar));
    p.setColor(QPalette::Text, QColor(kInkPrimary));
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(kInkFaint));
    p.setColor(QPalette::ToolTipBase, QColor(kSlateMenu));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, inkOn(accent));
    p.setColor(QPalette::Accent, accent);
    p.setColor(QPalette::Link, QColor(0x58, 0xa6, 0xff));
    p.setColor(QPalette::LinkVisited, QColor(0xa9, 0xc6, 0xff));
    // 3D bevel roles: rarely painted under QSS, but keep them on the ramp. Light
    // matters more than it looks - QWidget::foregroundRole() turns a Dark/Shadow
    // background role into Light ink, which is how the floating tool panels used
    // to draw invisible labels.
    p.setColor(QPalette::Light, QColor(0x2a, 0x31, 0x3a));
    p.setColor(QPalette::Midlight, QColor(0x24, 0x2b, 0x37));
    p.setColor(QPalette::Mid, QColor(0x12, 0x16, 0x1f));
    p.setColor(QPalette::Dark, QColor(0x0c, 0x0f, 0x16));
    p.setColor(QPalette::Shadow, Qt::black);
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                                  QPalette::HighlightedText, QPalette::PlaceholderText})
        p.setColor(QPalette::Disabled, r, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(kSlateMenu));
    p.setColor(QPalette::Disabled, QPalette::Base, QColor(kSlateStatus));
    return p;
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

QColor theme::defaultAccent(bool dark)
{
    return dark ? QColor(0x4f, 0x8c, 0xff)  // the Compact Slate design accent
                : QColor(0x00, 0x67, 0xc0); // the brand blue
}

const theme::Brand &theme::brand()
{
    static const Brand b = [] {
        Brand v;
        v.gradientStart = QColor(0x2a, 0xa7, 0xe0);
        v.gradientEnd   = QColor(0x0a, 0x4d, 0x8c);
        v.onBrand       = QColor(Qt::white);
        v.onBrandSoft   = QColor(255, 255, 255, 160);
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
