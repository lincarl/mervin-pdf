#pragma once

#include "io/Background.h"
#include "io/JobContext.h"
#include "render/Document.h"
#include "render/FormTypes.h"
#include "render/AnnotTypes.h"
#include "render/TextIndex.h"

#include <QObject>
#include <QHash>
#include <atomic>
#include <functional>
#include <memory>

namespace mervin {

// One page of immutable values, transferred from a worker to its viewer.
struct ViewerPage
{
    int page = -1;
    std::vector<FormField> fields;
    std::vector<Annotation> annotations;
    std::vector<std::pair<QRectF, PdfLinkTarget>> links;
    std::vector<PdfItemProperties> properties;
    PageMeasurement measurement;
    TextIndex::PageText text;
};

// Serializes lazy reads and accepted edits without waiting on the UI thread.
// Only the UI owns caches. Workers own their models and send plain values back.
class ViewerQueries : public QObject
{
public:
    ViewerQueries(Document *document, std::shared_ptr<fz_context> base);
    ~ViewerQueries() override;

    // Cancel reads at page boundaries. Accepted edits survive pause and finish in order.
    void cancelReads();
    void requestPage(int page, std::function<void(ViewerPage)> completed);
    void requestGeometry(int page, std::function<void(PageGeometry)> completed);
    void requestOutline(std::function<void(std::vector<OutlineItem>)> completed);
    void requestAnnotations(int page, std::function<void(std::vector<Annotation>)> completed);
    void requestText(int page, std::function<void(TextIndex::PageText)> completed);
    void requestFields(int page, std::function<void(std::vector<FormField>)> completed);

    // The edit fence keeps the document alive while work runs on the worker.
    // Methods take their own access locks. Then done runs on the UI thread.
    // External Hold users wait for these edits, so saving cannot overtake them.
    void edit(std::function<void(Document &)> work, std::function<void()> done);

    struct testing {
        // Called on the worker after it owns the document, before a page read or edit.
        static void observe(std::function<void(bool edit, int page)> observer);
    };

private:
    void enqueueRead(int key, std::function<void()> work, std::function<void()> done);
    void read(int key, std::function<void(Document &)> work, std::function<void()> done);
    std::shared_ptr<DocumentLifetime> lifetime_;
    std::shared_ptr<fz_context> base_;
    std::shared_ptr<background::SerialQueue> queue_ = std::make_shared<background::SerialQueue>();
    std::shared_ptr<job::State> reads_ = std::make_shared<job::State>();
    std::shared_ptr<job::State> textReads_ = std::make_shared<job::State>();
    QHash<int, background::SerialQueue::JobId> pending_;
};
}
