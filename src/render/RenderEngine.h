#pragma once

#include "render/RenderTypes.h"

#include <QObject>

#include <memory>

typedef struct fz_context fz_context;

namespace mervin {

class Document;
class DocumentOpener;

// Owns the worker pool and the base MuPDF context, which lives on while any context
// cloned from it does (see baseContext()). Each worker uses a cloned context; mutex
// callbacks protect shared MuPDF caches. Contexts themselves are not thread-safe.
// The engine's contexts share one MuPDF resource store. ContentSearch has a separate
// store and shares only the locks. Unpatched MuPDF 1.28 reads a store entry
// it has found after it releases the store lock (fz_find_item, and fz_store_item when
// the entry already exists), and makes a new entry findable before it links it.
// Mervin's MuPDF patches close both windows (see "MuPDF patches" in docs/design.md).
// fz_empty_store, fz_filter_store and fz_remove_item free entries whatever their
// reference counts, which is safe only with those patches, so do not call them on the
// shared store. trimCache() uses only calls that cannot free an entry in use.
class RenderEngine : public QObject
{
    Q_OBJECT

public:
    explicit RenderEngine(QObject *parent = nullptr);
    ~RenderEngine() override;

    // Opens a document on its own cloned context. For an encrypted document, `password`
    // is used to authenticate; if a (correct) password is required but not
    // supplied, returns nullptr and sets *needsPassword to true so the caller can
    // prompt and retry. Other failures return nullptr with *error set. The document
    // can outlive the engine. The open reports job::reading() first, so the job it
    // runs in shows the file, and a stopped job opens nothing.
    // Standalone formats, including PDFs, keep the complete file in an owning memory
    // buffer. Viewer reads then use those bytes even if the original drive stops
    // answering. Reading ticks job progress and checks cancellation between chunks.
    // Directory-dependent formats retain their path-based resource lookup.
    std::unique_ptr<Document> openDocument(const QString &path, const QString &password = QString(),
                                           QString *error = nullptr, bool *needsPassword = nullptr);

    // openDocument() for another thread: the context is cloned here, on the thread
    // that owns the engine, and the open can run later on any thread, after the
    // engine is gone too.
    std::shared_ptr<DocumentOpener> opener();
    // Exposes process-wide open counters for worker and unloaded-Save checks.
    struct testing {
        // PDF/document opens attempted by this process, on any thread.
        static int opens();
        // Live queued/active requests and attempted worker rasters for cancellation checks.
        static int pendingRequests(const RenderEngine &engine, quint64 requester);
        static int activeRequests(const RenderEngine &engine, quint64 requester);
        static int rasterizations(const RenderEngine &engine);
        static int cacheTrims(const RenderEngine &engine);
    };

    // Thread-safe. Workers process requests; resultReady() follows on this
    // object's thread.
    // Each request carries the requester's own epoch (echoed back in the result);
    // the engine does not interpret it, so epochs are private to each viewer.
    void submit(const RenderRequest &req);

    void cancelRequests(quint64 requester);

    // Frees the shared MuPDF resources that nothing uses, such as the decoded images,
    // colour links and glyph-cache fonts that closed documents leave behind. This is
    // safe while workers render. fz_shrink_store evicts only store entries that nothing
    // else holds, and a lookup takes its reference before it releases the store lock.
    // fz_purge_glyph_cache is safe because a glyph lookup takes its reference under the
    // glyph cache's own lock. Call it on the engine's thread. It also evicts what
    // loaded documents would reuse, so scheduleCacheTrim coalesces close-time trims.
    void trimCache();
    // Coalesces completed-close notifications into one trim after the render queue
    // and active workers become idle. Call on the engine thread, including when
    // other documents remain open. There is no recurring trim timer.
    void scheduleCacheTrim();

    // Stops the workers without waiting for them (idempotent). A worker stuck reading
    // a document on a drive that stopped answering ends once the read returns.
    void shutdown();

    // Synchronous render for thumbnails and the extract strip on the engine's thread.
    // Renders on the base context under the document's access lock, alongside the
    // worker pool. Returns a null QImage on failure, and on the screen thread while a
    // job holds the document or waits for it (see Document::ScreenQuery). scale maps
    // points->pixels (e.g. dpi/72); rotation is 0/90/180/270.
    QImage renderPageImage(Document *doc, int pageNo, double scale, int rotation);

    // The base MuPDF context. Exposed so render/ helpers (e.g. TextIndex) can
    // clone their own context off it; not for use outside the render subsystem.
    // Whatever holds a clone holds this too and drops its clone first: MuPDF frees
    // the base context before the parts it shares with its clones, so it must be
    // the last of them dropped.
    const std::shared_ptr<fz_context> &baseContext() const { return base_; }

signals:
    void resultReady(const mervin::RenderResult &result);

private:
    friend class DocumentOpener;
    struct Shared; // the queue, shared with the workers, which may outlive the engine

    static std::unique_ptr<Document> open(fz_context *ctx, std::shared_ptr<fz_context> base,
                                          const QString &path, const QString &password,
                                          QString *error, bool *needsPassword);
    // Runs on a background::start task until the engine stops, then drops `ctx`.
    static void workerLoop(const std::shared_ptr<Shared> &shared, fz_context *ctx);
    static void postCacheTrimIfIdle(const std::shared_ptr<Shared> &shared);

    std::shared_ptr<fz_context> base_; // locked with sharedMuPdfLocks()
    std::shared_ptr<Shared> shared_;
};

// Opens one document on a context cloned when RenderEngine::opener() made it.
class DocumentOpener
{
public:
    explicit DocumentOpener(std::shared_ptr<fz_context> base);
    // Drops the context unless open() took it.
    ~DocumentOpener();
    DocumentOpener(const DocumentOpener &) = delete;
    DocumentOpener &operator=(const DocumentOpener &) = delete;

    // RenderEngine::openDocument(), once, on any thread.
    std::unique_ptr<Document> open(const QString &path, const QString &password = QString(),
                                   QString *error = nullptr, bool *needsPassword = nullptr);

private:
    std::shared_ptr<fz_context> base_; // see RenderEngine::baseContext()
    fz_context *ctx_ = nullptr;
};

} // namespace mervin
