#pragma once

#include "render/AnnotTypes.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <optional>
#include <vector>

namespace mervin {

class Document;

// The sole owner of all markup-annotation mutation in the app (highlights,
// underlines, strike-outs, and sticky-note comments), keeping MuPDF confined to
// render/ (see design §6, mirrored for §6b). Bound to one open Document; every
// access runs through Document::withPdfDocument(), which holds the document's
// access mutex and yields the live pdf_document - so an annotation edit is
// serialised against the render workers' page loads exactly like a form-field
// edit or any other object-model touch.
//
// Annotations persist as standard PDF /Annots (/Subtype /Highlight | /Underline |
// /StrikeOut | /Text, with /Contents carrying the comment), written by
// Document::savePdfTo (the same full MuPDF rewrite forms use). There is no private
// blob to restore - re-opening a saved file just re-enumerates /Annots. GUI-free;
// lives in mervin_core.
//
// Identity: an Annotation is addressed by (page, id) where id is the PDF object
// number, stable for the life of one open document. A save+reopen builds a fresh
// AnnotModel against the reopened Document, so ids never have to survive a save.
//
// Lifetime: holds the Document by reference and must not outlive it. The viewer
// recreates the AnnotModel whenever it binds a new Document, so the reference is
// always valid for the model's lifetime.
class AnnotModel
{
public:
    explicit AnnotModel(const Document &doc);

    // The managed/visible annotations on a page, in MuPDF annotation order. Lazily
    // enumerated and cached in app page-point space. Returns a reference valid
    // until the next mutation of that page (which invalidates its cache) or
    // reset(). Includes foreign subtypes (Annotation::editable() == false) so they
    // still show in the comments list, but Mervin never mutates those.
    const std::vector<Annotation> &pageAnnots(int pageNo) const;

    // Every annotation across the document (for the comments sidebar), page order
    // then in-page order. Enumerates lazily via pageAnnots and concatenates.
    std::vector<Annotation> allAnnots() const;

    // The single annotation addressed by (page, id), or nullopt if it no longer
    // exists. Cheap (reads the page cache).
    std::optional<Annotation> annot(int page, int id) const;

    // --- creation (returns the new annotation's id, or -1 on failure) ---
    // A text-markup annotation built from one or more line rectangles in app
    // page-point space (the merged per-line rects of a text selection). `type`
    // must be a text-markup kind (Highlight/Underline/StrikeOut). The optional
    // comment is stored as /Contents.
    int addTextMarkup(int page, AnnotType type, const std::vector<QRectF> &lineRects,
                      const QColor &color, const QString &author, const QString &contents = {});

    // A sticky-note comment (/Text annotation) whose icon is anchored at `at`
    // (app page-point space). `contents` is the comment body.
    int addTextNote(int page, QPointF at, const QColor &color, const QString &author,
                    const QString &contents);

    // --- mutation (return true iff something actually changed) ---
    bool setContents(int page, int id, const QString &contents);
    bool setColor(int page, int id, const QColor &color);
    bool remove(int page, int id);

    // Whether any annotation has been created/edited/deleted since open / the last
    // clearDirty().
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // Drop all cached enumeration (the bound Document's object model may have
    // changed underneath us). Does not clear the dirty flag.
    void reset();

private:
    void invalidatePage(int page);

    const Document &doc_;
    mutable std::vector<std::optional<std::vector<Annotation>>> cache_; // lazy, per page
    bool dirty_ = false;
};

} // namespace mervin
