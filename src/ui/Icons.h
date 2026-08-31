#pragma once

#include <QIcon>

class QColor;
class QPixmap;

namespace mervin::icons {

// The app's single icon language: "Fluent Outline", drawn with QPainter on a
// 24-unit grid. One set, one look, every surface - toolbar, hamburger menu,
// Document popover, context menus, tabs and panels.
//
// Nothing here depends on an icon font. Until v1.45.0 the toolbar drew Segoe
// Fluent Icons / MDL2 font glyphs while the menus drew painted pictographs, so
// the two families sat side by side on Windows, and on Linux (no Segoe fonts,
// the codepoints are Private-Use-Area with no fallback) the toolbar silently
// dropped to an incomplete painted stand-in - "Fill in form", "Comment" and the
// tab glyph all rendered as a generic document. Painting everything ourselves
// makes Windows and Linux identical by construction.
//
// House style, applied by glyph() to every pictograph below:
//   - 1.45 grid-unit stroke, round caps and joins, no fill
//   - 1.09x optical size (Fluent sits large in its box); the pen is divided by
//     the same factor so the stroke still lands at 1.45
//   - generous corner rounding (2.1x the base radius)
//   - solid triangular arrow terminals
// Icons are painted natively at 16/20/24/32/48 px - not scaled from one pixmap -
// so thin strokes stay sharp wherever Qt asks for them.
enum class Glyph {
    // Toolbar
    Open,          // folder (also "open containing folder")
    PrevPage,      // chevron left
    NextPage,      // chevron right
    ChevronDown,   // dropdown affordance
    Search,        // magnifier
    ZoomOut,       // minus sign
    ZoomIn,        // plus sign
    FitMode,       // page centred in a frame
    RotateLeft,    // circular arrow, counter-clockwise
    RotateRight,   // circular arrow, clockwise (also Document > Rotate pages)
    Print,         // printer
    Copy,          // two stacked pages
    Save,          // floppy disk
    FillForm,      // pencil
    Ocr,           // OCR wordmark inside a capture frame
    Measure,       // |<->| extent marker
    Document,      // folded-corner page (Document button, tab glyph, generic page)
    Menu,          // hamburger

    // Hamburger menu
    FitPage,          // page outline
    FitWidth,         // horizontal double arrow
    FullScreen,       // four expanding corner brackets
    ContinuousScroll, // vertical strip of pages running past the edges
    SinglePage,       // one page with text lines
    TwoPageSpread,    // open book
    Outline,          // bulleted list
    Thumbnails,       // 2x2 grid
    Comments,         // speech bubble (toolbar Comment and the Comments panel)
    SelectAll,        // dashed rectangle
    HighlightFields,  // two form fields carrying the viewer's tint
    UiTheme,          // crescent moon (document-theme menu, comfort toggle at rest)
    Sun,              // sun disc with rays (comfort toggle, active state)
    DocumentTheme,    // page split by a mid line
    AlwaysOnTop,      // star
    Settings,         // gear
    Keyboard,         // keyboard
    About,            // info circle

    // Document popover
    ExtractPages, // down arrow into a tray
    SplitPages,   // two side-by-side pages
    MergePages,   // up arrow into a tray
    Security,     // padlock
    Delete,       // trash can (Document > Delete pages, context-menu deletes)

    // Context menus and panels
    OpenInNewWindow, // window with an out-arrow
    ShowAllWindows,  // two overlapping windows
    Close,           // cross
    Broom,           // sweep entries away
    DragHandle,      // six-dot grip: press here to drag a row
};

// The pictograph tinted to `color` (the palette's WindowText, Theme::iconInk, or
// the accent tone the Document popover uses).
QIcon glyph(Glyph id, const QColor &color);

// The OCR toolbar mark deliberately uses a wide canvas so its three-letter
// wordmark remains readable. Menus may scale this icon down to their square icon
// slot; the toolbar gives it the native 34:20 aspect ratio.
QIcon ocrWordmark(const QColor &color);

// glyph() with a second, smaller glyph badged over the base's bottom-right
// corner - the margin behind the badge is erased (not surface-filled) so the
// composite stays correct over any row background, hover wash included. Used by
// the file context menu's "Copy folder path" / "Copy file path".
QIcon glyphBadged(Glyph base, Glyph badge, const QColor &color);

// A single pictograph as a transparent `sizePx`-square pixmap, for the places
// that paint an icon directly instead of handing Qt a QIcon (the split Open
// button draws its own ChevronDown over the menu strip).
QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx);

// A single up ("^") or down ("v") chevron, antialiased and tinted to `color`,
// rendered into a transparent `sizePx`-square pixmap. Used to supply the
// spin-box stepper arrows via QSS (which can only reference arrow glyphs as
// image: url(...) - the native arrows are illegibly small under our stylesheet).
QPixmap spinChevron(bool down, const QColor &color, int sizePx);

} // namespace mervin::icons
