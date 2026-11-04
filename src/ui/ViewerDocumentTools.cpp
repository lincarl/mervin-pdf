#include "ui/ViewerWidget.h"

#include "render/AnnotModel.h"
#include "render/ComfortTransform.h"
#include "render/Document.h"
#include "render/DocumentSearch.h"
#include "render/FormModel.h"
#include "render/MeasureContent.h"
#include "render/RenderEngine.h"
#include "ui/AnnotPopup.h"
#include "ui/Icons.h"
#include "ui/PdfPropertiesPopup.h"
#include "ui/ThemeTokens.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPointer>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace mervin {
namespace {
// Form-fill highlights (painted over fillable field rects in form mode).
const QColor &kFormFieldColor = theme::doc().formField;            // fillable field tint
const QColor &kFormRequiredColor = theme::doc().formFieldRequired; // required-but-empty
const QColor &kFormFieldBorder = theme::doc().formFieldBorder;     // field outline
const QColor &kFormFocusBorder = theme::doc().formFieldFocus;      // Tab-focused outline

/// Read option identity independently of display text and appended custom values.
int choiceEditorIndex(QWidget *editor)
{
    if (auto *combo = qobject_cast<QComboBox *>(editor))
        return combo->currentIndex() < 0 ? -1 : combo->currentData().toInt();
    if (auto *list = qobject_cast<QListWidget *>(editor))
        return list->currentRow();
    return -1;
}

/// Select the stored option, keeping custom combo text separate from /Opt entries.
void setComboChoice(QComboBox *combo, const FormField &field)
{
    int index = field.choiceIndex;
    if (index < 0 && !field.value.isEmpty()) {
        index = combo->findData(-1);
        if (index < 0) {
            combo->addItem(field.value, -1);
            index = combo->count() - 1;
        } else {
            combo->setItemText(index, field.value);
        }
    }
    combo->setCurrentIndex(index);
}

}

void ViewerWidget::rebuildFormModel()
{
    formModel_.reset();
    if (doc_ && doc_->hasForm()) {
        formModel_ = std::make_unique<FormModel>(*doc_);
        formModel_->setPageLoader([this](int page) { requestPageData(page); });
    }
}

bool ViewerWidget::hasFormEdits() const
{
    return formModel_ && formModel_->isDirty();
}

void ViewerWidget::clearFormDirty()
{
    if (formModel_)
        formModel_->clearDirty();
    emit formEditsChanged();
}

void ViewerWidget::setFormMode(bool on)
{
    if (on) {
        if (!doc_ || !formModel_)
            return; // nothing to fill
        if (measureToolEnabled_)
            setMeasureMode(false); // forms, measure and OCR are mutually exclusive
        if (toolMode_ == ToolMode::Ocr)
            setOcrMode(false);
        setCommentToolEnabled(false); // forms are fully exclusive: close the Comment panel
        selection_.clear();
        selecting_ = false;
        if (rubberBand_)
            rubberBand_->hide();
        toolMode_ = ToolMode::FillForms;
        formFocusIndex_ = -1;
        // Fill Forms keeps normal document text selectable. Individual editor
        // widgets provide their own text/choice cursors; the page itself uses the
        // same I-beam affordance as the standard Select tool.
        viewport()->setCursor(Qt::IBeamCursor);
        syncFormEditors();
        emit formModeChanged(true);
        viewport()->update();
    } else if (toolMode_ == ToolMode::FillForms) {
        commitActiveFormEditor();
        ++formOrderRevision_;
        if (!formFieldOrderBuilt_) formOrderRequested_ = false;
        pendingFormFocus_ = 0;
        pendingFormFocusIndex_ = -1;
        formFocusField_ = {-1, -1};
        destroyFormEditors();
        toolMode_ = ToolMode::None;
        formFocusIndex_ = -1;
        viewport()->setCursor(Qt::IBeamCursor);
        emit formModeChanged(false);
        viewport()->update();
    }
}

void ViewerWidget::setHighlightFormFields(bool on)
{
    if (highlightFormFields_ == on)
        return;
    highlightFormFields_ = on;
    if (toolMode_ == ToolMode::FillForms)
        viewport()->update();
}

// ─────────────────────────────── Annotations ───────────────────────────────

