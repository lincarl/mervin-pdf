#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace mervin {

// The application's colour vocabulary - the ONE place a UI colour is written
// down. Everything that paints chrome reads from here:
//
//   * Theme::buildStyleSheet interpolates the tokens into the app-wide QSS,
//   * the popovers that build their own local sheets (PdfPropertiesPopup,
//     AnnotPopup, the inline form editors) interpolate the same tokens,
//   * QPainter code (ViewerWidget's canvas and overlays, MainWindow's toolbar
//     dividers, RecentFilesPanel's icons) reads the QColor directly.
//
// So a colour change is a one-line edit in ThemeTokens.cpp, and light and dark
// can never drift apart by accident.
//
// Deliberately NOT here, because these are document data rather than UI chrome:
//   * annot::palette() / annot::defaultColor() in render/AnnotTypes.h - the
//     markup preset colours, written into the PDF's annotation objects.
//   * EmitStyle's stroke/fill in render/MeasureContent.h - PDF content-stream
//     colours for exported measurement marks.
//   * ComfortTransform's ramp endpoints - the Comfort document theme's pixel
//     transform, pinned by tst_comfort_transform. theme::doc() re-exports the
//     backdrop so the viewer does not re-type the literal.
//
// This module lives in mervin_core (values only - QColor/QPalette, no widgets)
// so every test target can link it without pulling in the application.
namespace theme {

// Which half of the vocabulary a palette selects. One definition of "dark",
// used everywhere (it was open-coded in eight places).
bool isDark(const QPalette &pal);

// ── Chrome: the theme-dependent half ────────────────────────────────────────
// Dark values are the "Compact Slate" spec from the Mervin PDF design project.
// Light values are the md-easy language, and the roles that should track the OS
// (body ink, input wells, primary ink) are taken from the live QPalette so light
// mode keeps following system accent/contrast changes as it always has.
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

// The explicit application palette for the dark chrome. The QSS covers the
// styled widgets, but panels, item delegates, icon tinting and the tab-drag
// indicator all read palette roles, so installing this keeps them on the same
// slate ramp. Light mode uses the platform palette unchanged, which is why there
// is no lightPalette() counterpart. Defined here, next to the values it is built
// from, so no other file needs the raw slate constants.
QPalette darkPalette(const QColor &accent);

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

    // Measurement overlays. The stroke is the UI accent; these are the parts
    // that are not.
    QColor measureHandle;      // vertex handle fill
    int measureAreaAlpha;      // area-polygon wash opacity
    int measureLabelAlpha;     // value-pill background opacity
    int measureLabelEdgeAlpha; // value-pill hairline opacity
};

const Doc &doc();

// The accent used when the setting is "system" and the platform palette carries
// no usable Accent/Highlight role. Dark gets the Compact Slate design accent,
// light the brand blue - a single fallback would put the light blue on the dark
// chrome, which is where selection and focus stop matching the spec.
QColor defaultAccent(bool dark);

// ── Brand ───────────────────────────────────────────────────────────────────
// The app icon and the Recent list's document tiles. resources/icons/make_icon.py
// generates mervin.ico offline and carries its own copy of the gradient - keep
// the two in step by hand if the brand ever changes.
struct Brand
{
    QColor gradientStart;
    QColor gradientEnd;
    QColor onBrand;     // the white glyph drawn on the gradient
    QColor onBrandSoft; // the fainter "text lines" in the document tile
    QColor starFill;    // a favourited row's star
    QColor starEdge;
    QColor starEmptyEdge;
    // Search-match highlight in the Recent list. Deliberately the same yellow the
    // find bar paints over a page, so a match looks the same wherever it appears.
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
