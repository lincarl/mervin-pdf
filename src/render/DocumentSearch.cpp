#include "render/DocumentSearch.h"
#include "render/Document.h"
#include "render/RenderEngine.h"

#include <QPointer>
#include <mutex>

namespace mervin {
namespace {
std::mutex observerMutex;
std::function<void(DocumentSearch::testing::Phase, const QString &)> searchObserver;
}

void DocumentSearch::testing::observe(std::function<void(Phase, const QString &)> observer)
{
    std::lock_guard lock(observerMutex);
    searchObserver = std::move(observer);
}

DocumentSearch::DocumentSearch(RenderEngine *engine) : engine_(engine)
{
}

DocumentSearch::~DocumentSearch()
{
    cancel();
}

void DocumentSearch::cancel()
{
    ++*generation_;
    searches_.clear();
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
    const quint64 generation = generation_->load();
    std::function<void(testing::Phase, const QString &)> observer;
    {
        std::lock_guard lock(observerMutex);
        observer = searchObserver;
    }
    searches_.add([owner = QPointer<QObject>(this), generations = generation_, index = index_,
                   query, matchCase, wholeWord, generation, observer, completed = std::move(completed)] {
        if (observer)
            observer(testing::Phase::Started, query);
        const auto canceled = [&generations, generation] { return generations->load() != generation; };
        auto result = index->search(query, matchCase, wholeWord, canceled);
        background::post(owner, [generations, generation, result = std::move(result), completed] {
            if (generations->load() == generation)
                completed(result);
        });
        if (observer)
            observer(testing::Phase::CompletionQueued, query);
    });
}
}