void ViewerWidget::rebuildAnnotModel()
{
    annotModel_.reset();
    if (doc_ && doc_->isPdf()) {
        annotModel_ = std::make_unique<AnnotModel>(*doc_);
        annotModel_->setPageLoader([this](int page) { requestPageData(page); });
    }
}

bool ViewerWidget::hasAnnotEdits() const
{
    return annotModel_ && annotModel_->isDirty();
}

void ViewerWidget::clearAnnotDirty()
{
    if (annotModel_)
        annotModel_->clearDirty();
    emit annotEditsChanged();
}

void ViewerWidget::commitActiveAnnotEditor()
{
    if (annotPopup_ && annotPopup_->isVisible())
        annotPopup_->commit(); // pushes any edited comment into the model
}

std::vector<Annotation> ViewerWidget::allAnnotations() const
{
    if (queries_ && !annotationsRequested_ && !queryPaused_) {
        annotationsRequested_ = true;
        annotationsComplete_ = false;
        collectAnnotations(0);
    }
    return allAnnotations_;
}

void ViewerWidget::setMarkupStyle(AnnotType type)
{
    if (isTextMarkup(type))
        markupStyle_ = type;
}

void ViewerWidget::setMarkupColor(const QColor &color)
{
    if (color.isValid())
        markupColor_ = color;
}

void ViewerWidget::idleMeasureCursor()
{
    // Release the single active gesture from the measuring crosshair without
    // closing the measure panel (its committed marks stay drawn). Cancels any
    // half-drawn vector and tells the measure panel to un-check its cursor toggle.
    cancelInProgressMeasure();
    clearSnapState();
    emit measureCursorActiveChanged(false);
}

void ViewerWidget::idleAnnotGesture()
{
    // Release the single active gesture from the annotation tool without closing
    // the Comment panel; the panel shows "Select".
    closeAnnotPopup();
    selection_.clear();
    selecting_ = false;
    emit annotSubModeChanged(AnnotSubMode::Select);
}

void ViewerWidget::setCommentToolEnabled(bool on)
{
    if (on) {
        if (!doc_ || !annotModel_)
            return;
        // OCR and Forms remain fully exclusive; Measure does NOT (the panels dock
        // together) - instead the measure crosshair just idles.
        if (toolMode_ == ToolMode::Ocr)
            setOcrMode(false);
        if (toolMode_ == ToolMode::FillForms)
            setFormMode(false);
        commentToolEnabled_ = true;
        if (rubberBand_)
            rubberBand_->hide();
        // Arm the Markup sub-mode by default and take the single active gesture
        // from the measuring crosshair (the measure panel stays open if it was).
        toolMode_ = ToolMode::Highlight;
        idleMeasureCursor();
        viewport()->setCursor(Qt::IBeamCursor); // drag selects text to mark up
        emit commentToolEnabledChanged(true); // TabPage shows + docks the Comment panel
        emit annotSubModeChanged(AnnotSubMode::Markup);
        viewport()->update();
    } else if (commentToolEnabled_) {
        closeAnnotPopup();
        commentToolEnabled_ = false;
        selection_.clear();
        selecting_ = false;
        // Only release the gesture if the Comment tool holds it; if Measure owns it
        // (panel docked alongside), leave its crosshair gesture and cursor intact.
        if (toolMode_ == ToolMode::Highlight || toolMode_ == ToolMode::Comment) {
            toolMode_ = ToolMode::None;
            viewport()->setCursor(Qt::IBeamCursor);
        } else if (measureMode()) {
            viewport()->setCursor(Qt::CrossCursor);
        }
        emit commentToolEnabledChanged(false); // TabPage hides the Comment panel
        viewport()->update();
    }
}

void ViewerWidget::setAnnotSubMode(AnnotSubMode mode)
{
    if (!commentToolEnabled_)
        return;
    switch (mode) {
    case AnnotSubMode::Select:
        toolMode_ = ToolMode::None;
        closeAnnotPopup();
        selection_.clear();
        selecting_ = false;
        viewport()->setCursor(Qt::IBeamCursor);
        idleMeasureCursor(); // explicit Select is pure pointer - measure idles too
        break;
    case AnnotSubMode::Markup:
        toolMode_ = ToolMode::Highlight;
        viewport()->setCursor(Qt::IBeamCursor);
        idleMeasureCursor();
        break;
    case AnnotSubMode::Note:
        toolMode_ = ToolMode::Comment;
        viewport()->setCursor(Qt::PointingHandCursor);
        idleMeasureCursor();
        break;
    }
    emit annotSubModeChanged(mode); // keep the panel selector in sync (blocked there)
    viewport()->update();
}

