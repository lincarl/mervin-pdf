#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>

namespace mervin {

class RenderEngine;

// On-demand, no-index content search across a list of files (the recent-files
// history, most-recent first). Extraction uses MuPDF, so this lives in the
// render subsystem and its header exposes no fz_* types: it clones the engine's
// base context onto a private worker thread, opens each file in turn, extracts
// page text, and streams a hit for the first page of each file that contains
// the query. Results arrive incrementally so the user can open one before the
// search completes; a new start() or cancel() stops the in-flight scan.
//
// Matching is case-insensitive substring (the recent-files "search contents"
// affordance), independent of the in-page find bar's case/whole-word toggles.
class ContentSearch : public QObject
{
    Q_OBJECT

public:
    explicit ContentSearch(RenderEngine *engine, QObject *parent = nullptr);
    ~ContentSearch() override; // cancels and joins the worker

    // Begin scanning `paths` for `query`. Cancels any running search first.
    // A blank query or empty path list is a no-op that emits finished().
    void start(const QStringList &paths, const QString &query);

    // Request the running scan to stop. Non-blocking; finished(canceled=true)
    // follows once the worker notices.
    void cancel();

    bool isRunning() const { return running_.load(); }

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
    void stopWorker();                                 // signal cancel + join
    void run(QStringList paths, QString query, quint64 generation);

    RenderEngine *engine_ = nullptr;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    // Bumped on every start()/cancel(); a worker whose generation is stale exits
    // without emitting (guards against a just-canceled run racing the next one).
    std::atomic<quint64> generation_{0};
};

} // namespace mervin
