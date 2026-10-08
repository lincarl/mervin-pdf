#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace mervin {

// Shared chrome colours for QSS, local popup styles and QPainter code; values live in
// ThemeTokens.cpp. This widget-free module belongs to mervin_core.
// PDF annotation colours (AnnotTypes), exported measurement colours (EmitStyle), and the
// Comfort pixel ramp remain document data; theme::doc() re-exports the Comfort backdrop.
namespace theme {

// Which half of the vocabulary a palette selects. One definition of "dark",
// used everywhere (it was open-coded in eight places). A palette whose Window
// colour is fully transparent says nothing about the theme - the stylesheet gives
// widgets with "background: transparent" such a palette - so it is judged by the
// application palette instead.
bool isDark(const QPalette &pal);

// ── Chrome: the theme-dependent half ────────────────────────────────────────
// Dark values are the "Nord" scheme: lifted blue-grey surfaces with no near-black
// wells, body text at about 9.4:1 and a soft frost accent that carries dark text,
// chosen for comfort in long sessions. Light values are the "Cool slate" scheme:
// cool blue-grey bands with white controls around the same slate canvas. Every
// token is explicit in both themes; only the accent comes from the caller.
struct Chrome
{
    bool dark = false;

    // Surfaces, layered from the deepest well up to floating popovers.
    QColor well;     // inset input wells: line edits, spin boxes, combos, lists
    QColor status;   // title bar + status bar (the darkest chrome band)
    QColor bar;      // tab strip, dock titles, secondary bars
    QColor window;   // window/toolbar chrome and dialog bodies
    QColor fill;     // filled bodies: selected tab, checked segment, active pill
    QColor popover;  // menus, tooltips, floating tool panels, property cards
    QColor canvas;   // the backdrop behind document pages

    // Ink, brightest first.
    QColor inkPrimary;    // headings, active values, text inside input wells
    QColor ink;           // body text (the palette's WindowText)
    QColor inkBody;       // toolbar labels and runtime-tinted icons
    QColor inkSoft;       // muted secondary copy, group captions
    QColor inkFaint;      // meta text: counters, status bar, page totals
    QColor inkDisabled;   // disabled controls and empty check indicators
    QColor inkMenuHeader; // the uppercase menu section captions
    QColor inkDanger;     // the one red: a blocking validation message
    QColor inkLink;       // hyperlinks in property cards

    // Borders and interaction states. All translucent, so they read correctly
    // over whatever surface sits underneath.
    QColor border;         // the default 1px control outline
    QColor borderStrong;   // hover/pressed outline
    QColor borderBar;      // chrome row separators (toolbar/status/tab row)
    QColor borderPush;     // outlined push-button / pill edge
    QColor borderDisabled; // outline of a disabled control
    // A floating surface has to read as an object in front of the page, and the
    // controls on it have to read as controls - the borderless toolbar treatment
    // does not work there, so these two sit well above `border`.
    QColor borderPopover;        // the edge of a floating panel / popup card
    QColor borderPopoverControl; // controls sitting on such a surface
    QColor hover;          // button hover wash
    QColor pressed;        // button pressed wash
    QColor rowHover;       // the softer wash used for list/menu rows
    QColor separator;      // menu separators, splitters, dock handles
    QColor hairline;       // the custom-painted toolbar dividers
    QColor scrollThumb;
    QColor scrollThumbHover;

    // Accent: one colour carries selection, focus and the primary button.
    QColor accent;
    QColor accentHover;
    QColor onAccent;   // black or white, whichever stays legible on `accent`
    QColor accentWash; // 15% accent - the active tint for checked tools
};

// Resolve the vocabulary for `pal`, with `accent` as the accent colour (which
// the caller resolves from Settings - see Theme::accentColor).
Chrome chrome(const QPalette &pal, const QColor &accent);
// Same, taking the accent from the palette's Accent/Highlight role. Correct for
// paint code: applyApp() installs the resolved accent into the palette.
Chrome chrome(const QPalette &pal);

// Palettes for painters outside QSS, including delegates and tab dragging, from the
// Nord and Cool slate tokens. An invalid accent leaves the Accent, Highlight and
// HighlightedText roles unset, so once installed they resolve against the platform
// palette. That is how Theme::applyApp() reads the OS accent.
QPalette darkPalette(const QColor &accent);
QPalette lightPalette(const QColor &accent);

// ── Document surface: the theme-independent half ────────────────────────────
// Colours painted on or over a PDF page. They do not follow the UI theme: they
// have to work on the page's own paper, and a highlight that changed colour with
// the chrome would be unrecognisable.
struct Doc
{
    // Page backing, painted under an area that has not been rasterised yet, so
    // it must match the paper each document theme produces.
    QColor paperNormal;
    QColor paperInverted;
    QColor paperComfort; // ComfortTransform's backdrop endpoint
    QColor pageBorder;   // the 1px frame round every page rect