QRectF ViewerWidget::annotWidgetRect(int page, int id) const
{
    if (!annotModel_)
        return {};
    for (const Annotation &a : annotModel_->pageAnnots(page))
        if (a.id == id)
            return pageRectToWidget(page, a.rect);
    return {};
}

bool ViewerWidget::annotAt(QPoint viewportPos, int &page, int &id) const
{
    if (!annotModel_)
        return false;
    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());
    bool found = false;
    // Iterate visible pages; within a page the later annotation wins (drawn on
    // top), so a click on overlapping marks selects the topmost.
    for (int p : layout_.pagesInViewport(vpCanvas)) {
        for (const Annotation &a : annotModel_->pageAnnots(p)) {
            const QRectF w = pageRectToWidget(p, a.rect);
            // Sticky-note icons are tiny; give them a little slack so they're easy
            // to click.
            const QRectF hit = a.type == AnnotType::Text ? w.adjusted(-3, -3, 3, 3) : w;
            if (hit.contains(viewportPos)) {
                page = p;
                id = a.id;
                found = true; // keep scanning: prefer the last (topmost) match
            }
        }
    }
    return found;
}

bool ViewerWidget::annotShowsReadOnly(int page, int id) const
{
    // A plain click while the Comment tool is closed should pop the read-only
    // viewer only when there's something to read: sticky notes (their whole purpose
    // is the comment) and any mark that actually carries comment text. Comment-less
    // highlights are skipped so casual clicks while reading don't flash empty cards.
    const std::optional<Annotation> a =
        annotModel_ ? annotModel_->annot(page, id) : std::nullopt;
    return a && (a->type == AnnotType::Text || !a->contents.trimmed().isEmpty());
}

void ViewerWidget::createHighlightFromSelection()
{
    if (!annotModel_ || !textIndex_ || !selection_.hasSelection())
        return;
    const TextPos s = selection_.start();
    const TextPos e = selection_.end();
    struct Result {
        std::vector<std::pair<int, int>> created;
        std::vector<Annotation> firstPage;
    };
    auto result = std::make_shared<Result>();
    cancelQueryReads();
    annotModel_->markDirty();
    queries_->edit([result, s, e, base = engine_->baseContext(), style = markupStyle_,
                     color = markupColor_, author = annotAuthor_](Document &document) {
        TextIndex text(base, &document);
        AnnotModel annotations(document);
        for (int pg = s.page; pg <= e.page; ++pg) {
            const int from = pg == s.page ? s.offset : 0;
            const int to = pg == e.page ? std::min(e.offset, text.pageTextLength(pg))
                                      : text.pageTextLength(pg);
            const auto rects = text.rangeRects(pg, from, to - from);
            if (!rects.empty()) {
                const int id = annotations.addTextMarkup(pg, style, rects, color, author);
                if (id >= 0) result->created.emplace_back(pg, id);
            }
            text.discardPage(pg);
        }
        if (!result->created.empty())
            result->firstPage = annotations.pageAnnots(result->created.front().first);
    }, [this, result] {
        for (const auto &[pg, id] : result->created) {
            annotModel_->discardPage(pg);
            applyAnnotChange(pg);
            Q_UNUSED(id);
        }
        annotationsRequested_ = annotationsComplete_ = false;
        ++annotationsRevision_;
        const QPointer<ViewerWidget> self(this);
        emit annotEditsChanged();
        if (!self) return;
        emit annotationsChanged();
        if (!self) return;
        if (!result->created.empty()) {
            const auto [pg, id] = result->created.front();
            annotModel_->storePage(pg, std::move(result->firstPage));
            openAnnotPopup(pg, id);
        }
    });
    selection_.clear();
    viewport()->update();
    emit annotEditsChanged();
}

