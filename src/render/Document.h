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

// Owns an open MuPDF document and caches its per-page sizes and title.
// Created by RenderEngine::openDocument(). The render workers read handle()
// using their own cloned fz_context (MuPDF's documented multi-thread pattern).
//
// IMPORTANT: a single fz_document is NOT thread-safe. MuPDF's lock callbacks
// (fz_locks_context) only protect the shared store / glyph cache / allocator -
// they do NOT serialize access to one document's object cache and lazy loading.
// Every thread that touches this document's handle (render workers + the
// per-document TextIndex) must therefore hold accessMutex() around its
// page-load / content-parse / text-extraction. Rasterizing an already-built
// display list does not touch the document and can run outside the lock.
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

    // The affine transform that maps this page's app page-point space (top-left
    // origin, y-down, 72 dpi, unrotated - what Measurement::pts live in) into the
    // PDF's user space (y-up, MediaBox origin). Returned as {a,b,c,d,e,f} with
    //   x' = a*x + c*y + e,  y' = b*x + d*y + f  (MuPDF's fz_matrix convention).
    // It is the inverse of the content->app transform pageMeasurement() applies to
    // /VP BBoxes, so writers (flatten / annotate) place marks exactly where they
    // appear on screen, handling /Rotate and a non-zero MediaBox origin. Identity
    // for a non-PDF document or on failure. Read on demand under the access lock.
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

    // True when the PDF catalog carries Mervin's private measurement blob
    // (/Mervin_Measurements, written by MeasureExport::embedMervin). One dict
    // lookup on the already-parsed catalog, which is what makes it worth having:
    // it gates the qpdf reopen MeasureExport::readMervinBlob needs, and that
    // second full parse of the same file used to run on EVERY document open even
    // though almost no file carries the blob. Cached; false for non-PDFs. Read
    // under the access lock, so it is safe on the UI thread.
    bool hasMervinMeasurements() const;

    // True when this is a PDF (pdf_specifics succeeds) - i.e. annotations and form
    // fields can be created and written. False for the other formats MuPDF can
    // open (XPS, CBZ, image documents). Read under the access lock.
    bool isPdf() const;

    // Run `fn` against the live pdf_document under accessMutex() - the ONLY
    // sanctioned way to mutate the PDF object model (AcroForm field edits, see
    // FormModel; markup annotations, see AnnotModel). This serialises the edit
    // against the render workers' page loads exactly like every other handle()
    // access. `fn` receives the base context and the pdf_document and must let
    // neither escape the call. Returns false without calling `fn` for a non-PDF
    // document, false if `fn` raises a MuPDF exception, and true otherwise.
    // (Const: the PDF bytes change, but the Document object's own state does not -
    // like outline()/pageMeasurement() reading via handle().)
    bool withPdfDocument(const std::function<void(fz_context *, pdf_document *)> &fn) const;

    // Persist the live pdf_document - with all in-memory mutations (filled form
    // fields and created/edited annotations) - to `tmpPath` via a full MuPDF
    // rewrite (do_incremental = 0, do_encrypt = PDF_ENCRYPT_KEEP). The caller
    // swaps it over the original with the measurement atomic-swap flow. This is
    // the single MuPDF save path shared by FormModel and AnnotModel, since both
    // mutate this one live pdf_document. Returns false (and sets *error) on a
    // non-PDF document or write failure. Const for the same reason as above.
    bool savePdfTo(const QString &tmpPath, QString *error = nullptr) const;

    // Serializes all access to handle() across threads (see class note).
    std::mutex &accessMutex() const { return access_; }

private:
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
