#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <optional>

typedef struct fz_context fz_context;

namespace mervin {

class RenderEngine;

// Search recent files on a private worker/context, streaming the first matching page per file.
// start()/cancel() supersede pending work. Matching is a case-insensitive substring,
// independent of in-page find options.
class ContentSearch : public QObject
{
    Q_OBJECT

public:
    explicit ContentSearch(RenderEngine *engine, QObject *parent = nullptr);
    ~ContentSearch() override; // cancels and joins the worker

    // Begin scanning `paths` for `query`. Cancels any running search first.
    // A blank query or empty path list is a no-op that emits finished().
    void start(const QStringList &paths, const QString &query);

    // Invalidate pending deliveries and request cancellation without joining the worker.
    void cancel();

    bool isRunning() const { return running_.load(); }

signals:
    // The first page (1-based) of `path` that contains the query, together with
    // a short one-line snippet of surrounding text (the match included) so the
    // result row can preview where the term was found.
    void hit(const QString &path, int page, const QString &snippet);
    // Emitted periodically so the UI can show "scanned / total".
    void progress(int scanned, int total);
    // Within the file being read (the one after the `scanned` of the last
    // progress()): `page` of `pageCount` pages done. At most about ten times a
    // second, so a large file still shows movement.
    void pageProgress(int page, int pageCount);
    // canceled is true if the scan was stopped early; matched is the hit count.
    void finished(bool canceled, int matched);

private:
    struct Request { QStringList paths; QString query; quint64 generation; };
    void workerLoop(fz_context *ctx);
    void run(QStringList paths, QString query, quint64 generation, fz_context *ctx);

    RenderEngine *engine_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    bool stopping_ = false;
    std::atomic<bool> running_{false};
    // Bumped on every start()/cancel(); a worker whose generation is stale exits
    // without emitting (guards against a just-canceled run racing the next one).
    std::atomic<quint64> generation_{0};
};

} // namespace mervin
