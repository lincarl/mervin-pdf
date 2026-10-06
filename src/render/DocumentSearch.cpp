#include "render/DocumentSearch.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

namespace mervin {
DocumentSearch::DocumentSearch(RenderEngine *engine) : engine_(engine)
{
    pool_.setMaxThreadCount(1);
}

DocumentSearch::~DocumentSearch()
{
    cancel();
    pool_.waitForDone();
}

void DocumentSearch::cancel()
{
    ++generation_;
    pool_.clear();
}

void DocumentSearch::clear()
{
    cancel();
    index_.reset();
    document_.reset();
}

void DocumentSearch::start(Document *document, const QString &query, bool matchCase,
                           bool wholeWord, std::function<void(std::vector<TextMatch>)> completed)
{
    cancel();
    if (!document || query.isEmpty())
        return;
    if (document_ != document->lifetime()) {
        document_ = document->lifetime();
        index_ = std::make_shared<TextIndex>(engine_->baseContext(), document);
    }
    const quint64 generation = generation_.load();
    pool_.start([this, index = index_, query, matchCase, wholeWord, generation,
                 completed = std::move(completed)] {
        auto result = index->search(query, matchCase, wholeWord,
                                    [this, generation] { return generation_.load() != generation; });
        QMetaObject::invokeMethod(this, [this, generation, result = std::move(result), completed] {
            if (generation_.load() == generation)
                completed(result);
        }, Qt::QueuedConnection);
    });
}
}
