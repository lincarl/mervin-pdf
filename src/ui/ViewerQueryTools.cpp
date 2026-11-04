#include "ui/ViewerWidget.h"
#include "render/AnnotModel.h"
#include "render/FormModel.h"

#include <QMouseEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QListWidget>
#include <QTimer>
#include <algorithm>

namespace mervin {

void ViewerWidget::requestPageData(int page) const
{
    if (!queries_ || queryPaused_ || page < 0 || page >= pageCount()
        || requestedPages_.contains(page))
        return;
    if (queryPages_.contains(page)) {
        if ((!formModel_ || formModel_->pageReady(page))
            && (!annotModel_ || annotModel_->pageReady(page))
            && (!textIndex_ || textIndex_->pageReady(page))) return;
        queryPages_.remove(page);
        std::erase(queryPageOrder_, page);
    }
    // Match the queue's backpressure, leaving paint/input free to retry later.
    if (requestedPages_.size() >= 12)
        return;
    requestedPages_.insert(page);
    const QPointer<ViewerWidget> self(const_cast<ViewerWidget *>(this));
    const quint64 formRevision = formModel_ ? formModel_->revision() : 0;
    const quint64 annotRevision = annotModel_ ? annotModel_->revision() : 0;
    const quint64 generation = queryRevision_;
    queries_->requestPage(page, [self, generation, formRevision, annotRevision](ViewerPage result) {
        const auto current = [&] { return self && self->queryRevision_ == generation; };
        if (!current()) return;
        const int page = result.page;
        self->requestedPages_.remove(page);
        if ((self->formModel_ && self->formModel_->revision() != formRevision)
            || (self->annotModel_ && self->annotModel_->revision() != annotRevision)) {
            self->requestPageData(page);
            return;
        }
        if (self->formModel_) self->formModel_->storePage(page, std::move(result.fields));
        if (self->annotModel_) self->annotModel_->storePage(page, std::move(result.annotations));
        if (self->textIndex_) self->textIndex_->storePage(page, std::move(result.text));
        self->queryPages_.insert(page, std::make_shared<ViewerPage>(std::move(result)));
        self->queryPageOrder_.push_back(page);
        // Keep a small working set. Explicit sidebar scans retain only their output,
        // never complete text/geometry snapshots for the whole document. Pin visible
        // pages so a very small zoom cannot cause repeated eviction and reparsing.
        const auto visiblePages = self->layout_.pagesInViewport(
            QRect(self->contentOffset(), self->viewport()->size()));
        while (self->queryPageOrder_.size() > 24) {
            const auto victim = std::find_if(self->queryPageOrder_.begin(), self->queryPageOrder_.end(),
                [&](int old) { return old != page && std::find(visiblePages.begin(), visiblePages.end(), old)
                    == visiblePages.end(); });
            if (victim == self->queryPageOrder_.end()) break;
            const int old = *victim;
            self->queryPageOrder_.erase(victim);
            self->queryPages_.remove(old);
            if (self->formModel_) self->formModel_->discardPage(old);
            if (self->annotModel_) self->annotModel_->discardPage(old);
            if (self->textIndex_) self->textIndex_->discardPage(old);
        }
        if (self->pendingReveal_ && self->pendingReveal_->first == page) {
            const int id = self->pendingReveal_->second;
            self->pendingReveal_.reset();
            self->revealAnnotation(page, id);
            if (!current()) return;
        }
        self->syncFormEditors();
        if (!current()) return;
        if (self->pendingFormFocusIndex_ >= 0)
            self->focusFormFieldAt(self->pendingFormFocusIndex_);
        if (!current()) return;
        if (self->measureToolEnabled_ && page == self->currentPage_) {
            const ToolMode previousMode = self->toolMode_;
            self->maybeAutoCalibrate();
            if (!current()) return;
            if (previousMode == ToolMode::Measure && self->toolMode_ == ToolMode::Calibrate)
                for (auto &input : self->deferredMouse_)
                    if (input.mode == ToolMode::Measure) input.mode = ToolMode::Calibrate;
            const QSizeF size = self->doc_->pageSize(page);
            self->updateHoverScale(page, QPointF(size.width() / 2, size.height() / 2));
            if (!current()) return;
            self->emitMeasurementsChanged();
            if (!current()) return;
        }
        if (self->pendingMatchScroll_ >= 0 && self->pendingMatchScroll_ == self->currentMatch_
            && self->matches_[self->pendingMatchScroll_].page == page)
            self->scrollToMatch(std::exchange(self->pendingMatchScroll_, -1));
        self->replayQueryInput();
        if (!current()) return;
        self->viewport()->update();
        emit self->queryDataChanged();
    });
}

void ViewerWidget::readSelectionText(TextPos start, TextPos end, std::function<void(QString)> completed) const
{
    if (!queries_ || !start.valid() || !end.valid()) return;
    struct State {
        int page;
        TextPos start, end;
        QString text;
        std::function<void(QString)> completed;
    };
    auto state = std::make_shared<State>(State{start.page, start, end, {}, std::move(completed)});
    auto next = std::make_shared<std::function<void()>>();
    const QPointer<ViewerQueries> queries(queries_.get());
    *next = [queries, state, weak = std::weak_ptr(next)] {
        if (!queries) return;
        auto continuation = weak.lock();
        if (!continuation) return;
        queries->requestText(state->page, [state, continuation](TextIndex::PageText page) {
            const int begin = state->page == state->start.page ? state->start.offset : 0;
            const int end = state->page == state->end.page
                ? std::min(state->end.offset, int(page.text.size())) : int(page.text.size());
            if (end > begin) state->text += page.text.mid(begin, end - begin);
            if (state->page++ == state->end.page) {
                state->completed(std::move(state->text));
            } else {
                state->text += QLatin1Char('\n');
                (*continuation)();
            }
        });
    };
    (*next)();
}

void ViewerWidget::cancelQueryReads()
{
    if (queries_) queries_->cancelReads();
    requestedPages_.clear();
    if (!annotationsComplete_) annotationsRequested_ = false;
    if (!formFieldOrderBuilt_) formOrderRequested_ = false;
    ++formOrderRevision_;
    pendingFormFocus_ = 0;
    pendingFormFocusIndex_ = -1;
    // An unfinished outline is retried when the sidebar next requests it.
    if (!outlineReady_) outlineRequested_ = false;
    ++annotationsRevision_;
}

const std::vector<OutlineItem> &ViewerWidget::outline() const
{
    if (queries_ && !outlineRequested_ && !queryPaused_) {
        outlineRequested_ = true;
        const QPointer<ViewerWidget> self(const_cast<ViewerWidget *>(this));
        queries_->requestOutline([self](std::vector<OutlineItem> outline) {
            self->outline_ = std::move(outline);
            self->outlineReady_ = true;
            emit self->outlineChanged();
        });
    }
    return outline_;
}

void ViewerWidget::stopAnnotationListing()
{
    if (annotationsComplete_) return;
    ++annotationsRevision_;
    annotationsRequested_ = false;
}

void ViewerWidget::collectAnnotations(int page) const
{
    if (!queries_ || queryPaused_) return;
    const QPointer<ViewerWidget> self(const_cast<ViewerWidget *>(this));
    if (page == 0) { allAnnotations_.clear(); annotationsPublished_ = 0; }
    if (page >= pageCount()) {
        annotationsComplete_ = true;
        emit self->annotationsChanged();
        return;
    }
    const quint64 revision = annotationsRevision_;
    queries_->requestAnnotations(page, [self, page, revision](std::vector<Annotation> annotations) {
        if (revision != self->annotationsRevision_) {
            // Reopening can race the one read already running for the old listing.
            // Its key is available now, so retry the new listing instead of losing it.
            if (self->annotationsRequested_ && !self->annotationsComplete_)
                self->collectAnnotations(0);
            return;
        }
        self->allAnnotations_.insert(self->allAnnotations_.end(), annotations.begin(), annotations.end());
        if (self->allAnnotations_.size() > self->annotationsPublished_
            && self->allAnnotations_.size() >= self->annotationsPublished_ * 2) {
            self->annotationsPublished_ = self->allAnnotations_.size();
            emit self->annotationsChanged();
        }
        if (self && revision == self->annotationsRevision_) self->collectAnnotations(page + 1);
    });
}

void ViewerWidget::collectFormOrder(int page)
{
    if (!queries_ || queryPaused_ || toolMode_ != ToolMode::FillForms) return;
    if (page >= pageCount()) {
        formFieldOrderBuilt_ = true;
        const auto focused = std::find(formFieldOrder_.begin(), formFieldOrder_.end(), formFocusField_);
        if (focused != formFieldOrder_.end())
            formFocusIndex_ = int(focused - formFieldOrder_.begin());
        const int advance = std::exchange(pendingFormFocus_, 0);
        if (advance) advanceFormFocus(advance);
        return;
    }
    const quint64 revision = formOrderRevision_;
    queries_->requestFields(page, [this, page, revision](std::vector<FormField> fields) {
        if (revision != formOrderRevision_) {
            if (formOrderRequested_ && !formFieldOrderBuilt_) collectFormOrder(0);
            return;
        }
        for (int field = 0; field < int(fields.size()); ++field)
            if (fields[field].editable()) formFieldOrder_.emplace_back(page, field);
        collectFormOrder(page + 1);
    });
}

bool ViewerWidget::deferMouseQuery(QMouseEvent *event)
{
    if (replayingMouse_ || !queries_ || queryPaused_ || panning_ || toolMode_ == ToolMode::Ocr
        || event->button() == Qt::MiddleButton)
        return false;
    const QPoint canvas = event->pos() + contentOffset();
    const int page = pageAtCanvas(canvas);
    // OCR and unsnapped manual calibration use only the existing layout.
    if (measureMode() && !measureSnap_
        && (toolMode_ == ToolMode::Calibrate || measureOverrides_.hasOverride(page)))
        return false;
    const bool needsGeometry = measureMode() && measureSnap_ && page >= 0;
    if (page >= 0) {
        requestPageData(page);
        if (needsGeometry) ensureMeasureGeometry(page);
    }
    const bool ready = page < 0 || (queryPages_.contains(page)
        && (!needsGeometry || measureGeoPage_ == page));
    if (ready && deferredMouse_.empty()) return false;
    // Hovering only requests data. A press owns a gesture, retained until its
    // snapshot arrives, including a release that happened while parsing was busy.
    if (event->type() == QEvent::MouseMove && deferredMouse_.empty()) return true;
    DeferredMouse input{event->type(), toolMode_, page, page >= 0 ? canvasToPagePoint(page, canvas)
        : event->position(), event->button(), event->buttons(), event->modifiers()};
    if (input.type == QEvent::MouseMove && !deferredMouse_.empty()
        && deferredMouse_.back().type == QEvent::MouseMove)
        deferredMouse_.back() = input;
    else
        deferredMouse_.push_back(input);
    event->accept();
    return true;
}

void ViewerWidget::replayQueryInput()
{
    const QPointer<ViewerWidget> self(this);
    const quint64 generation = queryRevision_;
    while (!deferredMouse_.empty() && !queryPaused_) {
        const auto input = deferredMouse_.front();
        if (input.mode != toolMode_) {
            deferredMouse_.clear();
            replayFormTarget_.clear();
            return;
        }
        if (input.page >= 0 && (!queryPages_.contains(input.page)
            || (measureMode() && measureSnap_ && measureGeoPage_ != input.page))) {
            requestPageData(input.page);
            if (measureMode() && measureSnap_) ensureMeasureGeometry(input.page);
            return;
        }
        deferredMouse_.pop_front();
        const QPointF position = input.page >= 0
            ? pageRectToWidget(input.page, QRectF(input.point, QSizeF())).topLeft() : input.point;
        if (input.type == QEvent::MouseButtonPress && toolMode_ == ToolMode::FillForms) {
            for (const FormEditor &editor : formEditors_)
                if (editor.widget && editor.widget->geometry().contains(position.toPoint())) {
                    replayFormTarget_ = editor.widget;
                    editor.widget->setFocus(Qt::MouseFocusReason);
                    break;
                }
        }
        if (replayFormTarget_) {
            QMouseEvent editorEvent(input.type, replayFormTarget_->mapFrom(viewport(), position),
                viewport()->mapToGlobal(position.toPoint()), input.button, input.buttons, input.modifiers);
            QCoreApplication::sendEvent(replayFormTarget_, &editorEvent);
            if (!self || queryRevision_ != generation) return;
            if (input.type == QEvent::MouseButtonRelease) replayFormTarget_.clear();
            continue;
        }
        QMouseEvent event(input.type, position, viewport()->mapToGlobal(position.toPoint()),
                          input.button, input.buttons, input.modifiers);
        replayingMouse_ = true;
        if (input.type == QEvent::MouseButtonPress) mousePressEvent(&event);
        else if (input.type == QEvent::MouseButtonRelease) mouseReleaseEvent(&event);
        else if (input.type == QEvent::MouseButtonDblClick) mouseDoubleClickEvent(&event);
        else mouseMoveEvent(&event);
        if (!self || queryRevision_ != generation) return;
        replayingMouse_ = false;
    }
}

void ViewerWidget::editFormValue(int page, int field, const QString &value, bool choice,
                                 int choiceIndex)
{
    if (!formModel_ || !queries_) return;
    auto fields = formModel_->pageFields(page);
    if (field < 0 || field >= int(fields.size())) return;
    if (fields[field].value == value && (!choice || fields[field].choiceIndex == choiceIndex)) return;
    fields[field].value = value;
    if (choice) fields[field].choiceIndex = choiceIndex;
    formModel_->storePage(page, std::move(fields));
    for (const FormEditor &editor : formEditors_)
        if (editor.page == page && editor.fieldIndex == field && editor.widget) {
            editor.widget->setProperty("mervinFormValue", value);
            if (choice) editor.widget->setProperty("mervinFormChoiceIndex", choiceIndex);
        }
    submitFormEdit(page, field, value, choice, choiceIndex);
}

void ViewerWidget::editFormToggle(int page, int field)
{
    if (formModel_ && queries_) submitFormEdit(page, field, std::nullopt, false);
}

void ViewerWidget::submitFormEdit(int page, int field, std::optional<QString> value, bool choice,
                                  int choiceIndex)
{
    struct Result {
        std::vector<std::pair<int, int>> affected;
        QHash<int, std::vector<FormField>> pages;
    };
    auto result = std::make_shared<Result>();
    auto pages = queryPages_.keys();
    if (!pages.contains(page)) pages.append(page);
    cancelQueryReads();
    ++pendingFormEdits_;
    formModel_->markDirty();
    queries_->edit([result, pages, page, field, value, choice, choiceIndex](Document &document) {
        FormModel model(document);
        if (!value) model.toggle(page, field);
        else if (choice) model.setChoiceValue(page, field, *value, choiceIndex);
        else model.setTextValue(page, field, *value);
        result->affected = model.changedFields();
        for (int p : pages) result->pages.insert(p, model.pageFields(p));
    }, [this, result, page] {
        for (const auto &[p, field] : result->affected) {
            editedFormPages_.insert(p);
            Q_UNUSED(field);
        }
        if (--pendingFormEdits_ != 0) return;
        std::vector<std::pair<int, int>> affected;
        for (auto it = result->pages.begin(); it != result->pages.end(); ++it) {
            for (int field = 0; field < int(it->size()); ++field)
                affected.emplace_back(it.key(), field);
            formModel_->storePage(it.key(), std::move(it.value()));
        }
        for (int p : editedFormPages_) {
            if (!result->pages.contains(p)) formModel_->discardPage(p);
            applyAnnotChange(p); // the common page-raster invalidation, including failure recovery
        }
        editedFormPages_.clear();
        formModel_->setChangedFields(std::move(affected));
        applyFormFieldChange(page);
        viewport()->update();
        emit formEditsChanged();
    });
    emit formEditsChanged(); // accepted edits count before Save/Close inspects dirty state
}

void ViewerWidget::submitAnnotEdit(int page, std::function<int(AnnotModel &)> edit,
                                    std::function<void(int)> completed)
{
    if (!queries_ || !annotModel_) return;
    struct Result { int id = -1; std::vector<Annotation> annotations; };
    auto result = std::make_shared<Result>();
    cancelQueryReads();
    annotModel_->markDirty();
    queries_->edit([result, page, edit](Document &document) {
        AnnotModel model(document);
        result->id = edit(model);
        result->annotations = model.pageAnnots(page);
    }, [this, page, result, completed] {
        annotModel_->storePage(page, std::move(result->annotations));
        annotationsRequested_ = annotationsComplete_ = false;
        ++annotationsRevision_;
        applyAnnotChange(page);
        const QPointer<ViewerWidget> self(this);
        const quint64 generation = queryRevision_;
        emit annotEditsChanged();
        if (!self || queryRevision_ != generation) return;
        emit annotationsChanged();
        if (self && queryRevision_ == generation && completed) completed(result->id);
    });
    emit annotEditsChanged();
}
}
