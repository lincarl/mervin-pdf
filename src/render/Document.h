#pragma once

#include "render/GeometryTypes.h"
#include "render/MeasureTypes.h"

#include <QByteArray>
#include <QPointF>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
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
// pointer under the same lock; the gate can outlive the document. Lock the gate
// before the document's access lock, never after it.
struct DocumentLifetime
{
    // How long a wait for the gate lasts before the waiter checks whether it still
    // wants the document (Document::Hold, the render workers).
    static constexpr std::chrono::milliseconds kStep{20};

    std::timed_mutex mutex;
    Document *document = nullptr;
    // Document::Hold objects that hold the gate or wait for it. While there is one,
    // the document's queries refuse on the screen thread (see Document::ScreenQuery).
    std::atomic<int> holds{0};
    // Document::ScreenQuery objects on the screen thread that were not refused. A
    // Hold waits for them before it takes the gate.
    std::atomic<int> screenQueries{0};
};

// Owns a MuPDF document, cached page sizes and title; created by RenderEngine. All handle
// access, including page loading and text extraction on cloned contexts, requires
// accessMutex(). MuPDF context locks protect shared caches, not document state. Playback of an
// independent display list may run outside the document lock.
//
// On the screen thread, the queries below return their empty value while a job holds
// the document or waits for it (see Hold and ScreenQuery), instead of waiting for the job.
class Document
{
public:
    // Lets a job thread use a document that a tab shows, for as long as it lives. It
    // locks only the lifetime gate. The job calls Document's methods as usual, and
    // each takes the access lock itself. Not for the screen thread, which never waits
    // for a job, and never taken while holding accessMutex(). The gate is not
    // recursive, so the holding thread releases the hold before it reads the document
    // through a TextIndex or OCR, destroys the document or waits for it to close.
    class Hold
    {
    public:
        // Waits for the gate in DocumentLifetime::kStep steps. Gives up, leaving
        // document() null, once `stop` returns true or the document has closed.
        explicit Hold(std::shared_ptr<DocumentLifetime> lifetime,
                      const std::function<bool()> &stop = {});
        ~Hold();
        Hold(const Hold &) = delete;
        Hold &operator=(const Hold &) = delete;

        // The held document, or null when the hold gave up.
        Document *document() const { return document_; }

    private:
        std::shared_ptr<DocumentLifetime> lifetime_;
        std::unique_lock<std::timed_mutex> gate_;
        Document *document_ = nullptr;
    };

    // Brackets one query of a document on the screen thread, so that it never waits
    // for a job's Hold. If a Hold holds the document or waits for it, refused() is
    // true, and the caller returns its empty value without locking anything. The
    // refusal is logged with `method` and counted. Otherwise a Hold that comes later
    // waits until this object is gone. A query made inside another one of the same
    // document on the screen thread is always let through, since the outer one keeps
    // later holds waiting. Does nothing on other threads, so render workers, the find
    // search, OCR and jobs wait for the document as before.
    class ScreenQuery
    {
    public:
        ScreenQuery(DocumentLifetime &lifetime, const char *method);
        ~ScreenQuery();
        ScreenQuery(const ScreenQuery &) = delete;
        ScreenQuery &operator=(const ScreenQuery &) = delete;

        bool refused() const { return refused_; }

    private:
        DocumentLifetime *counted_ = nullptr; // set while counted in screenQueries
        bool entered_ = false;                // let through, outermost or nested
        bool refused_ = false;
    };

    // For tests only.
    struct testing
    {
        // ScreenQuery refusals so far in the process.
        static int refusals();
    };

    // Own the document and, when requested, the independently cloned context.
    // `base` is the context `ctx` was cloned from, kept until `ctx` is dropped (see
    // RenderEngine::baseContext()).
    Document(fz_context *ctx, fz_document *doc, bool ownsContext = false,
             std::shared_ptr<fz_context> base = {});
    // Waits for render, search and OCR work in the document, then closes its file.
    // Both can stall on a drive that stopped answering, so the UI thread uses
    // closeInBackground() instead.
    ~Document();

