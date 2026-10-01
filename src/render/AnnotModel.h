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

// Owns markup and note mutations for one Document. withPdfDocument serializes access against
// page loads. Standard /Annots and /Contents persist through Document::savePdfTo.
// Annotation identity is (page, PDF object number), valid until save/reopen. Recreate this
// model when rebinding the viewer; it must not outlive its Document.
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
