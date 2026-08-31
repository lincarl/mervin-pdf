#pragma once

#include "render/RenderTypes.h"

#include <QObject>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

typedef struct fz_context fz_context;

namespace mervin {

class Document;

// Owns MuPDF's base fz_context and a pool of render worker threads, each with
// its own cloned context (the documented MuPDF multi-threading pattern). All
// MuPDF usage in the application is confined to this subsystem.
//
// fz_context is NOT thread-safe; the base context is created with lock
// callbacks backed by std::mutexes, and each worker renders on fz_clone_context().
class RenderEngine : public QObject
{
    Q_OBJECT

public:
    explicit RenderEngine(QObject *parent = nullptr);
    ~RenderEngine() override;

    // Opens a document on the base context. For an encrypted document, `password`
    // is used to authenticate; if a (correct) password is required but not
    // supplied, returns nullptr and sets *needsPassword to true so the caller can
    // prompt and retry. Other failures return nullptr with *error set.
    std::unique_ptr<Document> openDocument(const QString &path, const QString &password = QString(),
                                           QString *error = nullptr, bool *needsPassword = nullptr);

    // Thread-safe. Workers process requests and emit resultReady().
    // Each request carries the requester's own epoch (echoed back in the result);
    // the engine does not interpret it, so epochs are private to each viewer.
    void submit(const RenderRequest &req);

    void shutdown(); // stop and join workers (idempotent)

    // Synchronous, on-the-calling-thread render of a single page (used by Print,
    // which needs page pixmaps at the printer's resolution). Renders on the base
    // context under the document's access lock, so it is safe alongside the
    // worker pool. Returns a null QImage on failure. scale maps points->pixels
    // (e.g. dpi/72); rotation is 0/90/180/270.
    QImage renderPageImage(Document *doc, int pageNo, double scale, int rotation);

    // The base MuPDF context. Exposed so render/ helpers (e.g. TextIndex) can
    // clone their own context off it; not for use outside the render subsystem.
    fz_context *baseContext() const { return base_; }

signals:
    void resultReady(const mervin::RenderResult &result);

private:
    void workerLoop(fz_context *ctx);

    fz_context *base_ = nullptr;
    // Sized to MuPDF's FZ_LOCK_MAX (currently 3); 4 is a safe upper bound
    // (static_assert in the .cpp guards against growth).
    std::array<std::mutex, 4> locks_;

    std::vector<std::thread> workers_;
    std::deque<RenderRequest> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::atomic<bool> stop_{false};
};

} // namespace mervin