    // Destroys `document` on a background thread, then runs `then` there. Each
    // close takes its own thread: closing is the user's action, one tab or window
    // at a time. A null document without a `then` starts nothing.
    static void closeInBackground(std::unique_ptr<Document> document,
                                  std::function<void()> then = {});
    // The deleter of OwnedDocument.
    struct CloseInBackground
    {
        void operator()(Document *document) const;
    };
    // Closes closeInBackground() has not finished.
    static int closing();
    // Waits up to `ms` milliseconds for the closes with a `then`, for the end of the
    // process: a `then` removes files the next copy must not find, such as a recovery
    // copy. True when none is left. Closes without one are not waited for.
    static bool waitForCloses(int ms);

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

    // The /Mervin_Measurements blob read from this open document under the access lock,
    // or nullopt when it is absent, larger than kMaxMeasurementBlobBytes, encoded other
    // than Mervin writes it, or unreadable, which includes any MuPDF warning during the
    // read, such as for a bad Flate checksum or missing data. Also nullopt for non-PDFs.
    // Opening reads saved measurements here rather than with qpdf, which inflates whole
    // object streams without a limit while it resolves objects.
    std::optional<QByteArray> mervinMeasurements() const;

    // Whether this document holds a /Mervin_Measurements blob that Mervin could not use,
    // because mervinMeasurements() refused it or its caller could not parse it and called
    // markMeasurementsUnreadable(). Saves then keep the stored blob as it is, and the
    // viewer offers no measuring, so measurements Mervin could not read are never replaced.
    bool measurementsUnreadable() const { return measurementsUnreadable_.load(); }
    void markMeasurementsUnreadable() const { measurementsUnreadable_.store(true); }

    // True when this is a PDF (pdf_specifics succeeds) - i.e. annotations and form
    // fields can be created and written. False for the other formats MuPDF can
    // open (XPS, CBZ, image documents). Read under the access lock.
    bool isPdf() const;

    // Mutate the live PDF only through fn under accessMutex(). Neither the supplied context nor
    // document may escape. Return false for non-PDFs or MuPDF exceptions, true otherwise. Const
    // refers to the wrapper, not PDF contents. On the screen thread it also returns false
    // without running fn while a job holds the document; `refused`, when given, tells that
    // case apart.
    bool withPdfDocument(const std::function<void(fz_context *, pdf_document *)> &fn,
                         bool *refused = nullptr) const;

    // Full MuPDF rewrite of live form/annotation edits to tmpPath, preserving encryption.
    // Caller atomically replaces the destination. Returns false with *error for non-PDFs or
    // write failures.
    bool savePdfTo(const QString &tmpPath, QString *error = nullptr) const;

    // Rasterizes one page on this document's own context under its access lock.
    // A worker borrowing a live document keeps a Hold for this call. A non-empty
    // `clip` renders only those device pixels, relative to the top-left of the
    // page's full (scaled and rotated) raster, as RenderRequest::clip does. Without
    // `antialias`, edges are hard and MuPDF draws every stroke at least one pixel
    // wide, so a black and white threshold acts on colours rather than on partial
    // coverage. The result is the part of `clip` inside the page, as RGB888, or null
    // on failure or when the page's raster reaches beyond kMaxDevicePixel.
    QImage renderPage(int page, double scale, int rotation, const QRect &clip = {},
                      bool antialias = true) const;

    // Serializes all access to handle() across threads (see class note).
    std::mutex &accessMutex() const { return access_; }
    std::shared_ptr<DocumentLifetime> lifetime() const { return lifetime_; }

private:
    std::shared_ptr<DocumentLifetime> lifetime_ = std::make_shared<DocumentLifetime>();
    std::shared_ptr<fz_context> base_;
    fz_context *ctx_ = nullptr;
    bool ownsContext_ = false;
    fz_document *doc_ = nullptr; // owned
    std::vector<QSizeF> pageSizes_;
    QString title_;
    mutable std::mutex access_;
    mutable std::vector<std::optional<PageMeasurement>> measureCache_; // lazy, per page
    mutable std::vector<std::optional<PageGeometry>> geomCache_;       // lazy, per page
    mutable std::optional<bool> formCache_;                            // lazy: has /AcroForm
    mutable std::atomic<bool> measurementsUnreadable_{false};
};

// A document that a background job opened or took over, on its way between threads.
// Wherever it is dropped, it closes on a background thread (Document::closeInBackground),
// so a result that nobody takes, such as one in a post that is dropped, never closes
// on the screen thread. Code that must close the file before it removes or replaces it
// releases the pointer and destroys the document itself.
using OwnedDocument = std::unique_ptr<Document, Document::CloseInBackground>;

} // namespace mervin
