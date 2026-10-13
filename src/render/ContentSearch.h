#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

typedef struct fz_context fz_context;

namespace mervin {

// Search recent files on a private worker and MuPDF context of its own, streaming the first
// matching page per file. start()/cancel() supersede pending work. Matching is a
// case-insensitive substring, independent of in-page find options. The worker is a
// background::start task that nothing waits for, so a file on a share that stopped answering
// cannot hold up the window, and it shares nothing with the render engine.
class ContentSearch : public QObject
{
    Q_OBJECT

public:
    explicit ContentSearch(QObject *parent = nullptr);
    // Cancels the search and tells the worker to stop, without waiting for it. A
    // worker stuck in a file stops once the file answers.
    ~ContentSearch() override;

    // Begin scanning `paths` for `query`. Cancels any running search first.
    // A blank query or empty path list is a no-op that emits finished().
    void start(const QStringList &paths, const QString &query);

    // Invalidate pending deliveries and request cancellation without joining the worker.
    void cancel();

    bool isRunning() const { return running_; }

signals:
    // The first page (1-based) of `path` that contains the query, together with
    // a short one-line snippet of surrounding text (the match included) so the
    // result row can preview where the term was found.
    void hit(const QString &path, int page, const QString &snippet);
    // Emitted periodically so the UI can show "scanned / total".
    void progress(int scanned, int total);
    // canceled is true if the scan was stopped early; matched is the hit count.
    void finished(bool canceled, int matched);

private:
    struct Shared; // the request queue, shared with the worker, which may outlive this
    static void workerLoop(const std::shared_ptr<Shared> &shared);
    static void run(const std::shared_ptr<Shared> &shared, const QStringList &paths,
                    const QString &query, quint64 generation, fz_context *ctx);

    std::shared_ptr<Shared> shared_;
    bool running_ = false;
};

} // namespace mervin