void ViewerWidget::createCommentAt(QPoint viewportPos)
{
    if (!annotModel_)
        return;
    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0)
        return;
    const QPointF pagePoint = canvasToPagePoint(pg, canvas);
    // A new comment takes the current default annotation colour (set in Settings;
    // the single shared colour, also used for new highlights).
    setAnnotSubMode(AnnotSubMode::Select);
    submitAnnotEdit(pg, [pg, pagePoint, color = markupColor_, author = annotAuthor_](AnnotModel &model) {
        return model.addTextNote(pg, pagePoint, color, author, QString());
    }, [this, pg](int id) { if (id >= 0) openAnnotPopup(pg, id); });
}

void ViewerWidget::openAnnotPopup(int page, int id, bool readOnly)
{
    if (!annotModel_)
        return;
    const std::optional<Annotation> a = annotModel_->annot(page, id);
    if (!a)
        return;
    // Flush any edit in a popup already open for a DIFFERENT annotation before we
    // rebind to the new one. commit() runs the commentEdited lambda while
    // openAnnotPage_/openAnnotId_ still point at the OLD annotation, so the
    // in-progress text lands on the right one. The mouse paths closeAnnotPopup()
    // first, but the comments-sidebar reveal path comes straight here.
    if (annotPopup_ && annotPopup_->isVisible() && (openAnnotPage_ != page || openAnnotId_ != id))
        annotPopup_->commit();
    if (!annotPopup_) {
        annotPopup_ = new AnnotPopup(viewport());
        connect(annotPopup_, &AnnotPopup::commentEdited, this, [this](const QString &text) {
            const int page = openAnnotPage_, id = openAnnotId_;
            if (id >= 0) submitAnnotEdit(page, [page, id, text](AnnotModel &model) {
                return model.setContents(page, id, text) ? id : -1;
            });
        });
        connect(annotPopup_, &AnnotPopup::colorPicked, this, [this](const QColor &color) {
            const int page = openAnnotPage_, id = openAnnotId_;
            if (id >= 0) submitAnnotEdit(page, [page, id, color](AnnotModel &model) {
                return model.setColor(page, id, color) ? id : -1;
            });
        });
        connect(annotPopup_, &AnnotPopup::deleteRequested, this, [this] {
            const int page = openAnnotPage_, id = openAnnotId_;
            if (id < 0) return;
            closeAnnotPopup();
            submitAnnotEdit(page, [page, id](AnnotModel &model) {
                return model.remove(page, id) ? id : -1;
            });
        });
        connect(annotPopup_, &AnnotPopup::dismissed, this, [this] {
            openAnnotPage_ = -1;
            openAnnotId_ = -1;
            viewport()->update(); // clear the selection outline
            // The card is hiding. If it held the keyboard, return it to the page now,
            // or Qt passes it to the next widget in the window and the arrow and page
            // keys stop scrolling the document.
            if (isVisible() && annotPopup_->isAncestorOf(QApplication::focusWidget()))
                setFocus();
        });
    }
    openAnnotPage_ = page;
    openAnnotId_ = id;
    annotPopup_->showFor(*a, /*allowEdit=*/!readOnly);
    syncAnnotPopup();
    annotPopup_->focusComment();
    viewport()->update(); // draw the selection outline
}

void ViewerWidget::closeAnnotPopup()
{
    if (annotPopup_ && annotPopup_->isVisible())
        annotPopup_->hide(); // hideEvent commits + emits dismissed (clears open ids)
    openAnnotPage_ = -1;
    openAnnotId_ = -1;
}

void ViewerWidget::syncAnnotPopup()
{
    if (!annotPopup_ || !annotPopup_->isVisible() || openAnnotId_ < 0)
        return;
    const QRectF r = annotWidgetRect(openAnnotPage_, openAnnotId_);
    if (r.isNull()) {
        // The annotation scrolled out of view: keep the editor where it is rather
        // than jumping it around (it stays usable; closing re-commits).
        return;
    }
    annotPopup_->positionNear(r.toRect());
}

