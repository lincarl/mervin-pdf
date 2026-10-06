#pragma once

#include <QIcon>

class QAbstractButton;
class QColor;
class QPixmap;

namespace mervin::icons {

// The approved application artwork with all native Windows scale sizes. The
// embedded PNG frames also keep Linux independent of an ICO image plugin.
QIcon applicationIcon();

// Lucide icons (https://lucide.dev, ISC), vendored unmodified under resources/icons/lucide and
// compiled as Qt resources. See its README for the pinned version. glyph() substitutes
// currentColor, uses a 1.5-unit stroke that never draws thinner than about one device pixel
// (so 16 px renders get about 1.6 units), and rasterizes at 16/20/24/32/48 px.
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

    // Settings menu (the rest of its pages reuse the glyphs above)
    Appearance, // palette
    Viewing,    // eye

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

// Transparent sizePx-square glyph. strokeWidth uses the 24-unit Lucide grid; a valid fill
// colours closed shapes. strokeWidth 0 keeps the set's stroke and renders natively at
// `devicePixelRatio` (0 means the application's, as QIcon::pixmap() uses), so a painter
// that draws the pixmap unscaled should pass its own widget's ratio. An explicit
// strokeWidth renders exactly sizePx device pixels.
QPixmap glyphPixmap(Glyph id, const QColor &color, int sizePx, double strokeWidth = 0,
                    const QColor &fill = QColor(), qreal devicePixelRatio = 0);

// Set a text-free button glyph using active/disabled ButtonText colours. Re-render on polish or
// palette changes, including theme switches.
void setButtonGlyph(QAbstractButton *button, Glyph glyph, int iconPx);

// A single up or down chevron (Lucide chevron-up / chevron-down) as a transparent
// `sizePx`-square pixmap. Used to supply the spin-box stepper arrows and the
// combo-box arrow via QSS, which can only reference images through
// image: url(...) - the native arrows are illegibly small under our stylesheet.
QPixmap spinChevron(bool down, const QColor &color, int sizePx);

} // namespace mervin::icons
