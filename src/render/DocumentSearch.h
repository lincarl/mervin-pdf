#pragma once

#include "render/TextIndex.h"
#include <QObject>
#include <QThreadPool>
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
    ~DocumentSearch() override;
    void start(Document *document, const QString &query, bool matchCase, bool wholeWord,
               std::function<void(std::vector<TextMatch>)> completed);
    void cancel();
    // Cancel extraction and release its retained text index when the document unloads.
    void clear();

private:
    RenderEngine *engine_;
    QThreadPool pool_;
    std::atomic<quint64> generation_{0};
    std::shared_ptr<DocumentLifetime> document_;
    std::shared_ptr<TextIndex> index_;
};
}