void ViewerWidget::applyAnnotChange(int page)
{
    // Re-render just the touched page (annotations are baked into the page image),
    // mirroring applyFormFieldChange: drop the cached tile, its pre-edit frozen
    // preview and any stale request, then request a fresh one.
    cache_.erase(page);
    preview_.erase(page);
    pending_.remove(page);
    renderFailures_.remove(page);
    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());
    const QRect pageCanvas = layout_.pageRect(page);
    if (pageCanvas.isValid()) {
        const QRect needed = pageCanvas.intersected(vpCanvas);
        if (!needed.isEmpty())
            ensureRendered(page, needed);
    }
    viewport()->update();
}

void ViewerWidget::revealAnnotation(int page, int id)
{
    if (!annotModel_)
        return;
    if (!annotModel_->pageReady(page)) {
        pendingReveal_ = std::pair{page, id};
        goToPage(page);
        requestPageData(page);
        return;
    }
    pendingReveal_.reset();
    const std::optional<Annotation> a = annotModel_->annot(page, id);
    if (!a)
        return;
    // Scroll the annotation into view, then open its inline editor.
    goToPage(page);
    const QRectF canvasRect = pageRectToCanvas(page, a->rect);
    ensureCanvasRectVisible(canvasRect);
    // Editable only while an annotation gesture is active (Highlight / Comment
    // sub-mode); view-only otherwise, matching a plain pointer/Select click.
    openAnnotPopup(page, id, /*readOnly=*/!annotationMode());
}

