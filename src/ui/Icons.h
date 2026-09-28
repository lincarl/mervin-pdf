#pragma once

#include <QIcon>

class QAbstractButton;
class QColor;
class QPixmap;

namespace mervin::icons {

// The app's single icon language: Lucide (https://lucide.dev, ISC licence). One
// set, one look, every surface - toolbar, hamburger menu, Document popover,
// context menus, tabs, panels and the stylesheet's own indicator images.
//
// The SVGs are vendored unmodified in resources/icons/lucide (see its README for
// the pinned version and how to add one) and compiled in as a Qt resource, so
// nothing depends on an icon font or on the platform: Windows and Linux render
// the same files. glyph() substitutes the requested ink for Lucide's
// stroke="currentColor", draws the stroke at 1.75 units instead of Lucide's 2
// (closer to the weight of the app's text), and rasterizes each icon natively at
// 16/20/24/32/48 px, so strokes stay sharp wherever Qt asks for a size.
enum class Glyph {
    // Toolbar
    Open,          // folder-open (also "open containing folder")
    PrevPage,      // chevron-left
    NextPage,      // chevron-right
    ChevronDown,   // chevron-down: dropdown affordance
    Search,        // search
    ZoomOut,       // minus
    ZoomIn,        // plus
    FitMode,       // fullscreen: the fit page / fit width toggle
    RotateLeft,    // rotate-ccw
    RotateRight,   // rotate-cw (also Document > Rotate pages)
    Print,         // printer
    Copy,          // copy
    Save,          // save
    FillForm,      // pencil-line
    Ocr,           // scan-text: read the text inside a selection
    Measure,       // ruler-dimension-line
    Document,      // file (Document button, tab glyph, generic page)
    Menu,          // menu

    // Hamburger menu
    FitPage,          // shrink
    FitWidth,         // move-horizontal
    FullScreen,       // maximize
    ContinuousScroll, // gallery-vertical
    SinglePage,       // rectangle-vertical
    TwoPageSpread,    // book-open
    Outline,          // list
    Thumbnails,       // layout-grid
    Comments,         // message-square (toolbar Comment and the Comments panel)
    SelectAll,        // square-dashed
    HighlightFields,  // highlighter
    UiTheme,          // moon (document-theme menu, comfort toggle at rest)
    Sun,              // sun (comfort toggle, active state)
    DocumentTheme,    // contrast
    AlwaysOnTop,      // pin
    Settings,         // settings
    Keyboard,         // keyboard
    About,            // info

    // Document popover
    ExtractPages, // file-output
    SplitPages,   // split
    MergePages,   // merge
    Security,     // lock
    Delete,       // trash-2 (Document > Delete pages, context-menu deletes)

    // Context menus, panels and stylesheet indicators
    OpenInNewWindow, // square-arrow-out-up-right
    ShowAllWindows,  // app-window
    Close,           // x (also the tab close cross)
    Broom,           // brush-cleaning: sweep entries away
    DragHandle,      // grip-vertical: press here to drag a row
    FileText,        // file-text: pages the Extract strip folds away
    Check,           // check: menu check marks and the checkbox tick
    Star,            // star: favourite files in the Recent list (filled when set)
};

// The pictograph tinted to `color` (the palette's WindowText, Theme::iconInk, or
// the accent tone the Document popover uses).
QIcon glyph(Glyph id, const QColor &color);

// glyph() with a second, smaller glyph badged over the base's bottom-right
// corner - the margin behind the badge is erased (not surface-filled) so the
// composite stays correct over any row background, hover wash included. Used by
// the file context menu's "Copy folder path" / "Copy file path".
QIcon glyphBadged(Glyph base, Glyph badge, const QColor &color);

// A single pictograph as a transparent `sizePx`-square pixmap, for the places
// that paint an icon directly instead of handing Qt a QIcon (the split Open
// button draws its own ChevronDown over the menu strip; the stylesheet's
// generated indicator images). `strokeWidth` overrides Lucide's stroke, in its
// 24-unit grid, for the small indicators that need a heavier line to read; 0
// keeps the set's own. A valid `fill` fills the glyph's closed shapes (the
// Recent list's favourite star); by default Lucide icons are outlines only.
QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx, double strokeWidth = 0,
                    const QColor &fill = QColor());

// Puts `glyph` on a text-free button, drawn in the button's own foreground colour
// - the palette's ButtonText, which a stylesheet `color:` rule sets - with a
// disabled variant in the palette's disabled ButtonText, and keeps it that way:
// the icon is rendered again whenever the button is polished or its palette
// changes, as on a theme switch. For the small close, remove and -/+ buttons that
// used to draw a text character in that colour.
void setButtonGlyph(QAbstractButton *button, Glyph glyph, int iconPx);

// A single up or down chevron (Lucide chevron-up / chevron-down) as a transparent
// `sizePx`-square pixmap. Used to supply the spin-box stepper arrows and the
// combo-box arrow via QSS, which can only reference images through
// image: url(...) - the native arrows are illegibly small under our stylesheet.
QPixmap spinChevron(bool down, const QColor &color, int sizePx);

} // namespace mervin::icons
