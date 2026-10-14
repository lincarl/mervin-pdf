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
    std::unique_ptr<Document> openDocument(const QString &path, const QString &password = QString(),
                                           QString *error = nullptr, bool *needsPassword = nullptr);

    // openDocument() for another thread: the context is cloned here, on the thread
    // that owns the engine, and the open can run later on any thread, after the
    // engine is gone too.
    std::shared_ptr<DocumentOpener> opener();

    // Thread-safe. Workers process requests; resultReady() follows on this
    // object's thread.
    // Each request carries the requester's own epoch (echoed back in the result);
    // the engine does not interpret it, so epochs are private to each viewer.
    void submit(const RenderRequest &req);

    void cancelRequests(quint64 requester);

    // Release unused shared MuPDF resources after document contents have unloaded.
    void trimCache();

    // Stops the workers without waiting for them (idempotent). A worker stuck reading
    // a document on a drive that stopped answering ends once the read returns.
    void shutdown();

    // Synchronous, on-the-calling-thread render of a single page (used by Print,
    // which needs page pixmaps at the printer's resolution). Renders on the base
    // context under the document's access lock, so it is safe alongside the
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