void ViewerWidget::drawAnnotSelection(QPainter &p, int pageNo) const
{
    if (openAnnotId_ < 0 || openAnnotPage_ != pageNo || !annotModel_)
        return;
    const QRectF w = annotWidgetRect(pageNo, openAnnotId_);
    if (w.isNull())
        return;
    p.setPen(QPen(pageAccent(), 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(w.adjusted(-2, -2, 2, 2));
}

int ViewerWidget::editorIndexFor(QObject *widget) const
{
    for (int i = 0; i < static_cast<int>(formEditors_.size()); ++i)
        if (formEditors_[i].widget == widget)
            return i;
    return -1;
}

QWidget *ViewerWidget::createFormEditor(const FormField &f, int page, int fieldIndex)
{
    QWidget *w = nullptr;
    switch (f.type) {
    case FormFieldType::Text:
        if (f.multiline() || f.comb()) {
            auto *te = new QPlainTextEdit(viewport());
            te->setFrameShape(QFrame::NoFrame);
            te->document()->setDocumentMargin(0); // tighten text origin vs the render
            te->setPlainText(f.value);
            w = te;
        } else {
            auto *le = new QLineEdit(viewport());
            le->setFrame(false);
            le->setText(f.value);
            w = le;
        }
        break;
    case FormFieldType::ComboBox: {
        auto *cb = new QComboBox(viewport());
        for (int i = 0; i < f.options.size(); ++i)
            cb->addItem(f.options[i], i);
        setComboChoice(cb, f);
        connect(cb, &QComboBox::activated, this, [this, page, fieldIndex, cb](int) {
            if (formModel_)
                editFormChoice(page, fieldIndex, cb->currentText(), choiceEditorIndex(cb));
        });
        w = cb;
        break;
    }
    case FormFieldType::ListBox: {
        auto *lw = new QListWidget(viewport());
        lw->setSelectionMode(QAbstractItemView::SingleSelection);
        lw->addItems(f.options);
        lw->setCurrentRow(f.choiceIndex);
        connect(lw, &QListWidget::itemSelectionChanged, this, [this, page, fieldIndex, lw]() {
            const QString v = lw->currentItem() ? lw->currentItem()->text() : QString();
            if (formModel_)
                editFormChoice(page, fieldIndex, v, choiceEditorIndex(lw));
        });
        w = lw;
        break;
    }
    default:
        return nullptr; // toggles / signatures / push buttons have no inline editor
    }
    // Give the inline editor a clean light-blue input surface so the field being
    // filled stands out from the page (and so it overrides the app-wide chrome
    // stylesheet, which would otherwise tint it to the dark UI theme and clash
    // with the white page). Tight padding keeps the text aligned in the rect.
    const theme::Doc &d = theme::doc();
    w->setStyleSheet(
        QStringLiteral("QLineEdit, QPlainTextEdit, QComboBox, QListWidget {"
                       " background:%1; color:%2; border:1px solid %3; border-radius:2px;"
                       " padding:0 3px; selection-background-color:%4; selection-color:%5; }"
                       "QComboBox::drop-down { border:none; width:16px; }"
                       // The popup is a separate top-level view, so none of the type
                       // selectors above reach it; without this the field is a pale
                       // blue input whose list opens as a dark chrome menu.
                       "QComboBox QAbstractItemView { background:%1; color:%2;"
                       " selection-background-color:%4; selection-color:%5; }")
            .arg(theme::css(d.formEditorSurface), theme::css(d.formEditorInk),
                 theme::css(d.formEditorBorder), theme::css(d.formEditorSelection),
                 theme::css(d.formEditorSelectionInk)));
    w->setProperty("mervinFormValue", f.value);
    w->setProperty("mervinFormChoiceIndex", f.choiceIndex);
    w->installEventFilter(this);
    return w;
}

void ViewerWidget::syncFormEditors()
{
    if (syncingFormEditors_)
        return;
    syncingFormEditors_ = true;
    if (toolMode_ != ToolMode::FillForms || !formModel_) {
        syncingFormEditors_ = false;
        destroyFormEditors();
        return;
    }

    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());
    QSet<int> visible;
    for (int p : layout_.pagesInViewport(vpCanvas))
        visible.insert(p);

    // Drop editors whose page has scrolled out of view (committing first).
    for (size_t i = 0; i < formEditors_.size();) {
        if (!visible.contains(formEditors_[i].page)) {
            commitFormEditor(static_cast<int>(i));
            if (formEditors_[i].widget) {
                formEditors_[i].widget->removeEventFilter(this);
                formEditors_[i].widget->deleteLater();
            }
            formEditors_.erase(formEditors_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }

    // Ensure an editor exists for every editable text/choice field on a visible
    // page, and (re)position all editors over their fields.
    for (int p : layout_.pagesInViewport(vpCanvas)) {
        if (!layout_.pageRect(p).isValid())
            continue;
        const std::vector<FormField> &fields = formModel_->pageFields(p);
        for (int fi = 0; fi < static_cast<int>(fields.size()); ++fi) {
            const FormField &f = fields[fi];
            if (!f.editable() || f.isToggle())
                continue;
            int existing = -1;
            for (int k = 0; k < static_cast<int>(formEditors_.size()); ++k)
                if (formEditors_[k].page == p && formEditors_[k].fieldIndex == fi) {
                    existing = k;
                    break;
                }
            QWidget *w = nullptr;
            if (existing < 0) {
                w = createFormEditor(f, p, fi);
                if (!w)
                    continue;
                formEditors_.push_back(FormEditor{p, fi, f.type, w});
            } else {
                w = formEditors_[existing].widget;
            }
            const QRect r = pageRectToWidget(p, f.rect).toRect();
            w->setGeometry(r);
            // Scale the editor font to the PDF field height, excluding Qt padding and borders.
            {
                const int px = std::clamp(qRound(f.fontSizePt * scale_), 8, 4000);
                QFont fnt = w->font();
                if (fnt.pixelSize() != px
                    || (!f.fontFamily.isEmpty() && fnt.family() != f.fontFamily)) {
                    fnt.setPixelSize(px);
                    if (!f.fontFamily.isEmpty())
                        fnt.setFamily(f.fontFamily);
                    w->setFont(fnt);
                }
            }
            w->show();
        }
    }
    syncingFormEditors_ = false;
}

void ViewerWidget::destroyFormEditors()
{
    for (FormEditor &e : formEditors_) {
        if (e.widget) {
            e.widget->removeEventFilter(this);
            e.widget->deleteLater();
        }
    }
    formEditors_.clear();
}

void ViewerWidget::commitFormEditor(int editorIndex)
{
    if (editorIndex < 0 || editorIndex >= static_cast<int>(formEditors_.size()) || !formModel_)
        return;
    const FormEditor &e = formEditors_[editorIndex];
    QString current;
    if (auto *line = qobject_cast<QLineEdit *>(e.widget)) current = line->text();
    else if (auto *text = qobject_cast<QPlainTextEdit *>(e.widget)) current = text->toPlainText();
    else if (auto *combo = qobject_cast<QComboBox *>(e.widget)) current = combo->currentText();
    else if (auto *list = qobject_cast<QListWidget *>(e.widget)) current = list->currentItem() ? list->currentItem()->text() : QString();
    const int choiceIndex = choiceEditorIndex(e.widget);
    if (current == e.widget->property("mervinFormValue").toString()
        && choiceIndex == e.widget->property("mervinFormChoiceIndex").toInt()) return;
    if (e.type == FormFieldType::Text)
        editFormValue(e.page, e.fieldIndex, current);
    else if (e.type == FormFieldType::ComboBox || e.type == FormFieldType::ListBox)
        editFormChoice(e.page, e.fieldIndex, current, choiceIndex);
}

// Commits pending field values and stops if an edit observer destroys the viewer.
void ViewerWidget::commitActiveFormEditor()
{
    // Idempotent (unchanged values are no-ops), so flushing every editor before a
    // save / print or mode change is safe and catches the field still being typed.
    const QPointer<ViewerWidget> self(this);
    for (int i = static_cast<int>(formEditors_.size()) - 1; i >= 0; --i) {
        commitFormEditor(i);
        if (!self)
            return;
    }
}

void ViewerWidget::applyFormFieldChange(int page)
{
    QSet<int> pages{page};
    if (formModel_) {
        for (const auto &[changedPage, fieldIndex] : formModel_->changedFields()) {
            pages.insert(changedPage);
            const auto &fields = formModel_->pageFields(changedPage);
            if (fieldIndex < 0 || fieldIndex >= static_cast<int>(fields.size()))
                continue;
            const FormField &field = fields[fieldIndex];
            const QString &value = field.value;
            for (const FormEditor &editor : formEditors_) {
                if (editor.page != changedPage || editor.fieldIndex != fieldIndex || !editor.widget)
                    continue;
                // A sibling editor must not later commit its pre-edit value over
                // this shared field. Unrelated pending edits remain untouched.
                QString current;
                if (auto *line = qobject_cast<QLineEdit *>(editor.widget)) current = line->text();
                else if (auto *text = qobject_cast<QPlainTextEdit *>(editor.widget)) current = text->toPlainText();
                else if (auto *combo = qobject_cast<QComboBox *>(editor.widget)) current = combo->currentText();
                else if (auto *list = qobject_cast<QListWidget *>(editor.widget)) current = list->currentItem() ? list->currentItem()->text() : QString();
                if (current != editor.widget->property("mervinFormValue").toString()
                    || choiceEditorIndex(editor.widget)
                        != editor.widget->property("mervinFormChoiceIndex").toInt()) continue;
                editor.widget->setProperty("mervinFormValue", value);
                editor.widget->setProperty("mervinFormChoiceIndex", field.choiceIndex);
                const QSignalBlocker block(editor.widget);
                if (auto *line = qobject_cast<QLineEdit *>(editor.widget)) {
                    if (line->text() != value)
                        line->setText(value);
                } else if (auto *text = qobject_cast<QPlainTextEdit *>(editor.widget)) {
                    if (text->toPlainText() != value)
                        text->setPlainText(value);
                } else if (auto *combo = qobject_cast<QComboBox *>(editor.widget)) {
                    setComboChoice(combo, field);
                } else if (auto *list = qobject_cast<QListWidget *>(editor.widget)) {
                    list->setCurrentRow(field.choiceIndex);
                }
            }
        }
    }
    // Retire all pre-edit rasters and request identities, including offscreen
    // siblings. Fresh visible tiles supersede any old result already in flight.
    for (int affected : pages) {
        cache_.erase(affected);
        preview_.erase(affected);
        pending_.remove(affected);
        renderFailures_.remove(affected);
    }
    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());
    for (int affected : pages) {
        const QRect pageCanvas = layout_.pageRect(affected);
        if (pageCanvas.isValid()) {
            const QRect needed = pageCanvas.intersected(vpCanvas);
            if (!needed.isEmpty())
                ensureRendered(affected, needed);
        }
    }
    viewport()->update();
}

const std::vector<std::pair<int, int>> &ViewerWidget::formFieldOrder()
{
    static const std::vector<std::pair<int, int>> kEmpty;
    if (!formModel_)
        return kEmpty;
    if (!formFieldOrderBuilt_ && !formOrderRequested_) {
        formOrderRequested_ = true;
        formFieldOrder_.clear();
        collectFormOrder(0);
    }
    return formFieldOrder_;
}

void ViewerWidget::focusFormFieldAt(int orderIndex)
{
    if (toolMode_ != ToolMode::FillForms) return;
    const std::vector<std::pair<int, int>> &order = formFieldOrder();
    if (orderIndex < 0 || orderIndex >= static_cast<int>(order.size()) || !formModel_)
        return;
    formFocusIndex_ = orderIndex;
    formFocusField_ = order[orderIndex];
    const int pg = order[orderIndex].first;
    const int fi = order[orderIndex].second;
    if (!formModel_->pageReady(pg)) {
        pendingFormFocusIndex_ = orderIndex;
        requestPageData(pg);
        return;
    }
    pendingFormFocusIndex_ = -1;
    const std::vector<FormField> &fields = formModel_->pageFields(pg);
    if (fi < 0 || fi >= static_cast<int>(fields.size()))
        return;
    ensureCanvasRectVisible(pageRectToCanvas(pg, fields[fi].rect)); // may scroll
    syncFormEditors(); // make sure the now-visible field's editor exists
    if (fields[fi].isToggle()) {
        setFocus(); // a toggle has no editor; Space/Enter flips it via keyPressEvent
        viewport()->update();
        return;
    }
    for (FormEditor &e : formEditors_)
        if (e.page == pg && e.fieldIndex == fi && e.widget) {
            e.widget->setFocus(Qt::TabFocusReason);
            if (auto *le = qobject_cast<QLineEdit *>(e.widget))
                le->selectAll();
            break;
        }
    viewport()->update();
}

void ViewerWidget::advanceFormFocus(int delta)
{
    const std::vector<std::pair<int, int>> &order = formFieldOrder();
    if (!formFieldOrderBuilt_) {
        pendingFormFocus_ += delta;
        return;
    }
    if (order.empty())
        return;
    const int n = static_cast<int>(order.size());
    int idx = (formFocusIndex_ < 0) ? (delta > 0 ? -1 : 0) : formFocusIndex_;
    idx = ((idx + delta) % n + n) % n; // wrap both directions
    focusFormFieldAt(idx);
}

bool ViewerWidget::formFieldAt(QPoint viewportPos, int &page, int &fieldIndex) const
{
    if (!formModel_)
        return false;
    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0 || !layout_.pageRect(pg).isValid())
        return false;
    const QPointF pp = canvasToPagePoint(pg, QPointF(canvas), true);
    const std::vector<FormField> &fields = formModel_->pageFields(pg);
    for (int i = 0; i < static_cast<int>(fields.size()); ++i)
        if (fields[i].editable() && fields[i].rect.contains(pp)) {
            page = pg;
            fieldIndex = i;
            return true;
        }
    return false;
}

void ViewerWidget::drawFormHighlights(QPainter &p, int pageNo) const
{
    if (!formModel_)
        return;
    const std::vector<FormField> &fields = formModel_->pageFields(pageNo);
    p.save();
    for (int fi = 0; fi < static_cast<int>(fields.size()); ++fi) {
        const FormField &f = fields[fi];
        if (!f.editable())
            continue;
        const QRectF wr = pageRectToWidget(pageNo, f.rect);
        if (!wr.isValid())
            continue;
        if (highlightFormFields_) {
            const bool requiredEmpty = f.required() && f.value.isEmpty() && !f.isToggle();
            p.setPen(QPen(kFormFieldBorder, 1.0));
            p.setBrush(requiredEmpty ? kFormRequiredColor : kFormFieldColor);
            p.drawRect(wr.adjusted(0, 0, -1, -1));
        }
        // Tab-focused field ring (the only on-screen cue for a focused toggle).
        if (formFocusIndex_ >= 0 && formFocusIndex_ < static_cast<int>(formFieldOrder_.size())
            && formFieldOrder_[formFocusIndex_].first == pageNo
            && formFieldOrder_[formFocusIndex_].second == fi) {
            p.setPen(QPen(kFormFocusBorder, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(wr.adjusted(1, 1, -2, -2));
        }
    }
    p.restore();
}


} // namespace mervin
