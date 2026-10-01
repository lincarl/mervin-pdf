#pragma once

#include "render/GeometryTypes.h"
#include "render/MeasureTypes.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

// Forward declarations of MuPDF C types (defined in <mupdf/fitz.h> / <mupdf/pdf.h>).
// Re-typedef of an identical type is legal in C++; this keeps the heavy MuPDF
// headers out of the rest of the codebase.
typedef struct fz_context fz_context;
typedef struct fz_document fz_document;
typedef struct pdf_document pdf_document;

namespace mervin {

// One document-outline (bookmark) entry. page is 0-based, or -1 if the entry
// has no page destination. Plain value type (no fz_* leakage).
struct OutlineItem
{
    QString title;
    int page = -1;
    std::vector<OutlineItem> children;
};

// One clickable PDF link annotation under the cursor. `uri` is MuPDF's canonical
// link string: web links are normal URLs, internal destinations look like
// "#page=4&view=FitB". `page` is 0-based and set only when the URI resolves to a
// page in this document.
struct PdfLinkTarget
{
    QString uri;
    int page = -1;

    bool valid() const { return !uri.isEmpty(); }
    bool isInternal() const { return page >= 0; }
};

// KiCad embeds item/net properties as JavaScript Link annotations
// (ShM([["Reference = R7"], ...])). Mervin does not execute JavaScript; it reads
// the static string list and shows it in a copy-friendly popup.
struct PdfItemProperties
{
    int page = -1;
    QRectF rect; // app page-point space, matching FormField::rect / Annotation::rect
    QStringList values;

    bool valid() const { return page >= 0 && !values.isEmpty(); }
};

class Document;

// A job locks this gate before touching a document. Destruction clears the
// pointer under the same lock; the gate can outlive the document.
struct DocumentLifetime
{
    std::mutex mutex;
    Document *document = nullptr;
};

// Owns a MuPDF document, cached page sizes and title; created by RenderEngine. All handle
// access, including page loading and text extraction on cloned contexts, requires
// accessMutex(). MuPDF context locks protect shared caches, not document state. Playback of an
// independent display list may run outside the document lock.
class Document
{
public:
    Document(fz_context *baseCtx, fz_document *doc); // takes ownership of doc
    ~Document();

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    int pageCount() const { return static_cast<int>(pageSizes_.size()); }
    QSizeF pageSize(int pageNo) const; // points (72 dpi), unrotated
    QString title() const { return title_; }

    // The page's embedded measurement metadata (/VP viewports with rectilinear
    // /Measure dictionaries), used to auto-detect a drawing's scale the way
    // Adobe's measure tool does. Empty when the PDF carries none. Read on demand
    // under the access lock and cached; safe to call from the UI thread.
    PageMeasurement pageMeasurement(int pageNo) const;

    // The page's flattened vector geometry (line segments + deduped endpoints in
    // page-point space), harvested by running the page through a path-collecting
    // device. Backs the measuring tool's vertex/edge snapping. Extracted on
    // demand under the access lock and cached; safe to call from the UI thread.
    // Empty for pages with no vector content (or on failure).
    PageGeometry pageGeometry(int pageNo) const;

    // Map app page points (zero top-left, y-down, 72 dpi) to PDF user space, including rotation
    // and MediaBox origin. Return {a,b,c,d,e,f}: x'=a*x+c*y+e, y'=b*x+d*y+f. Inverse of the
    // viewport transform used by pageMeasurement(); identity for non-PDF/failure. Read under
    // the access lock.
    std::array<double, 6> pagePointToPdfMatrix(int pageNo) const;

    fz_document *handle() const { return doc_; }

    // The document outline / bookmarks (empty if none). Extracted on demand
    // under the access lock.
    std::vector<OutlineItem> outline() const;

    // PDF link annotation at `pagePoint` in page-point space, if any. Text-only
    // URLs are handled by TextIndex. Internal links carry a resolved page.
    std::optional<PdfLinkTarget> linkTargetAt(int pageNo, QPointF pagePoint) const;

    // Link URI at `pagePoint` in page-point space, or empty when no PDF link
    // annotation is under the point. Compatibility wrapper used by older tests.
    QString linkAt(int pageNo, QPointF pagePoint) const;

    // KiCad item/net properties at `pagePoint`, if the clicked Link annotation
    // carries a static JavaScript ShM(...) property list. Empty for non-PDF docs
    // and ordinary navigation links.
    std::optional<PdfItemProperties> itemPropertiesAt(int pageNo, QPointF pagePoint) const;

    // True when this document carries a fillable AcroForm (an /AcroForm catalog
    // entry with at least one field). Cached; false for non-PDF documents. Drives
    // the Fill-Forms action's enabled state. Read on demand under the access lock.
    bool hasForm() const;

    // Cached /Mervin_Measurements catalog check under the access lock; false for non-PDFs.
    // Gates the second qpdf parse used to restore measurements.
    bool hasMervinMeasurements() const;

    // True when this is a PDF (pdf_specifics succeeds) - i.e. annotations and form
    // fields can be created and written. False for the other formats MuPDF can
    // open (XPS, CBZ, image documents). Read under the access lock.
    bool isPdf() const;

    // Mutate the live PDF only through fn under accessMutex(). Neither the supplied context nor
    // document may escape. Return false for non-PDFs or MuPDF exceptions, true otherwise. Const
    // refers to the wrapper, not PDF contents.
    bool withPdfDocument(const std::function<void(fz_context *, pdf_document *)> &fn) const;

    // Full MuPDF rewrite of live form/annotation edits to tmpPath, preserving encryption.
    // Caller atomically replaces the destination. Returns false with *error for non-PDFs or
    // write failures.
    bool savePdfTo(const QString &tmpPath, QString *error = nullptr) const;

    // Serializes all access to handle() across threads (see class note).
    std::mutex &accessMutex() const { return access_; }
    std::shared_ptr<DocumentLifetime> lifetime() const { return lifetime_; }

private:
    std::shared_ptr<DocumentLifetime> lifetime_ = std::make_shared<DocumentLifetime>();
    fz_context *ctx_ = nullptr;  // base context, not owned (owned by RenderEngine)
    fz_document *doc_ = nullptr; // owned
    std::vector<QSizeF> pageSizes_;
    QString title_;
    mutable std::mutex access_;
    mutable std::vector<std::optional<PageMeasurement>> measureCache_; // lazy, per page
    mutable std::vector<std::optional<PageGeometry>> geomCache_;       // lazy, per page
    mutable std::optional<bool> formCache_;                            // lazy: has /AcroForm
    mutable std::optional<bool> mervinBlobCache_; // lazy: has /Mervin_Measurements
};

} // namespace mervin
