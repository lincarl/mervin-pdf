#pragma once

#include "io/Background.h"
#include "render/TextIndex.h"
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>

namespace mervin {
class RenderEngine;
struct DocumentLifetime;

// One serial worker per viewer; replacing a query never waits for extraction.
class DocumentSearch : public QObject
{
public:
    explicit DocumentSearch(RenderEngine *engine);
    // Cancels the search without waiting for it. One stuck reading a page from a
    // drive that stopped answering ends once the read returns.
    ~DocumentSearch() override;
    void start(Document *document, const QString &query, bool matchCase, bool wholeWord,
               std::function<void(std::vector<TextMatch>)> completed);
    void cancel();
    // Cancel extraction and release its retained text index when the document unloads.
    void clear();

    struct testing {
        enum class Phase { Started, CompletionQueued };
        // Observes worker phases. Copied when start() queues a search.
        static void observe(std::function<void(Phase, const QString &)> observer);
    };

private:
    RenderEngine *engine_;
    background::SerialQueue searches_;
    // Shared with the searches, which may outlive this.
    std::shared_ptr<std::atomic<quint64>> generation_ = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<DocumentLifetime> document_;
    std::shared_ptr<TextIndex> index_;
};
}
