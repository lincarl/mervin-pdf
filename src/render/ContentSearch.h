#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

typedef struct fz_context fz_context;

namespace mervin {

// Search recent files with a MuPDF context of its own, streaming the first matching page per
// file. start()/cancel() supersede earlier work. Matching is a case-insensitive substring,
// independent of in-page find options. Searches run on one background::start task at a time,
// which takes the newest query waiting and ends when none is. Nothing waits for it, so a file on
// a share that stopped answering cannot hold up the window. A task stuck in such a file keeps
// only the newest query waiting and starts it once the file answers, so stuck searches never
// collect threads. It shares nothing with the render engine.
class ContentSearch : public QObject
{
    Q_OBJECT

public:
    explicit ContentSearch(QObject *parent = nullptr);
    // Cancels the search without waiting for it. One stuck in a file stops once the
    // file answers.
    ~ContentSearch() override;

    // Begin scanning `paths` for `query`. Cancels any running search first.
    // A blank query or empty path list is a no-op that emits finished().
    void start(const QStringList &paths, const QString &query);

    // Invalidate pending deliveries and stop the search without waiting for it.
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
    struct Shared; // the queue, shared with the task, which may outlive this
    // Runs the waiting queries, newest only, until none is left.
    static void drain(const std::shared_ptr<Shared> &shared);
    static void run(const std::shared_ptr<Shared> &shared, const QStringList &paths,
                    const QString &query, quint64 generation, fz_context *ctx);

    std::shared_ptr<Shared> shared_;
    bool running_ = false;
};

} // namespace mervin
