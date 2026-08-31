#pragma once

#include <QColor>
#include <QRectF>
#include <QString>

#include <array>

namespace mervin {

namespace annot {

// The markup colour presets offered in the annotation tool panel and the inline
// edit popup. Index 0 (yellow) is the default highlight colour and matches the
// example files. Kept in the model layer (QColor is Qt6::Gui, which mervin_core
// already links) so both the model and the UI share one source of truth.
inline std::array<QColor, 6> palette()
{
    return {QColor(0xFF, 0xF2, 0x00), QColor(0x7C, 0xE6, 0x5A), QColor(0xFF, 0x5A, 0x5A),
            QColor(0x5A, 0xB4, 0xFF), QColor(0xFF, 0x8C, 0xC8), QColor(0xFF, 0xAA, 0x3C)};
}

inline QColor defaultColor() { return QColor(0xFF, 0xF2, 0x00); }

// The swatch-chip stylesheet used to live here, but its ring colours are UI
// chrome rather than document content: it is now theme::swatchStyle() in
// ui/ThemeTokens.h, with the rest of the app's colours.

} // namespace annot

// The kind of markup annotation Mervin creates and manages, collapsed from
// MuPDF's pdf_annot_type into the set the annotation UI distinguishes. Any other
// subtype found in a document (Square, Ink, Stamp, …) maps to Other: surfaced for
// display/listing but not created by Mervin. A pure value-type header, like
// FormTypes.h / MeasureTypes.h - no fz_*/pdf_* leakage.
enum class AnnotType {
    Highlight,  // text highlight        (PDF_ANNOT_HIGHLIGHT)
    Underline,  // text underline        (PDF_ANNOT_UNDERLINE)
    StrikeOut,  // text strike-through    (PDF_ANNOT_STRIKE_OUT)
    Text,       // sticky-note comment    (PDF_ANNOT_TEXT)
    Other,      // any other subtype (surfaced, not created here)
};

// True for the three text-markup kinds (built from a text selection's quads).
inline bool isTextMarkup(AnnotType t)
{
    return t == AnnotType::Highlight || t == AnnotType::Underline || t == AnnotType::StrikeOut;
}

// The active sub-mode of the Comment tool (what a page gesture does while the
// Comment panel is open). Select is the idle/pointer state (text selection, no
// annotation gesture) - it is also what the panel shows when the Measure tool has
// taken over the single active gesture. Markup builds a highlight/underline/
// strike-out from the current text selection; Note drops a sticky-note comment.
enum class AnnotSubMode { Select, Markup, Note };

// One annotation on a page, as the UI sees it. A plain value type, mirroring
// FormField. `rect` is in app page-point space (top-left origin, y-down, 72 dpi,
// unrotated - the same space Measurement::pts and FormField::rect live in), so it
// survives zoom/rotation; for text markup it is the union of the marked lines,
// for a sticky note it is the icon box. `id` is the PDF object number - stable
// for the life of one open document (until a save+reopen re-enumerates), used to
// re-find the annotation for edit/delete without relying on a list index.
struct Annotation {
    int page = -1;
    int id = 0;             // PDF object number (stable per open document)
    AnnotType type = AnnotType::Other;
    QRectF rect;            // app page-point space (markup union / note icon box)
    QColor color;           // stroke colour (markup) / note colour
    QString contents;       // /Contents - the comment text (may be empty)
    QString author;         // /T - the author/title
    qint64 modifiedMs = 0;  // /M as epoch ms (0 if absent); for sidebar display

    // Whether Mervin lets the user edit/recolour/delete this annotation. The
    // managed markup kinds are editable; foreign subtypes are shown read-only.
    bool editable() const { return type != AnnotType::Other; }
    // Whether a comment can be attached (every managed kind, incl. sticky notes).
    bool canComment() const { return editable(); }
};

} // namespace mervin