    // Find + selection overlays.
    QColor findMatch;
    QColor findMatchCurrent;
    QColor textSelection;

    // Form-field affordances (Fill Forms mode).
    QColor formField;
    QColor formFieldRequired;
    QColor formFieldBorder;
    QColor formFieldFocus;

    // The inline form editor: a light input surface that must stay legible on a
    // white page, so it overrides the app chrome in both themes.
    QColor formEditorSurface;
    QColor formEditorInk;
    QColor formEditorBorder;
    QColor formEditorSelection;
    QColor formEditorSelectionInk;

    // The sticky-note edit card, likewise a light card in both themes.
    QColor noteCard;
    QColor noteCardBorder;
    QColor noteCardEditor;
    QColor noteCardEditorBorder;
    QColor noteCardInk;
    QColor noteCardLabelInk;

    // Annotation chrome: the ring round a colour swatch and the border round the
    // colour chip in the comments sidebar (a pale annotation colour needs one).
    QColor swatchRingDark;  // ring on the selected swatch, dark chrome
    QColor swatchRingLight; // ring on the selected swatch, light card
    QColor swatchRingRest;  // unselected swatch edge on the dark chrome
    QColor chipBorder;      // unselected swatch edge on a light card, and the
                            // comment row's colour chip
    QColor colorChipEdge;   // the Settings accent swatch: a fixed dark wash that
                            // stays visible on any colour the user picks

    // Measurement overlays. The stroke is the UI accent as pageAccent() resolves
    // it for the paper; these are the parts that are not.
    QColor measureHandle;      // vertex handle fill
    int measureAreaAlpha;      // area-polygon wash opacity
    int measureLabelAlpha;     // value-pill background opacity
    int measureLabelEdgeAlpha; // value-pill hairline opacity
};

const Doc &doc();

// The accent for marks drawn on the page (measurements, snap markers, the open
// annotation's outline): the UI accent while it reaches 3:1 against `paper`, else
// the design accent of a theme with that paper's lightness. Dark chrome's pale
// accent would otherwise vanish on white paper, and a deep one on inverted paper.
QColor pageAccent(const QColor &accent, const QColor &paper);

// `accent` at the contrast the design accents keep in each theme, 3:1 on the
// window and in the well: lightened in dark and darkened in light, keeping its
// hue. An accent that already reads is returned as it is. For OS accents, which
// a desktop may tune for the other scheme.
QColor legibleAccent(const QColor &accent, bool dark);

// The design accent of each theme: the frost blue of Nord in dark and the Cool
// slate blue in light. "system" falls back to it when the desktop reports no
// accent (see Theme::accentColor). A single
// value would put a mid blue under dark button text, or a pale one on white.
QColor defaultAccent(bool dark);

// ── Brand ───────────────────────────────────────────────────────────────────
// Accent colours for favourite stars and search matches in the Recent list.
// Application artwork is embedded separately by ui/Icons.
struct Brand
{
    QColor starFill;    // a favourited row's star
    QColor starEdge;
    QColor starEmptyEdge;
    // Search-match highlight in the Recent list. Deliberately the same yellow the
    // find card highlights on a page, so a match looks the same wherever it appears.
    QColor searchMatch;
};

const Brand &brand();

// A colour-chip QToolButton's stylesheet, with a ring marking the active colour.
// Shared by the annotation comment card's swatches and the Settings default-colour
// picker so both read identically. `dark` picks ring colours that stay visible on
// the dark chrome; the sticky-note card keeps its light values.
QString swatchStyle(const QColor &c, bool checked, bool dark = false);

// ── QSS interpolation ───────────────────────────────────────────────────────
// Render a token for a stylesheet: "#rrggbb" when opaque, "rgba(r,g,b,a.aaa)"
// when translucent - the two forms the sheets already used.
QString css(const QColor &c);

} // namespace theme
} // namespace mervin
