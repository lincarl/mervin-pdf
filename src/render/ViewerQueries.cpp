#include "render/ViewerQueries.h"
#include "render/AnnotModel.h"
#include "render/FormModel.h"

#include <mutex>

namespace mervin {
namespace {
std::mutex observerMutex;
std::function<void(bool, int)> queryObserver;

// Snapshot test hooks so a blocked observer never owns the registration lock.
void observeQuery(bool edit, int page)
{
    std::function<void(bool, int)> observer;
    {
        std::lock_guard lock(observerMutex);
        observer = queryObserver;
    }
    if (observer)
        observer(edit, page);
}

// The fence is acquired on the UI before queueing, and released on every exit.
struct PendingEdit
{
    std::shared_ptr<DocumentLifetime> lifetime;
    explicit PendingEdit(std::shared_ptr<DocumentLifetime> value) : lifetime(std::move(value))
    { ++lifetime->pendingEdits; }
    ~PendingEdit() { --lifetime->pendingEdits; }
};
}

void ViewerQueries::testing::observe(std::function<void(bool, int)> observer)
{
    std::lock_guard lock(observerMutex);
    queryObserver = std::move(observer);
}

ViewerQueries::ViewerQueries(Document *document, std::shared_ptr<fz_context> base)
    : lifetime_(document->lifetime()), base_(std::move(base))
{
}

ViewerQueries::~ViewerQueries()
{
    textReads_->stop();
    cancelReads();
}

void ViewerQueries::cancelReads()
{
    reads_->stop();
    reads_ = std::make_shared<job::State>();
    for (auto id : pending_) queue_->remove(id);
    pending_.clear();
}

void ViewerQueries::enqueueRead(int key, std::function<void()> work, std::function<void()> done)
{
    // Backpressure prevents fast scrolling from accumulating arbitrary page jobs.
    if (pending_.contains(key) || pending_.size() >= 16)
        return;
    const auto id = queue_->add([owner = QPointer<ViewerQueries>(this), state = reads_,
                  key, work = std::move(work), done = std::move(done)] {
        const job::Scope scope(*state);
        if (state->stopped()) return;
        work();
        background::post(owner, [owner, state, key, done] {
            if (state->stopped())
                return;
            owner->pending_.remove(key);
            done();
        });
    });
    pending_.insert(key, id);
}

void ViewerQueries::read(int key, std::function<void(Document &)> work, std::function<void()> done)
{
    enqueueRead(key, [lifetime = lifetime_, key, work = std::move(work)] {
        Document::Hold hold(lifetime, [] { return job::stopped(); }, false);
        if (!hold.document() || job::stopped()) return;
        observeQuery(false, key);
        if (!job::stopped()) work(*hold.document());
    }, std::move(done));
}

void ViewerQueries::requestPage(int page, std::function<void(ViewerPage)> completed)
{
    // TextIndex acquires its own lifetime gate. End the metadata Hold before
    // extracting text, while retaining one queue identity for cancellation.
    auto result = std::make_shared<ViewerPage>();
    enqueueRead(page, [result, page, base = base_, lifetime = lifetime_] {
        std::unique_ptr<TextIndex> index;
        {
            Document::Hold hold(lifetime, [] { return job::stopped(); }, false);
            if (!hold.document() || job::stopped()) return;
            observeQuery(false, page);
            if (job::stopped()) return;
            auto &doc = *hold.document();
            result->page = page;
            FormModel forms(doc);
            AnnotModel annotations(doc);
            result->fields = forms.pageFields(page);
            if (job::stopped()) return;
            result->annotations = annotations.pageAnnots(page);
            if (job::stopped()) return;
            result->links = doc.pageLinks(page);
            result->properties = doc.pageItemProperties(page);
            result->measurement = doc.pageMeasurement(page);
            index = std::make_unique<TextIndex>(base, &doc);
        }
        if (!job::stopped()) result->text = index->snapshot(page);
    }, [result, completed = std::move(completed)] {
        if (result->page >= 0) completed(std::move(*result));
    });
}

void ViewerQueries::requestGeometry(int page, std::function<void(PageGeometry)> completed)
{
    auto result = std::make_shared<PageGeometry>();
    read(-1, [page, result](Document &doc) { *result = doc.pageGeometry(page); },
         [result, completed] { completed(std::move(*result)); });
}

void ViewerQueries::requestOutline(std::function<void(std::vector<OutlineItem>)> completed)
{
    auto result = std::make_shared<std::vector<OutlineItem>>();
    read(-2, [result](Document &doc) { *result = doc.outline(); },
         [result, completed] { completed(std::move(*result)); });
}

void ViewerQueries::requestAnnotations(int page, std::function<void(std::vector<Annotation>)> completed)
{
    auto result = std::make_shared<std::vector<Annotation>>();
    read(-3, [page, result](Document &doc) { *result = AnnotModel(doc).pageAnnots(page); },
         [result, completed] { completed(std::move(*result)); });
}

void ViewerQueries::requestFields(int page, std::function<void(std::vector<FormField>)> completed)
{
    auto result = std::make_shared<std::vector<FormField>>();
    read(-4, [page, result](Document &doc) { *result = FormModel(doc).pageFields(page); },
         [result, completed] { completed(std::move(*result)); });
}

void ViewerQueries::requestText(int page, std::function<void(TextIndex::PageText)> completed)
{
    queue_->add([owner = QPointer<ViewerQueries>(this), lifetime = lifetime_, state = textReads_,
                  base = base_, page, completed = std::move(completed)] {
        const job::Scope scope(*state);
        std::unique_ptr<TextIndex> index;
        {
            Document::Hold hold(lifetime, [state] { return state->stopped(); }, false);
            if (!hold.document()) return;
            observeQuery(false, -5);
            if (state->stopped()) return;
            index = std::make_unique<TextIndex>(base, hold.document());
        }
        auto result = index->snapshot(page);
        background::post(owner, [state, completed, result = std::move(result)]() mutable {
            if (!state->stopped()) completed(std::move(result));
        });
    });
}

void ViewerQueries::edit(std::function<void(Document &)> work, std::function<void()> done)
{
    auto fence = std::make_shared<PendingEdit>(lifetime_);
    queue_->add([owner = QPointer<ViewerQueries>(this), lifetime = lifetime_,
                  queue = queue_, fence, work = std::move(work), done = std::move(done)]() mutable {
        Document *document = nullptr;
        {
            Document::Hold hold(lifetime, {}, false);
            document = hold.document();
        }
        // PendingEdit keeps destruction waiting until work finishes. Release the
        // gate here so text extraction can acquire its own Hold without nesting.
        if (document) {
            observeQuery(true, -1);
            work(*document);
        }
        fence.reset();
        background::post(owner, std::move(done));
    });
}
}
