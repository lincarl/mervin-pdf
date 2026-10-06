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
#include <QResizeEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
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
constexpr double kMinScale = 0.08;
constexpr double kMaxScale = 100.0; // 10000% - deep zoom uses clipped rendering (see ensureRendered)

// Discrete zoom uses round rungs; pinch gestures and typed values remain continuous.
// Every rung exceeds kZoomLadderMinStep so the minimum-step rule cannot skip alternate levels.
constexpr double kZoomLadder[] = {0.08, 0.12, 0.18, 0.25, 0.35, 0.5,  0.75,
                                  1.0,  1.5,  2.0,  3.0,  4.0,  6.0,  8.0,
                                  12.0, 16.0, 24.0, 32.0, 50.0, 70.0, 100.0};
// A rung nearer than this is skipped, so a scale that sits just short of one does
// not produce a zoom too small to see: coming off Fit Width at 95% the next rung up
// is 150%, not 100%. Set to the old fixed step, so no gesture is ever smaller than
// it used to be.
constexpr double kZoomLadderMinStep = 1.25;
// One wheel notch is 120 units and steps one rung. High-resolution wheels and
// trackpads report a fraction of a notch per event, so the deltas are accumulated
// (see wheelEvent) instead of stepping a rung per micro-tick.
constexpr int kWheelNotch = 120;
// Above this device-pixel area, a page is rendered as a single viewport-sized
// tile (only the visible region) rather than one whole-page bitmap. Deep zoom
// would otherwise allocate gigantic pixmaps that MuPDF refuses, blanking the
// page. ~32 MP ≈ 96 MB at RGB888.
constexpr double kMaxWholePagePx = 32.0 * 1024.0 * 1024.0;
// Logical-px margin rendered around the visible region of a clipped page, so a
// small scroll/pan does not immediately force a re-render.
constexpr int kClipMargin = 256;
// How far a frozen preview image may be magnified before the viewer stops
// showing it (see drawPreview): beyond this it is unrecognisable mush, and clean
// paper for the length of one render is the better picture.
constexpr double kMaxPreviewStretch = 8.0;

// Animate the drawn rectangles only; layout and render requests immediately use the final scale.
constexpr int kZoomEaseMs = 130;     // for exactly one kZoomStep
constexpr int kZoomEaseTickMs = 16;

// save()/restore() that survives the `continue` in the middle of paintEvent's page
// loop (the no-text-index early out). A hand-rolled pair would leak painter state
// there and mis-place every later page.
struct PainterStateGuard
{
    explicit PainterStateGuard(QPainter &pp) : p(pp) { p.save(); }
    ~PainterStateGuard() { p.restore(); }
    PainterStateGuard(const PainterStateGuard &) = delete;
    PainterStateGuard &operator=(const PainterStateGuard &) = delete;
    QPainter &p;
};

// Overlays painted onto the page. Every value comes from theme::doc() - the
// document half of the colour vocabulary, which does not follow the UI theme
// because these have to work on the page's own paper (see ui/ThemeTokens.h).
const QColor &kMatchColor = theme::doc().findMatch;               // all matches: yellow
const QColor &kCurrentMatchColor = theme::doc().findMatchCurrent; // active match: orange
const QColor &kSelectionColor = theme::doc().textSelection;       // text selection: blue

constexpr int kAutoScrollMargin = 24; // px from the viewport edge
constexpr int kAutoScrollMaxStep = 40;

// Hand out a unique id per viewer instance. Viewers are only created on the UI
// thread, so a plain counter is sufficient; it is monotonic (never reused) so a
// late result for a destroyed viewer cannot be mistaken for a freshly created one.
quint64 nextViewerId()
{
    static quint64 counter = 0;
    return ++counter;
}

std::optional<QUrl> openableWebUrl(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return std::nullopt;
    if (text.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        text.prepend(QStringLiteral("https://"));

    const QUrl url = QUrl::fromUserInput(text);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        return std::nullopt;
    }
    return url;
}
} // namespace

ViewerWidget::ViewerWidget(RenderEngine *engine, QWidget *parent)
    : QAbstractScrollArea(parent)
    , engine_(engine)
    , viewerId_(nextViewerId())
{
    // Window gives viewport children readable foreground colors. Painting supplies the canvas background.
    documentSearch_ = std::make_unique<DocumentSearch>(engine_);
    viewport()->setBackgroundRole(QPalette::Window);
    viewport()->setCursor(Qt::IBeamCursor); // text-selection affordance
    // Deliver mouse-move events even when no button is held. Without this the
    // measuring tool's live preview and snap marker never update while the
    // cursor hovers toward the next point (QAbstractScrollArea's viewport has
    // mouse tracking off by default), which makes snapping appear not to work.
    viewport()->setMouseTracking(true);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(40);

    autoScrollTimer_ = new QTimer(this);
    autoScrollTimer_->setInterval(30);
    connect(autoScrollTimer_, &QTimer::timeout, this, &ViewerWidget::onAutoScroll);

    zoomEaseTimer_ = new QTimer(this);
    // A coarse timer plus Windows' ~15.6 ms system tick cannot hold 60 fps. The
    // ease reads its progress from a QElapsedTimer, so the tick rate only affects
    // how smooth it looks, never how long it takes.
    zoomEaseTimer_->setTimerType(Qt::PreciseTimer);
    zoomEaseTimer_->setInterval(kZoomEaseTickMs);
    connect(zoomEaseTimer_, &QTimer::timeout, this, &ViewerWidget::onZoomEaseTick);

    connect(engine_, &RenderEngine::resultReady, this, &ViewerWidget::onResultReady);
}

ViewerWidget::~ViewerWidget()
{
    documentSearch_.reset();
    engine_->cancelRequests(viewerId_);
}

int ViewerWidget::pageCount() const
{
    return doc_ ? doc_->pageCount() : 0;
}

double ViewerWidget::clampScale(double s) const
{
    return std::clamp(s, kMinScale, kMaxScale);
}

double ViewerWidget::nextZoomLevel(double current, int dir) const
{
    // The next rung of kZoomLadder in `dir`, skipping any that sits nearer than
    // kZoomLadderMinStep - so a scale just short of a rung (Fit Width at 95%, or a
    // pinch that landed on 103%) steps past it rather than producing a zoom too
    // small to see. At either end of the ladder the clamp is returned, which the
    // callers turn into a no-op.
    if (dir > 0) {
        const double floorScale = current * kZoomLadderMinStep;
        for (double rung : kZoomLadder) {
            if (rung > floorScale)
                return rung;
        }
        return kMaxScale;
    }
    const double ceilScale = current / kZoomLadderMinStep;
    for (auto it = std::rbegin(kZoomLadder); it != std::rend(kZoomLadder); ++it) {
        if (*it < ceilScale)
            return *it;
    }
    return kMinScale;
}

QPoint ViewerWidget::scrollOffset() const
{
    return {horizontalScrollBar()->value(), verticalScrollBar()->value()};
}

// When the laid-out content is smaller than the viewport on an axis, centre it
// (otherwise QAbstractScrollArea parks it at the top-left). Used e.g. by Fit
// Page, where the page is scaled to the height and is narrower than the window.
QPoint ViewerWidget::centerDelta() const
{
    const QSize total = layout_.totalSize();
    const QSize vp = viewport()->size();
    return {std::max(0, (vp.width() - total.width()) / 2),
            std::max(0, (vp.height() - total.height()) / 2)};
}

// Translation from content (canvas) coordinates to viewport coordinates:
//   viewportPos = contentPos - contentOffset().
// Equals the scroll offset, shifted to centre under-sized content.
QPoint ViewerWidget::contentOffset() const
{
    return scrollOffset() - centerDelta();
}

ViewerWidget::ResumeState ViewerWidget::captureResumeState() const
{
    ResumeState state;
    const auto anchor = scrollAnchor();
    state.view.page = pendingRestore_ ? pendingPage_ : anchor.page;
    state.view.offsetX = pendingRestore_ ? pendingFracX_ : anchor.fracX;
    state.view.offsetY = pendingRestore_ ? pendingFracY_ : anchor.fracY;
    state.view.rotation = rotation_;
    state.view.scale = scale_;
    state.view.zoomMode = zoomMode_ == ZoomMode::FitPage ? QStringLiteral("fit-page")
                         : zoomMode_ == ZoomMode::Custom ? QStringLiteral("custom")
                                                       : QStringLiteral("fit-width");
    state.layout = layoutMode_;
    state.query = findQuery_;
    state.caseSensitive = findCaseSensitive_;
    state.wholeWord = findWholeWord_;
    state.currentMatch = currentMatch_;
    state.tool = toolMode_;
    state.measureEnabled = measureToolEnabled_;
    state.commentEnabled = commentToolEnabled_;
    state.measureKind = measureKind_;
    state.measureUnit = measureUnit_;
    state.measurePrecision = measurePrecision_;
    state.measureLineWidth = measureLineWidth_;
    state.inProgress = inProgress_;
    state.inProgressPage = inProgressPage_;
    return state;
}

void ViewerWidget::restoreResumeState(const ResumeState &state)
{
    if (!doc_)
        return;
    const bool wasSuppressed = zoomEaseSuppress_;
    zoomEaseSuppress_ = true;
    setLayoutMode(state.layout);
    setRotation(state.view.rotation);
    if (state.view.zoomMode == QLatin1String("custom"))
        setScale(state.view.scale);
    else
        setZoomMode(state.view.zoomMode == QLatin1String("fit-page")
                        ? ZoomMode::FitPage : ZoomMode::FitWidth);
    restorePageScrollFraction(std::clamp(state.view.page, 0, std::max(0, pageCount() - 1)),
                              state.view.offsetX, state.view.offsetY);
    zoomEaseSuppress_ = wasSuppressed;

    measureKind_ = state.measureKind;
    measureUnit_ = state.measureUnit;
    measurePrecision_ = state.measurePrecision;
    measureLineWidth_ = state.measureLineWidth;
    measureToolEnabled_ = state.measureEnabled;
    commentToolEnabled_ = state.commentEnabled && annotModel_;
    toolMode_ = state.tool;
    if (toolMode_ == ToolMode::FillForms && !formModel_)
        toolMode_ = ToolMode::None;
    if ((toolMode_ == ToolMode::Highlight || toolMode_ == ToolMode::Comment) && !annotModel_)
        toolMode_ = ToolMode::None;
    inProgress_ = state.inProgress;
    inProgressPage_ = state.inProgressPage;
    if (inProgressPage_ >= pageCount()) {
        inProgress_.clear();
        inProgressPage_ = -1;
    }
    viewport()->setCursor(measureMode() || toolMode_ == ToolMode::Ocr ? Qt::CrossCursor
                             : commentMode() ? Qt::PointingHandCursor : Qt::IBeamCursor);
    // Opening a form can create editors automatically. Remove them if this tab's saved tool
    // was something else, even when its form fields still exist.
    syncFormEditors();
    emit measureModeChanged(measureToolEnabled_);
    emit measureCursorActiveChanged(measureMode());
    emit commentToolEnabledChanged(commentToolEnabled_);
    emit annotSubModeChanged(toolMode_ == ToolMode::Highlight ? AnnotSubMode::Markup
                             : toolMode_ == ToolMode::Comment ? AnnotSubMode::Note
                                                             : AnnotSubMode::Select);
    emit formModeChanged(formMode());
    if (measureToolEnabled_ && pageCount() > 0) {
        const QSizeF size = doc_->pageSize(currentPage_);
        updateHoverScale(currentPage_, QPointF(size.width() / 2, size.height() / 2));
    }
    emitMeasurementsChanged();

    findQuery_ = state.query;
    findCaseSensitive_ = state.caseSensitive;
    findWholeWord_ = state.wholeWord;
    // Rebuild search results without moving the restored reading position.
    documentSearch_->start(doc_, state.query, state.caseSensitive, state.wholeWord,
                           [this, match = state.currentMatch](std::vector<TextMatch> matches) {
        matches_ = std::move(matches);
        rebuildMatchIndex();
        currentMatch_ = matches_.empty() ? -1
            : std::clamp(match, 0, static_cast<int>(matches_.size()) - 1);
        emit findStatusChanged(currentMatch_ + 1, static_cast<int>(matches_.size()));
        viewport()->update();
    });
    viewport()->update();
}

void ViewerWidget::setDocument(Document *doc)
{
    engine_->cancelRequests(viewerId_);
    documentSearch_->clear();
    // First, while doc_ and layout_ still agree: no gliding the new document's
    // pages in from where the old one's happened to be.
    endZoomEase();
    doc_ = doc;
    rotation_ = 0;
    currentPage_ = 0;
    zoomMode_ = ZoomMode::FitWidth;
    cache_.clear();
    preview_.clear(); // the frozen images belong to the outgoing document
    pending_.clear();

    // Per-document text model (lazy extraction) backing find and selection.
    textIndex_ = doc_ ? std::make_unique<TextIndex>(engine_->baseContext(), doc_) : nullptr;
    selection_.clear();
    selecting_ = false;
    stopAutoScroll();
    clearLinkToolTip();

    // Measuring tool: drop any measurements/overrides and leave measure mode.
    toolMode_ = ToolMode::None;
    measureToolEnabled_ = false;
    measurements_ = {};
    measurements_.shrink_to_fit();
    inProgress_ = {};
    inProgress_.shrink_to_fit();
    inProgressPage_ = -1;
    hoverValid_ = false;
    measureOverrides_ = MeasureModel{};
    lastScaleDesc_.clear();
    lastScaleResettable_ = -1;
    scalePage_ = -1;
    measureGeo_ = PageGeometry{};
    measureGeoPage_ = -1;
    clearSnapState();
    measureDrag_ = MeasureDrag::None;
    dragMeasureIdx_ = -1;
    panning_ = false;
    if (rubberBand_)
        rubberBand_->hide();
    sincePanelPopupClosed_.invalidate();
    viewport()->setCursor(Qt::IBeamCursor);
    emit measureModeChanged(false);
    emit measurementReadout(QString());

    // Form filling: tear down any editors and (re)build the model for the new doc.
    destroyFormEditors();
    formFieldOrder_.clear();
    formFieldOrder_.shrink_to_fit();
    formFieldOrderBuilt_ = false;
    formFocusIndex_ = -1;
    rebuildFormModel();
    emit formModeChanged(false);
    emit formEditsChanged();

    // Annotations: close any open inline editor and (re)build the model. Built for
    // every PDF document (any PDF can carry annotations), so the Highlight/Comment
    // actions enable and existing marks are listable.
    closeAnnotPopup();
    closePropertiesPopup();
    commentToolEnabled_ = false;
    rebuildAnnotModel();
    emit commentToolEnabledChanged(false);
    emit annotSubModeChanged(AnnotSubMode::Select);
    emit annotEditsChanged();
    emit annotationsChanged();
    matches_ = {};
    matches_.shrink_to_fit();
    matchesByPage_.clear();
    currentMatch_ = -1;
    findQuery_.clear();
    emit findStatusChanged(0, 0);

    markMeasurementsSaved();
    emitMeasurementsChanged();
    layout_ = ViewLayout{};
    layout_.setDocument(doc_);
    dpr_ = devicePixelRatioF();
    applyFitScale();
    relayout();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    emit zoomModeChanged(zoomMode_);
    emit scaleChanged(scale_);
    emit pageChanged(pageCount() > 0 ? 1 : 0, pageCount());
    setFocus();

    // Auto-enter Fill Forms for a document that carries fillable fields (user
    // setting, default on). Done here so it fires on every open path - fresh
    // open, session restore, save-and-reopen - not just one call site. A document
    // that also restores measurements turns the measure tool on right after this
    // (loadMeasurements), which clears form mode again, so measure wins there.
    if (autoFormFill_ && formModel_)
        setFormMode(true);
}

void ViewerWidget::applyFitScale()
{
    if (zoomMode_ != ZoomMode::FitWidth && zoomMode_ != ZoomMode::FitPage)
        return;
    if (!doc_ || doc_->pageCount() == 0)
        return;

    // Fit against the viewport without scrollbars to avoid visibility/resize feedback.
    // Qt hides scrollbar containers, so isVisibleTo() detects their contribution; maximumViewportSize()
    // already excludes AlwaysOn bars.
    QScrollBar *vsb = verticalScrollBar();
    QScrollBar *hsb = horizontalScrollBar();
    const int vsbW = vsb->sizeHint().width();
    const int hsbH = hsb->sizeHint().height();
    const double fullW = std::max(1, viewport()->width() + (vsb->isVisibleTo(this) ? vsbW : 0));
    const double fullH = std::max(1, viewport()->height() + (hsb->isVisibleTo(this) ? hsbH : 0));

    // Use ViewLayout for fit arithmetic, including spread columns and mixed page sizes.
    // Fit Page limits height to the current row; Fit Width uses the complete canvas width.
    const auto fitTo = [&](double w) {
        const ViewLayout::FitBasis b =
            layout_.fitBasis(w, fullH, layoutMode_, rotation_, currentPage_);
        return (zoomMode_ == ZoomMode::FitWidth)
                   ? clampScale(b.widthScale)
                   : clampScale(std::min(b.widthScale, b.heightScale));
    };
    double s = fitTo(fullW);
    // Content that scrolls vertically at this scale will show the vertical bar,
    // so re-fit into the bar-reduced width up front. Preferring the bar-shown
    // solution keeps the result unique even when the two states disagree: worst
    // case the page sits one bar-width narrower than optimal, never unstable.
    if (layout_.heightForScale(s, layoutMode_, rotation_, currentPage_) > fullH)
        s = fitTo(std::max(1.0, fullW - vsbW));
    scale_ = s;
}

void ViewerWidget::relayout()
{
    dpr_ = devicePixelRatioF();
    layout_.setMode(layoutMode_);
    layout_.setRotation(rotation_);
    layout_.setScale(scale_);
    if (layoutMode_.scroll == ViewLayout::Scroll::Single)
        layout_.setCurrentPage(currentPage_);
    updateScrollBars();
    if (toolMode_ == ToolMode::FillForms)
        syncFormEditors(); // re-anchor editors after a zoom / rotation / page-mode change
    viewport()->update();
}

void ViewerWidget::updateScrollBars()
{
    const QSize total = layout_.totalSize();
    const QSize vp = viewport()->size();
    verticalScrollBar()->setRange(0, std::max(0, total.height() - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
    horizontalScrollBar()->setRange(0, std::max(0, total.width() - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
}

void ViewerWidget::invalidateRenders(PreviewPolicy preview)
{
    if (preview == PreviewPolicy::Keep)
        seedPreview(); // before cache_.clear(): it reads the images we are dropping
    else
        preview_.clear();
    engine_->cancelRequests(viewerId_);
    ++viewEpoch_; // per-viewer epoch; results from older epochs are discarded
    cache_.clear();
    pending_.clear();
}

void ViewerWidget::seedPreview()
{
    if (!doc_ || pageCount() == 0)
        return;
    const QRect vpCanvas(contentOffset(), viewport()->size());
    // Capture the old viewport plus the area a zoom-out reveals. Cap expansion to bound preview memory.
    const double ratio = std::clamp(layout_.scale() / std::max(scale_, 1e-6), 1.0, 8.0);
    const int dx = static_cast<int>(vpCanvas.width() * (ratio - 0.5));
    const int dy = static_cast<int>(vpCanvas.height() * (ratio - 0.5));
    std::vector<int> pages = layout_.pagesInViewport(vpCanvas.adjusted(-dx, -dy, dx, dy));
    if (pages.empty())
        return;

    // Nearest to the page being read first: the byte budget is spent in this
    // order, and at deep zoom one tile can claim most of it.
    std::stable_sort(pages.begin(), pages.end(), [this](int a, int b) {
        return std::abs(a - currentPage_) < std::abs(b - currentPage_);
    });

    // Rebuilt from scratch rather than edited in place, so a tile for a page we
    // have moved away from can never squat the budget ahead of the page the user
    // is actually looking at.
    PreviewLayer next;
    for (int pageNo : pages) {
        // Carry the tile the page already had, then let a fresh render replace
        // it. During a fast zoom burst nothing fresh has landed yet, and the
        // older tile still maps correctly - it carries its own page fraction, so
        // it lands in the right place at any scale.
        if (const PreviewLayer::Tile *t = preview_.tile(pageNo))
            next.adopt(pageNo, *t);
        if (const PageCache::Entry *e = cache_.get(pageNo))
            next.add(pageNo, e->image, e->covered, layout_.pageRect(pageNo));
    }
    preview_ = std::move(next);
}

bool ViewerWidget::drawPreview(QPainter &p, int pageNo, const QRect &pageCanvas,
                               const QPoint &off, const QRect &easedCanvas) const
{
    const PreviewLayer::Tile *t = preview_.tile(pageNo);
    if (!t || t->image.isNull())
        return false;
    // Discard previews stretched beyond the quality limit, using final page geometry even during easing.
    if (PreviewLayer::targetRect(*t, pageCanvas).width() * dpr_
        > kMaxPreviewStretch * t->image.width()) {
        return false;
    }
    const QRectF target = PreviewLayer::targetRect(*t, easedCanvas).translated(-off);

    QRectF visible;
    QRectF source;
    // Draw only the visible slice: at deep zoom the full target is hundreds of
    // thousands of pixels across, which would both risk raster-engine
    // coordinate limits and tie the blit's cost to the zoom level.
    if (!PreviewLayer::clipToViewport(target, t->image,
                                      QRectF(QPointF(0, 0), QSizeF(viewport()->size())), &visible,
                                      &source)) {
        return false;
    }
    const bool wasSmooth = p.testRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(visible, t->image, source);
    p.setRenderHint(QPainter::SmoothPixmapTransform, wasSmooth);
    return true;
}

// Zoom easing interpolates viewport rectangles after the final layout is installed.
// Capture any interrupted animation before changing scale, then arm easing after scrollbar updates.
bool ViewerWidget::zoomEaseAllowed() const
{
    if (zoomEaseMs_ <= 0 || zoomEaseSuppress_ || !doc_ || pageCount() == 0 || !isVisible())
        return false;
    // Page-anchored CHILD widgets cannot be blitted with the page, and hiding one
    // mid-typing would lose focus and the user's text, so those modes keep the
    // instant zoom.
    if (toolMode_ == ToolMode::FillForms)
        return false;
    if ((annotPopup_ && annotPopup_->isVisible())
        || (propertiesPopup_ && propertiesPopup_->isVisible())) {
        return false;
    }
    // An in-flight drag is steering by what is on screen; do not move it under the
    // cursor.
    return !selecting_ && !panning_ && measureDrag_ == MeasureDrag::None;
}

bool ViewerWidget::captureZoomEase(double newScale, ZoomAnimation::Snapshot *out) const
{
    if (!zoomEaseAllowed())
        return false;
    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());
    // Same widening as seedPreview: a zoom-out pulls pages in from outside the
    // view, and capturing where they sit NOW (off screen) is what lets them slide
    // in instead of popping into place. layout_ still holds the old scale here.
    const double ratio = std::clamp(layout_.scale() / std::max(newScale, 1e-6), 1.0, 8.0);
    const int dx = static_cast<int>(vpCanvas.width() * (ratio - 0.5));
    const int dy = static_cast<int>(vpCanvas.height() * (ratio - 0.5));
    for (int pg : layout_.pagesInViewport(vpCanvas.adjusted(-dx, -dy, dx, dy))) {
        const QRect pc = layout_.pageRect(pg);
        if (!pc.isValid())
            continue; // Single page mode lays out only the current page
        const QRectF shown(pc.translated(-off));
        // Retargeting: a zoom arriving mid-ease starts from what is ON SCREEN, not
        // from the state (already at the previous target), so a burst of wheel
        // notches reads as one continuous accelerating movement instead of a
        // sequence of restarts.
        out->from.insert(pg, zoomEase_.active() ? zoomEaseRect(pg, shown) : shown);
    }
    if (out->from.isEmpty())
        return false;
    for (auto it = out->from.cbegin(); it != out->from.cend(); ++it) {
        if (out->repPage < 0
            || std::abs(it.key() - currentPage_) < std::abs(out->repPage - currentPage_)) {
            out->repPage = it.key();
        }
    }
    return true;
}

void ViewerWidget::startZoomEase(ZoomAnimation::Snapshot &&e)
{
    const QRect repFinal = layout_.pageRect(e.repPage);
    if (!zoomEase_.start(std::move(e), repFinal, zoomEaseMs_))
        return;
    zoomEaseTimer_->start();
    viewport()->update();
}

void ViewerWidget::endZoomEase()
{
    if (!zoomEase_.active())
        return;
    zoomEase_.reset();
    zoomEaseTimer_->stop();
    // Restore the invariant onResultReady holds while idle - a tile lives only
    // until its page's sharp render is in. Mid-ease that erase is skipped (the
    // page is still being drawn stretched), so catch up here.
    if (doc_ && pageCount() > 0 && !preview_.isEmpty()) {
        const QRect vpCanvas(contentOffset(), viewport()->size());
        for (int pg : layout_.pagesInViewport(vpCanvas)) {
            const QRect pr = layout_.pageRect(pg);
            const PageCache::Entry *e = cache_.get(pg);
            if (e && pr.isValid() && e->covered.contains(pr.intersected(vpCanvas)))
                preview_.erase(pg);
        }
    }
    viewport()->update(); // one plain frame at the final geometry
}

void ViewerWidget::onZoomEaseTick()
{
    if (!zoomEase_.active()) {
        zoomEaseTimer_->stop();
        return;
    }
    // Progress comes from the clock, never from a tick count: a slow frame
    // shortens the remaining path instead of stretching the gesture, and a stall
    // (or a cancel site we missed) self-heals on the next tick.
    if (zoomEase_.finished()) {
        endZoomEase();
        return;
    }
    viewport()->update();
}

QRectF ViewerWidget::zoomEaseRect(int pageNo, const QRectF &finalRect) const
{
    const QRectF representative = layout_.pageRect(zoomEase_.representativePage())
                                      .translated(-contentOffset());
    return zoomEase_.rect(pageNo, finalRect, representative);
}

void ViewerWidget::addPagesHeldByEase(std::vector<int> *pages) const
{
    const QRectF vp(QPointF(0, 0), QSizeF(viewport()->size()));
    const QPoint off = contentOffset();
    for (auto it = zoomEase_.capturedRects().cbegin(); it != zoomEase_.capturedRects().cend(); ++it) {
        if (std::find(pages->begin(), pages->end(), it.key()) != pages->end())
            continue;
        const QRect pc = layout_.pageRect(it.key());
        if (!pc.isValid())
            continue;
        if (zoomEaseRect(it.key(), QRectF(pc.translated(-off))).intersects(vp))
            pages->push_back(it.key()); // on screen only because the ease is running
    }
}

void ViewerWidget::setZoomEaseMs(int ms)
{
    zoomEaseMs_ = std::max(0, ms);
    if (zoomEaseMs_ == 0)
        endZoomEase();
}

void ViewerWidget::setZoomEaseProgressForTest(double t)
{
    zoomEase_.setProgressForTest(t); // < 0 hands progress back to the clock
    viewport()->update();
}

void ViewerWidget::ensureRendered(int pageNo, const QRect &neededCanvas)
{
    const QRect pr = layout_.pageRect(pageNo);
    if (!pr.isValid())
        return;

    // Whole page vs. clipped tile, decided by the page's full device-pixel area.
    const double devArea = (pr.width() * dpr_) * (pr.height() * dpr_);
    const bool clipped = devArea > kMaxWholePagePx;

    QRect region; // canvas-space region we will ask the engine to render
    if (!clipped) {
        region = pr;
    } else {
        region = neededCanvas.adjusted(-kClipMargin, -kClipMargin, kClipMargin, kClipMargin)
                     .intersected(pr);
        if (region.isEmpty())
            return;
    }

    // Already covered by the cached image, or by a request still in flight?
    if (const PageCache::Entry *e = cache_.get(pageNo); e && e->covered.contains(neededCanvas))
        return;
    if (auto it = pending_.constFind(pageNo);
        it != pending_.constEnd() && it.value().region.contains(neededCanvas))
        return;

    RenderRequest req;
    req.document = doc_;
    req.requester = viewerId_;
    req.pageNo = pageNo;
    req.scale = scale_ * dpr_; // render at device pixels for crisp output
    req.rotation = rotation_;
    req.epoch = viewEpoch_;
    req.token = nextToken_++;
    req.theme = pageTheme_; // Comfort is applied by the worker; Inverted below
    if (clipped) {
        // Clip is in device px relative to the page image's top-left.
        req.clip = QRect(qRound((region.x() - pr.x()) * dpr_),
                         qRound((region.y() - pr.y()) * dpr_),
                         qRound(region.width() * dpr_), qRound(region.height() * dpr_));
    }
    pending_[pageNo] = PendingRender{req.token, region};
    engine_->submit(req);
}

void ViewerWidget::onResultReady(const mervin::RenderResult &result)
{
    if (result.requester != viewerId_)
        return; // broadcast result belongs to a different viewer/document

    // Accept only the most-recent request we issued for this page; an older,
    // superseded result (e.g. from a region we have since scrolled past) is
    // dropped. invalidateRenders() empties pending_, so a result from a stale
    // epoch no longer matches and is ignored here too.
    auto it = pending_.find(result.pageNo);
    if (it == pending_.end() || it.value().token != result.token)
        return;
    const QRect covered = it.value().region;
    pending_.erase(it);

    if (result.epoch != viewEpoch_)
        return; // scale/rotation changed since this was requested
    if (!result.ok || result.image.isNull())
        return;

    QImage img = result.image;
    if (pageTheme_ == PageTheme::Inverted)
        img.invertPixels();
    img.setDevicePixelRatio(dpr_);
    cache_.put(result.pageNo, img, covered);
    if (zoomEase_.active()) {
        // Map fresh tiles into the eased page rectangle so they sharpen in place during animation.
        if (covered == layout_.pageRect(result.pageNo) || !preview_.tile(result.pageNo))
            preview_.add(result.pageNo, img, covered, layout_.pageRect(result.pageNo));
        viewport()->update(); // partial damage is meaningless while the page moves
    } else {
        // The sharp image is in, so the stretched stand-in has done its job. A tile
        // therefore lives from the zoom that froze it until the page renders again -
        // long enough to cover the gap, and no longer.
        preview_.erase(result.pageNo);
        viewport()->update(covered.translated(-contentOffset()));
    }
}

void ViewerWidget::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    // Canvas backdrop behind the pages: a dark slate in both themes, a step
    // lighter under the dark chrome (theme::Chrome::canvas).
    p.fillRect(event->rect(), theme::chrome(palette()).canvas);
    if (!doc_)
        return;

    const QPoint off = contentOffset();
    const QRect vpCanvas(off, viewport()->size());

    const bool haveSel = selection_.hasSelection();
    const TextPos selStart = haveSel ? selection_.start() : TextPos{};
    const TextPos selEnd = haveSel ? selection_.end() : TextPos{};

    // Placeholder / backing colour for not-yet-rendered page area: match the
    // active page theme's paper so pages do not flash white while re-rendering
    // in a dark document theme.
    const QColor pageBase = pagePaper();

    // While the zoom ease runs, pages are drawn at an interpolated size, so pages
    // outside the final viewport can still be on screen. They are painted but
    // neither counted nor rendered - they exist for this flight only.
    std::vector<int> pages = layout_.pagesInViewport(vpCanvas);
    const std::size_t liveCount = pages.size();
    if (zoomEase_.active())
        addPagesHeldByEase(&pages);

    for (std::size_t idx = 0; idx < pages.size(); ++idx) {
        const int i = pages[idx];
        const bool live = idx < liveCount; // in the FINAL viewport
        const QRect pageCanvas = layout_.pageRect(i);
        if (!pageCanvas.isValid())
            continue;
        const QRect r = pageCanvas.translated(-off);
        // Where this page is drawn this frame. Equals r whenever no ease is running.
        const QRect rDraw = zoomEase_.active() ? zoomEaseRect(i, QRectF(r)).toAlignedRect() : r;
        const QRect easedCanvas = rDraw.translated(off);
        const QRect neededCanvas = pageCanvas.intersected(vpCanvas);
        const PageCache::Entry *e = cache_.get(i);
        // Mid-ease a landed render is re-tiled into preview_ (see onResultReady),
        // so the drawing always goes through the cheap stretched path and the page
        // sharpens in place instead of snapping to its final size.
        if (!zoomEase_.active() && e && e->covered.contains(neededCanvas)) {
            // A clipped tile's realized image can be a sub-pixel short of the
            // region it claims (the layout and the renderer round the page bound
            // differently), so back it with page-white; otherwise the far-edge
            // sliver would show the dark viewport background. Whole-page tiles
            // (covered == pageCanvas) fully cover their rect and skip this.
            if (e->covered != pageCanvas)
                p.fillRect(QRect(e->covered.topLeft() - off, e->covered.size()), pageBase);
            p.drawImage(e->covered.topLeft() - off, e->image);
            ++paintStats_.fresh;
        } else {
            // Use the frozen preview for uncovered areas, then overlay any available sharp tile.
            p.fillRect(rDraw, pageBase);
            bool drew = drawPreview(p, i, pageCanvas, off, easedCanvas);
            if (!zoomEase_.active() && e && !e->image.isNull()) {
                p.drawImage(e->covered.topLeft() - off, e->image);
                drew = true;
            }
            if (live) {
                ++(drew ? paintStats_.preview : paintStats_.blank);
                ensureRendered(i, neededCanvas); // at the final scale, as always
            }
        }
        // Everything from here on is positioned from layout_, i.e. at the page's
        // FINAL geometry. Mid-ease the transform carries it onto the eased rect, so
        // the border and every overlay travel with the bitmap as one object.
        std::optional<PainterStateGuard> easeGuard;
        if (zoomEase_.active()) {
            ++paintStats_.eased;
            easeGuard.emplace(p);
            p.setTransform(ZoomAnimation::transform(QRectF(r), QRectF(rDraw)), /*combine=*/true);
        }
        p.setPen(theme::doc().pageBorder);
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));

        // Measurement overlays (drawn even when there is no text index). Hidden
        // while the tool is disabled - the measurements are kept, just not shown.
        if (measureToolEnabled_)
            drawMeasurements(p, i);

        // Form-field highlight tint (drawn over fillable rects in form mode; does
        // not need a text index).
        if (toolMode_ == ToolMode::FillForms)
            drawFormHighlights(p, i);

        // Outline the annotation whose inline editor is open (annotations
        // themselves are baked into the page image by MuPDF). Independent of text.
        drawAnnotSelection(p, i);

        if (!textIndex_)
            continue;

        // Find highlights (current match drawn in a stronger colour).
        if (!matches_.empty()) {
            const auto it = matchesByPage_.constFind(i);
            if (it != matchesByPage_.constEnd()) {
                p.setPen(Qt::NoPen);
                for (int mi : it.value()) {
                    const TextMatch &m = matches_[mi];
                    p.setBrush(mi == currentMatch_ ? kCurrentMatchColor : kMatchColor);
                    for (const QRectF &pr : textIndex_->rangeRects(m.page, m.start, m.length))
                        p.drawRect(pageRectToWidget(i, pr));
                }
            }
        }

        // Text selection.
        if (haveSel && i >= selStart.page && i <= selEnd.page) {
            const int s = (i == selStart.page) ? selStart.offset : 0;
            const int e = (i == selEnd.page) ? selEnd.offset : textIndex_->pageTextLength(i);
            if (e > s) {
                p.setPen(Qt::NoPen);
                p.setBrush(kSelectionColor);
                for (const QRectF &pr : textIndex_->rangeRects(i, s, e - s))
                    p.drawRect(pageRectToWidget(i, pr));
            }
        }
    }

    // Snap target marker (drawn last so it sits above the page + overlays).
    drawSnapIndicator(p);

    // Keep previews near the viewport so scrolling cannot retain obsolete page images indefinitely.
    if (!zoomEase_.active() && !preview_.isEmpty()) {
        const QRect nearby = vpCanvas.adjusted(-vpCanvas.width(), -vpCanvas.height(),
                                               vpCanvas.width(), vpCanvas.height());
        preview_.retain(layout_.pagesInViewport(nearby));
    }
}

void ViewerWidget::hideEvent(QHideEvent *event)
{
    QAbstractScrollArea::hideEvent(event);
    endZoomEase(); // before the clear below: the ease draws those tiles
    // A tab switched away from should not sit on frozen images; showEvent
    // re-renders what is visible anyway.
    preview_.clear();
}

void ViewerWidget::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    // The captured rects are viewport-space, so a resize invalidates them - and the
    // re-fit below writes scale_ directly rather than going through rescaleKeeping.
    endZoomEase();
    // A re-fit can change the scroll range and clamp the value; while a resume is
    // pending, shield that from scrollContentsBy and re-anchor at the new size.
    const bool wasRestoring = restoring_;
    if (pendingRestore_)
        restoring_ = true;
    if (zoomMode_ != ZoomMode::Custom) {
        // Only invalidate rendered images when the fitted scale changes.
        const double previous = scale_;
        applyFitScale();
        if (!qFuzzyCompare(previous, scale_) || dpr_ != devicePixelRatioF()) {
            invalidateRenders(PreviewPolicy::Keep);
            relayout();
            emit scaleChanged(scale_);
        } else {
            updateScrollBars(); // ranges still track the new viewport size
        }
    } else {
        updateScrollBars();
    }
    if (pendingRestore_)
        applyPendingRestore();
    restoring_ = wasRestoring;

    // Reposition inline editors after every resize, including centering changes at the same scale.
    if (toolMode_ == ToolMode::FillForms)
        syncFormEditors();
    if (annotPopup_ && annotPopup_->isVisible())
        syncAnnotPopup();
    if (propertiesPopup_ && propertiesPopup_->isVisible())
        syncPropertiesPopup();
}

void ViewerWidget::showEvent(QShowEvent *event)
{
    QAbstractScrollArea::showEvent(event);
    endZoomEase(); // hideEvent dropped the tiles; the re-fit below starts clean
    if (!doc_)
        return;
    // A tab may have been laid out / fit-scaled while hidden (e.g. opened in the
    // background). Re-fit on show, and always trigger a paint so visible pages
    // that are not yet cached get requested.
    const bool wasRestoring = restoring_;
    if (pendingRestore_)
        restoring_ = true; // shield the re-fit's clamp from cancelling the resume
    if (zoomMode_ != ZoomMode::Custom) {
        const double previous = scale_;
        applyFitScale();
        // Also re-render when the device pixel ratio moved while the tab was
        // hidden (window dragged to another monitor) - dpr_ only refreshes in
        // relayout(), and the cached images carry the old ratio.
        if (!qFuzzyCompare(previous, scale_) || dpr_ != devicePixelRatioF()) {
            invalidateRenders(PreviewPolicy::Keep);
            relayout();
            emit scaleChanged(scale_);
        }
    }
    updateScrollBars();
    if (pendingRestore_)
        applyPendingRestore(); // a tab shown for the first time anchors here
    if (toolMode_ == ToolMode::FillForms)
        syncFormEditors(); // build editors now the viewport finally has a real size
    viewport()->update();
    restoring_ = wasRestoring;
}

void ViewerWidget::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if ((event->modifiers() & Qt::ControlModifier) && delta != 0) {
        // The wheel walks the same ladder as the buttons, one rung per notch. The
        // deltas are accumulated because high-resolution wheels and trackpads report
        // a fraction of a notch per event: without this they would jump a whole rung
        // on every micro-tick. A change of direction starts the notch afresh, so
        // reversing always moves on the first event that completes a notch.
        if ((delta > 0) != (wheelZoomAccum_ > 0))
            wheelZoomAccum_ = 0;
        wheelZoomAccum_ += delta;
        const int dir = delta > 0 ? 1 : -1;
        double target = scale_;
        while (std::abs(wheelZoomAccum_) >= kWheelNotch) {
            wheelZoomAccum_ -= dir * kWheelNotch;
            target = nextZoomLevel(target, dir); // a fast flick can cross several
        }
        if (!qFuzzyCompare(target, scale_))
            zoomAtViewportPos(target, event->position()); // zoom toward the cursor
        event->accept();
        return;
    }
    // Shift+wheel pans the page horizontally (left/right), driving the
    // horizontal bar exactly like the vertical one below. A normal wheel reports
    // on the y axis, but some platforms move the delta to x while Shift is held,
    // so take whichever axis carries it (y first) - the pan then works
    // regardless of which axis Qt populated.
    if (event->modifiers() & Qt::ShiftModifier) {
        const QPoint ad = event->angleDelta();
        const int hdelta = ad.y() != 0 ? ad.y() : ad.x();
        if (hdelta != 0) {
            QScrollBar *bar = horizontalScrollBar();
            bar->setValue(bar->value() - hdelta);
            event->accept();
            return;
        }
    }
    // Default vertical scrolling.
    if (delta != 0) {
        QScrollBar *bar = verticalScrollBar();
        bar->setValue(bar->value() - delta);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

bool ViewerWidget::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave)
        clearLinkToolTip();

    // Trackpad pinch arrives as a native zoom gesture on the viewport. value()
    // is the incremental zoom (e.g. +0.1 = grow 10%); anchor it on the cursor.
    if (event->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture && doc_) {
            // A pinch is continuous by nature: it needs no ease, and easing it
            // would put the picture behind the fingers.
            zoomEaseSuppress_ = true;
            zoomAtViewportPos(scale_ * (1.0 + g->value()), g->position());
            zoomEaseSuppress_ = false;
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void ViewerWidget::scrollContentsBy(int, int)
{
    // A scroll the zoom ease did not cause is the user (or goToPage / find / a
    // resume) taking over: land the picture on the real geometry at once rather
    // than gliding toward a target that has moved. rescaleKeeping's own scroll
    // writes happen before it arms the ease, so they never reach this.
    endZoomEase();
    // A scroll we did not initiate (wheel, keyboard, scrollbar drag, pan) is the
    // user taking over - drop the resume anchor so it stops re-snapping.
    if (pendingRestore_ && !restoring_)
        pendingRestore_ = false;
    updateCurrentPage();
    if (toolMode_ == ToolMode::FillForms)
        syncFormEditors(); // keep inline editors anchored to their fields
    if (annotPopup_ && annotPopup_->isVisible())
        syncAnnotPopup(); // keep the open annotation editor anchored
    if (propertiesPopup_ && propertiesPopup_->isVisible())
        syncPropertiesPopup(); // keep the open properties popup anchored
    clearLinkToolTip();
    viewport()->update();
}

void ViewerWidget::keyPressEvent(QKeyEvent *event)
{
    // Esc closes an open inline annotation editor in ANY mode (the comments
    // sidebar can open it while no annotation tool is active).
    if (event->key() == Qt::Key_Escape && propertiesPopup_ && propertiesPopup_->isVisible()) {
        closePropertiesPopup();
        viewport()->update();
        return;
    }
    if (event->key() == Qt::Key_Escape && annotPopup_ && annotPopup_->isVisible()) {
        closeAnnotPopup();
        viewport()->update();
        return;
    }
    if (annotationMode() && event->key() == Qt::Key_Escape) {
        // No popup open: Esc drops the annotation gesture back to Select (pointer),
        // leaving the Comment panel docked. (Close it from its X / the toolbar.)
        setAnnotSubMode(AnnotSubMode::Select);
        return;
    }
    if (toolMode_ == ToolMode::FillForms) {
        // Tab / Shift+Tab are consumed by focusNextPrevChild before reaching here;
        // this handles Esc (leave the tool) and Space/Enter on a Tab-focused toggle.
        switch (event->key()) {
        case Qt::Key_Escape:
            setFormMode(false); // leave form-fill, back to the normal pointer
            return;
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (formModel_ && formFocusIndex_ >= 0
                && formFocusIndex_ < static_cast<int>(formFieldOrder_.size())) {
                const auto [pg, fi] = formFieldOrder_[formFocusIndex_];
                const std::vector<FormField> &fields = formModel_->pageFields(pg);
                if (fi >= 0 && fi < static_cast<int>(fields.size()) && fields[fi].isToggle()) {
                    if (formModel_->toggle(pg, fi)) {
                        applyFormFieldChange(pg);
                        emit formEditsChanged();
                    }
                    return;
                }
            }
            break;
        default:
            break;
        }
    }
    if (measureMode()) {
        switch (event->key()) {
        case Qt::Key_Escape:
            // Revert to the standard pointer; this also cancels any unfinished
            // vector and repaints. The tool stays enabled (panel + measurements
            // remain) - closing it is done only via the panel's X or the Measure
            // menu toggle. (Reachable only with the crosshair active, so
            // setMeasureCursorActive(false) always runs its full body here.)
            setMeasureCursorActive(false);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (toolMode_ == ToolMode::Measure)
                finishPolyOrArea();
            return;
        case Qt::Key_Backspace:
            if (!inProgress_.empty()) {
                inProgress_.pop_back();
                if (inProgress_.empty())
                    inProgressPage_ = -1;
                emit measurementReadout(
                    formatMeasurement(inProgressPage_, measureKind_, previewPts()));
                viewport()->update();
            }
            return;
        default:
            break;
        }
    }
    switch (event->key()) {
    case Qt::Key_Home:
        // Bare Home toggles Fit Width/Page via the window-level fit shortcut, so it
        // is intercepted before it reaches here; Ctrl+Home jumps to the first page.
        if (event->modifiers() & Qt::ControlModifier) {
            goToPage(0);
            return;
        }
        QAbstractScrollArea::keyPressEvent(event);
        return;
    case Qt::Key_End:
        goToPage(pageCount() - 1); // End / Ctrl+End -> last page
        return;
    case Qt::Key_Down:
    case Qt::Key_PageDown:
        // In single-row mode, page keys move to the adjacent row once the current row reaches its scroll limit.
        if (layoutMode_.scroll == ViewLayout::Scroll::Single) {
            QScrollBar *bar = verticalScrollBar();
            if (bar->value() >= bar->maximum()) {
                nextPage(); // row-aware in Single, and a no-op on the last row
                return;
            }
        }
        QAbstractScrollArea::keyPressEvent(event);
        return;
    case Qt::Key_Up:
    case Qt::Key_PageUp:
        if (layoutMode_.scroll == ViewLayout::Scroll::Single) {
            QScrollBar *bar = verticalScrollBar();
            if (bar->value() <= bar->minimum()) {
                if (layout_.rowStart(currentPage_) > 0) {
                    prevPage(); // row-aware in Single
                    // Arriving from below: land at the bottom of the previous
                    // row (goToPage resets to the top, which reads wrong when
                    // paging backwards through zoomed-in pages).
                    bar->setValue(bar->maximum());
                }
                return;
            }
        }
        QAbstractScrollArea::keyPressEvent(event);
        return;
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

void ViewerWidget::updateCurrentPage()
{
    if (!doc_ || layoutMode_.scroll == ViewLayout::Scroll::Single)
        return; // single mode: the current page is fixed until next/prev
    if (navigating_)
        return; // goToPage owns currentPage_ across its own scrollbar writes
    // contentOffset(), not the scrollbar value: the canvas is centred when it is
    // smaller than the viewport, and ignoring that centreDelta biases the probe
    // down the document by half the slack - enough to report the last page while
    // the whole document is on screen.
    const int centerY = contentOffset().y() + viewport()->height() / 2;
    const int row = layout_.pageAtY(centerY);
    if (row < 0)
        return;
    // pageAtY reports row leaders, so in a spread it would drag the current page
    // back to the left-hand sheet after the reader navigated to the right-hand
    // one. Any page of the row the probe found counts as still being there.
    if (currentPage_ >= row && currentPage_ < layout_.rowEnd(row))
        return;
    currentPage_ = row;
    emit pageChanged(row + 1, pageCount());
}

void ViewerWidget::setZoomMode(ZoomMode mode)
{
    zoomMode_ = mode;
    // applyFitScale() writes the fitted value straight into scale_, but the anchor
    // has to be read at the OLD scale - so take the fitted value and put the old
    // one back for rescaleKeeping to apply.
    const double previous = scale_;
    if (mode != ZoomMode::Custom)
        applyFitScale();
    const double fitted = scale_;
    scale_ = previous;
    // A fit mode is chosen from the toolbar or the menu, so it holds the middle of
    // the view. Without that the raw scroll value survives into a layout of a
    // different height, which after a deep zoom lands the reader on a different
    // page entirely.
    rescaleKeeping(fitted, viewportCenter(), true);
    emit zoomModeChanged(zoomMode_);
    emit scaleChanged(scale_);
}

void ViewerWidget::setScale(double scale)
{
    const double target = clampScale(scale);
    zoomMode_ = ZoomMode::Custom;
    // The zoom driven by the toolbar buttons, the menu, the keyboard and the zoom
    // box: no cursor is involved, so the spot the user is reading - the middle of
    // the view - is the one to keep still. (Wheel and pinch zoom come through
    // zoomAtViewportPos with the cursor instead.)
    if (doc_ && pageCount() > 0 && !qFuzzyCompare(target, scale_))
        rescaleKeeping(target, viewportCenter(), true);
    else
        scale_ = target; // no document, or already at that scale
    emit zoomModeChanged(zoomMode_);
    emit scaleChanged(scale_);
}

QPointF ViewerWidget::viewportCenter() const
{
    return QRectF(QPointF(0, 0), QSizeF(viewport()->size())).center();
}

void ViewerWidget::zoomAtViewportPos(double newScale, QPointF viewportPos)
{
    if (!doc_ || pageCount() == 0)
        return;
    newScale = clampScale(newScale);
    if (qFuzzyCompare(newScale, scale_))
        return;
    zoomMode_ = ZoomMode::Custom;
    rescaleKeeping(newScale, viewportPos);
    emit zoomModeChanged(zoomMode_);
    emit scaleChanged(scale_);
}

void ViewerWidget::rescaleKeeping(double newScale, QPointF viewportPos, bool keepCenter)
{
    // Snapshot where the pages are DRAWN right now, before anything moves, then
    // arm the ease as the very LAST statement. That ordering is load-bearing: the
    // scrollbar writes below (and relayout's range clamp) both land in
    // scrollContentsBy, which ends an ease on the rule that a scroll the ease did
    // not cause is the user taking over. Arming first would cancel it immediately.
    ZoomAnimation::Snapshot ease;
    const bool wantEase = captureZoomEase(newScale, &ease);
    endZoomEase(); // the snapshot has consumed any running one

    // Preserve the page point under the cursor or viewport center. Page coordinates avoid drift from fixed margins.
    const QPointF canvasBefore = viewportPos + QPointF(contentOffset());
    const int pg = (doc_ && pageCount() > 0) ? pageAtCanvas(canvasBefore.toPoint()) : -1;
    const QPointF anchorPage =
        (pg >= 0) ? canvasToPagePoint(pg, canvasBefore, /*clampToPage=*/false) : QPointF();

    scale_ = newScale;
    invalidateRenders(PreviewPolicy::Keep);
    relayout(); // recomputes the layout and scrollbar ranges at the new scale
    if (keepCenter)
        viewportPos = viewportCenter(); // scrollbar visibility can change the viewport size

    if (pg >= 0) {
        // Want: canvasAfter - contentOffset == viewportPos, and
        //       scrollOffset == contentOffset + centerDelta.
        const QPointF canvasAfter = pagePointToCanvas(pg, anchorPage);
        const QPointF target = canvasAfter - viewportPos + QPointF(centerDelta());
        horizontalScrollBar()->setValue(std::clamp(qRound(target.x()),
                                                   horizontalScrollBar()->minimum(),
                                                   horizontalScrollBar()->maximum()));
        verticalScrollBar()->setValue(std::clamp(qRound(target.y()),
                                                 verticalScrollBar()->minimum(),
                                                 verticalScrollBar()->maximum()));
    }

    updateCurrentPage();
    if (wantEase)
        startZoomEase(std::move(ease));
}

void ViewerWidget::zoomIn()
{
    setScale(nextZoomLevel(scale_, +1));
}

void ViewerWidget::zoomOut()
{
    setScale(nextZoomLevel(scale_, -1));
}

void ViewerWidget::rotateLeft()
{
    rotation_ = (rotation_ + 270) % 360;
    if (zoomMode_ != ZoomMode::Custom)
        applyFitScale();
    endZoomEase();                          // the eased rects belong to the old aspect
    invalidateRenders(PreviewPolicy::Drop); // the quarter turn flips the page's aspect
    relayout();
    goToPage(currentPage_);
    emit scaleChanged(scale_);
}

void ViewerWidget::rotateRight()
{
    rotation_ = (rotation_ + 90) % 360;
    if (zoomMode_ != ZoomMode::Custom)
        applyFitScale();
    endZoomEase();                          // the eased rects belong to the old aspect
    invalidateRenders(PreviewPolicy::Drop); // the quarter turn flips the page's aspect
    relayout();
    goToPage(currentPage_);
    emit scaleChanged(scale_);
}

void ViewerWidget::setRotation(int degrees)
{
    int r = ((degrees % 360) + 360) % 360; // normalize into [0, 360)
    r = (r / 90) * 90;                      // snap to a quarter turn
    if (r == rotation_)
        return;
    rotation_ = r;
    if (zoomMode_ != ZoomMode::Custom)
        applyFitScale();
    endZoomEase();                          // the eased rects belong to the old aspect
    invalidateRenders(PreviewPolicy::Drop); // the rotation flips the page's aspect
    relayout();
    goToPage(currentPage_);
    emit scaleChanged(scale_);
}

void ViewerWidget::setScrollMode(ViewLayout::Scroll scroll)
{
    ViewLayout::Mode m = layoutMode_;
    m.scroll = scroll;
    setLayoutMode(m);
}

void ViewerWidget::setSpread(bool on)
{
    ViewLayout::Mode m = layoutMode_;
    m.spread = on;
    setLayoutMode(m);
}

void ViewerWidget::setLayoutMode(ViewLayout::Mode mode)
{
    if (layoutMode_ == mode)
        return;
    layoutMode_ = mode;
    if (zoomMode_ != ZoomMode::Custom)
        applyFitScale();
    endZoomEase();
    invalidateRenders(PreviewPolicy::Keep); // same pages, only re-laid out
    relayout();
    goToPage(currentPage_);
    emit layoutModeChanged(mode);
    emit scaleChanged(scale_);
}

void ViewerWidget::setPageTheme(PageTheme theme)
{
    if (pageTheme_ == theme)
        return;
    pageTheme_ = theme;
    // Re-render so cached images are re-toned; the frozen previews carry the old
    // theme's pixels, so they go too rather than flashing the wrong colours.
    endZoomEase();
    invalidateRenders(PreviewPolicy::Drop);
    viewport()->update();
}

void ViewerWidget::goToPage(int pageNo)
{
    if (!doc_ || pageCount() == 0)
        return;
    // An explicit navigation (page box, outline, thumbnail, next/prev, Ctrl+Home/
    // End, find-via-scrollToMatch, zoom-via-setScale) abandons any pending resume
    // anchor, so a later settling resize can't snap the view back off this page.
    pendingRestore_ = false;
    pageNo = std::clamp(pageNo, 0, pageCount() - 1);
    // setValue() below reaches scrollContentsBy -> updateCurrentPage synchronously,
    // which used to overwrite currentPage_ with whatever the scroll position
    // implied and emit a second, disagreeing pageChanged. Own the field across the
    // scroll writes and assign it after them.
    navigating_ = true;
    // Whether the target page still has to be scrolled to horizontally after the
    // vertical move below. Single resets the bar to the row's left edge, which is
    // already where the row's FIRST page sits - so only a following sheet of a
    // spread can need anything more, and a lone page keeps landing at 0 exactly as
    // it always did.
    bool ensureTargetVisible = true;
    if (layoutMode_.scroll == ViewLayout::Scroll::Single) {
        layout_.setCurrentPage(pageNo); // relayouts to show only this page's row
        updateScrollBars();
        verticalScrollBar()->setValue(0);
        horizontalScrollBar()->setValue(0);
        ensureTargetVisible = (pageNo != layout_.rowStart(pageNo));
    } else {
        // Scroll to the top of the page's ROW, not of the page: in a spread whose
        // halves differ in height a shorter page starts below the row top, and
        // aiming at it would cut the head off its partner.
        const QRect r = layout_.pageRect(layout_.rowStart(pageNo));
        verticalScrollBar()->setValue(std::max(0, r.top() - 8));
    }
    if (ensureTargetVisible) {
        // Nothing ever moved the horizontal bar, so navigating to the right-hand
        // sheet of a spread while zoomed in left it off-screen. Scroll the least
        // that brings the target page into view, and otherwise stay put.
        const QRect target = layout_.pageRect(pageNo);
        QScrollBar *hb = horizontalScrollBar();
        if (target.isValid() && hb->maximum() > 0) {
            // pageRect is canvas-space and the bar is scroll-space, so convert.
            const int dx = centerDelta().x();
            const int lo = target.right() + 8 + dx - viewport()->width();
            const int hi = target.left() - 8 + dx;
            hb->setValue(std::clamp(hb->value(), std::min(lo, hi), std::max(lo, hi)));
        }
    }
    currentPage_ = pageNo;
    navigating_ = false;
    emit pageChanged(pageNo + 1, pageCount());
    // Re-anchor the inline overlay editors to the now-visible page(s). In Single
    // mode this relayouts via layout_.setCurrentPage() without going through
    // relayout(), and the scroll reset to 0 fires no scrollContentsBy when the bar
    // was already at 0 - so without this the editors would be left on the previous
    // page. Idempotent in the paths where a scroll change already synced.
    if (toolMode_ == ToolMode::FillForms)
        syncFormEditors();
    if (annotPopup_ && annotPopup_->isVisible())
        syncAnnotPopup();
    if (propertiesPopup_ && propertiesPopup_->isVisible())
        syncPropertiesPopup();
    viewport()->update();
}

// Next / Previous move by whatever unit is on screen. In Scroll::Single that is a
// ROW: with the spread on, stepping one page from the left-hand sheet would land
// on its facing partner, re-showing the spread already laid out - so the toolbar
// arrow would look dead every other click. In Continuous every page is reachable
// by scrolling, so a page really is the unit and stepping stays page-granular.
void ViewerWidget::nextPage()
{
    if (layoutMode_.scroll == ViewLayout::Scroll::Single) {
        const int next = layout_.rowEnd(currentPage_);
        goToPage(next < pageCount() ? next : currentPage_); // no next row: stay put
        return;
    }
    goToPage(currentPage_ + 1);
}

void ViewerWidget::prevPage()
{
    if (layoutMode_.scroll == ViewLayout::Scroll::Single) {
        const int here = layout_.rowStart(currentPage_);
        goToPage(here > 0 ? layout_.rowStart(here - 1) : here);
    } else {
        goToPage(currentPage_ - 1);
    }
}

ViewerWidget::ScrollAnchor ViewerWidget::scrollAnchor() const
{
    ScrollAnchor a;
    if (!doc_ || pageCount() == 0)
        return a;
    // Canvas point shown at the viewport's top-left corner. From contentOffset():
    // viewportPos = canvasPos - (scrollOffset - centerDelta), so the (0,0) corner
    // maps to scrollOffset - centerDelta in canvas space.
    const QPoint corner = scrollOffset() - centerDelta();
    // Anchor on the page UNDER the corner (not the centre page currentPage_ shows),
    // so the fraction is a genuine within-page value that scales cleanly. In Single
    // mode only the current page is laid out, so pageAtY returns it.
    a.page = std::clamp(layout_.pageAtY(corner.y()), 0, pageCount() - 1);
    const QRect pr = layout_.pageRect(a.page);
    if (pr.width() <= 0 || pr.height() <= 0)
        return a;
    a.fracX = (corner.x() - pr.left()) / double(pr.width());
    a.fracY = (corner.y() - pr.top()) / double(pr.height());
    return a;
}

void ViewerWidget::restorePageScrollFraction(int page, double fracX, double fracY)
{
    if (!doc_ || pageCount() == 0)
        return;
    pendingRestore_ = true;
    pendingPage_ = std::clamp(page, 0, pageCount() - 1);
    pendingFracX_ = fracX;
    pendingFracY_ = fracY;
    applyPendingRestore();
}

void ViewerWidget::applyPendingRestore()
{
    if (!pendingRestore_ || !doc_ || pageCount() == 0)
        return;
    // Our own scrollbar writes (and the relayout that precedes them) must not be
    // mistaken for a user scroll, which would cancel the pending restore.
    const bool wasRestoring = restoring_;
    restoring_ = true;

    currentPage_ = pendingPage_;
    if (layoutMode_.scroll == ViewLayout::Scroll::Single)
        layout_.setCurrentPage(currentPage_); // relayouts to show only this page
    updateScrollBars();

    const QRect pr = layout_.pageRect(currentPage_);
    if (pr.isValid()) {
        const QPoint cd = centerDelta();
        const int tx = qRound(pr.left() + pendingFracX_ * pr.width() + cd.x());
        const int ty = qRound(pr.top() + pendingFracY_ * pr.height() + cd.y());
        horizontalScrollBar()->setValue(
            std::clamp(tx, horizontalScrollBar()->minimum(), horizontalScrollBar()->maximum()));
        verticalScrollBar()->setValue(
            std::clamp(ty, verticalScrollBar()->minimum(), verticalScrollBar()->maximum()));
    }
    emit pageChanged(currentPage_ + 1, pageCount());
    viewport()->update();

    restoring_ = wasRestoring;
}

// ---- Coordinate mapping ----------------------------------------------------
// page-point space (TextIndex, 72 dpi, unrotated) <-> canvas/widget pixels.
// The mapping mirrors the render CTM fz_pre_rotate(fz_scale(s,s), rotation):
// a page point (x,y) lands within the page's displayed box (dW x dH).

QRectF ViewerWidget::pageRectToCanvas(int pageNo, const QRectF &pr) const
{
    const QRect box = layout_.pageRect(pageNo);
    if (!box.isValid())
        return {};
    const double s = scale_;
    const double dW = box.width();
    const double dH = box.height();
    auto map = [&](double x, double y) -> QPointF {
        switch (rotation_) {
        case 90:
            return {dW - s * y, s * x};
        case 180:
            return {dW - s * x, dH - s * y};
        case 270:
            return {s * y, dH - s * x};
        default:
            return {s * x, s * y};
        }
    };
    const QPointF a = map(pr.left(), pr.top());
    const QPointF b = map(pr.right(), pr.bottom());
    return QRectF(a, b).normalized().translated(box.topLeft());
}

QRectF ViewerWidget::pageRectToWidget(int pageNo, const QRectF &pr) const
{
    return pageRectToCanvas(pageNo, pr).translated(-contentOffset());
}

// Map a single page point to canvas (content) coordinates (mirrors
// pageRectToCanvas's rotation-aware map).
QPointF ViewerWidget::pagePointToCanvas(int pageNo, QPointF pp) const
{
    const QRect box = layout_.pageRect(pageNo);
    if (!box.isValid())
        return {};
    const double s = scale_;
    const double dW = box.width();
    const double dH = box.height();
    QPointF c;
    switch (rotation_) {
    case 90:
        c = {dW - s * pp.y(), s * pp.x()};
        break;
    case 180:
        c = {dW - s * pp.x(), dH - s * pp.y()};
        break;
    case 270:
        c = {s * pp.y(), dH - s * pp.x()};
        break;
    default:
        c = {s * pp.x(), s * pp.y()};
        break;
    }
    return c + QPointF(box.topLeft());
}

// Map a single page point to widget coordinates (canvas, shifted to the viewport).
QPointF ViewerWidget::pagePointToWidget(int pageNo, QPointF pp) const
{
    if (!layout_.pageRect(pageNo).isValid())
        return {};
    return pagePointToCanvas(pageNo, pp) - QPointF(contentOffset());
}

QPointF ViewerWidget::canvasToPagePoint(int pageNo, QPointF canvas, bool clampToPage) const
{
    const QRect box = layout_.pageRect(pageNo);
    const double s = (scale_ <= 0.0) ? 1.0 : scale_;
    const double dW = box.width();
    const double dH = box.height();
    const double lx = canvas.x() - box.left();
    const double ly = canvas.y() - box.top();
    double x = 0.0;
    double y = 0.0;
    switch (rotation_) {
    case 90:
        x = ly / s;
        y = (dW - lx) / s;
        break;
    case 180:
        x = (dW - lx) / s;
        y = (dH - ly) / s;
        break;
    case 270:
        x = (dH - ly) / s;
        y = lx / s;
        break;
    default:
        x = lx / s;
        y = ly / s;
        break;
    }
    if (clampToPage) {
        const QSizeF pt = doc_ ? doc_->pageSize(pageNo) : QSizeF(0, 0);
        x = std::clamp(x, 0.0, pt.width());
        y = std::clamp(y, 0.0, pt.height());
    }
    return {x, y};
}

int ViewerWidget::pageUnder(QPoint viewportPos) const
{
    const QPoint canvas = viewportPos + contentOffset();
    for (int i = 0; i < layout_.pageCount(); ++i) {
        const QRect r = layout_.pageRect(i);
        if (r.isValid() && r.contains(canvas))
            return i;
    }
    return -1;
}

int ViewerWidget::pageAtCanvas(QPoint canvas) const
{
    for (int i = 0; i < layout_.pageCount(); ++i) {
        const QRect r = layout_.pageRect(i);
        if (r.isValid() && r.contains(canvas))
            return i;
    }
    // Off-page: the nearest page by edge distance, not by vertical band. A band
    // test has no opinion about x, so a click in the blank strip beside the
    // shorter half of a spread used to land on its taller neighbour - and since
    // canvasToPagePoint clamps to the page, callers that draw (measure, comment,
    // OCR) silently committed to a point on the wrong sheet's edge.
    int best = -1;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    for (int i = 0; i < layout_.pageCount(); ++i) {
        const QRect r = layout_.pageRect(i);
        if (!r.isValid())
            continue;
        const qint64 dx = std::max({0, r.left() - canvas.x(), canvas.x() - r.right()});
        const qint64 dy = std::max({0, r.top() - canvas.y(), canvas.y() - r.bottom()});
        const qint64 d = dx * dx + dy * dy;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

TextPos ViewerWidget::posAt(QPoint viewportPos) const
{
    if (!doc_ || !textIndex_)
        return {};
    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0)
        return {};
    const QPointF pp = canvasToPagePoint(pg, canvas);
    return TextPos{pg, textIndex_->offsetAt(pg, pp)};
}

QString ViewerWidget::externalLinkAt(QPoint viewportPos) const
{
    if (const std::optional<PdfLinkTarget> pdfLink = pdfLinkAt(viewportPos);
        pdfLink && openableWebUrl(pdfLink->uri)) {
        return pdfLink->uri;
    }

    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0 || !layout_.pageRect(pg).contains(canvas))
        return {};

    if (textIndex_) {
        const QPointF pp = canvasToPagePoint(pg, canvas);
        const std::optional<TextLink> textLink = textIndex_->linkAt(pg, pp);
        if (textLink && openableWebUrl(textLink->url))
            return textLink->url;
    }
    return {};
}

void ViewerWidget::showLinkToolTip(const QString &url, QPoint globalPos)
{
    if (url.isEmpty()) {
        clearLinkToolTip();
        return;
    }

    if (hoveredLinkToolTip_ == url && QToolTip::isVisible())
        return;

    hoveredLinkToolTip_ = url;
    QToolTip::showText(globalPos, url, viewport());
}

void ViewerWidget::clearLinkToolTip()
{
    if (hoveredLinkToolTip_.isEmpty())
        return;

    hoveredLinkToolTip_.clear();
    QToolTip::hideText();
}

std::optional<PdfLinkTarget> ViewerWidget::pdfLinkAt(QPoint viewportPos) const
{
    if (!doc_)
        return std::nullopt;

    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0 || !layout_.pageRect(pg).contains(canvas))
        return std::nullopt;

    return doc_->linkTargetAt(pg, canvasToPagePoint(pg, canvas));
}

std::optional<PdfItemProperties> ViewerWidget::itemPropertiesAt(QPoint viewportPos) const
{
    if (!doc_)
        return std::nullopt;

    const QPoint canvas = viewportPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0 || !layout_.pageRect(pg).contains(canvas))
        return std::nullopt;

    return doc_->itemPropertiesAt(pg, canvasToPagePoint(pg, canvas));
}

bool ViewerWidget::activatePdfLink(const PdfLinkTarget &target)
{
    if (!target.valid())
        return false;
    if (target.isInternal()) {
        goToPage(target.page);
        return true;
    }
    return openExternalLink(target.uri);
}

bool ViewerWidget::openExternalLink(const QString &url)
{
    const std::optional<QUrl> webUrl = openableWebUrl(url);
    if (!webUrl)
        return false;
    return QDesktopServices::openUrl(*webUrl);
}

bool ViewerWidget::openItemProperties(QPoint viewportPos)
{
    const std::optional<PdfItemProperties> properties = itemPropertiesAt(viewportPos);
    if (!properties)
        return false;

    if (annotPopup_ && annotPopup_->isVisible())
        closeAnnotPopup();
    if (!propertiesPopup_) {
        propertiesPopup_ = new PdfPropertiesPopup(viewport());
        connect(propertiesPopup_, &PdfPropertiesPopup::dismissed, this,
                [this] { openProperties_.reset(); });
    }
    openProperties_ = *properties;
    propertiesPopup_->showFor(*properties);
    syncPropertiesPopup();
    setFocus();
    return true;
}

void ViewerWidget::closePropertiesPopup()
{
    openProperties_.reset();
    if (propertiesPopup_ && propertiesPopup_->isVisible())
        propertiesPopup_->hide();
}

void ViewerWidget::syncPropertiesPopup()
{
    if (!propertiesPopup_ || !propertiesPopup_->isVisible() || !openProperties_)
        return;
    propertiesPopup_->positionNear(pageRectToWidget(openProperties_->page, openProperties_->rect).toAlignedRect());
}

// ---- Find ------------------------------------------------------------------

void ViewerWidget::rebuildMatchIndex()
{
    matchesByPage_.clear();
    for (int i = 0; i < static_cast<int>(matches_.size()); ++i)
        matchesByPage_[matches_[i].page].append(i);
}

void ViewerWidget::startFind(const QString &query, bool caseSensitive, bool wholeWord)
{
    findQuery_ = query;
    findCaseSensitive_ = caseSensitive;
    findWholeWord_ = wholeWord;
    matches_.clear();
    matchesByPage_.clear();
    currentMatch_ = -1;

    documentSearch_->cancel();
    emit findStatusChanged(0, 0);
    viewport()->update();
    documentSearch_->start(doc_, query, caseSensitive, wholeWord,
                           [this](std::vector<TextMatch> matches) {
        matches_ = std::move(matches);
    rebuildMatchIndex();

    if (!matches_.empty()) {
        // Prefer the first match at or after the current page.
        int chosen = 0;
        for (int i = 0; i < static_cast<int>(matches_.size()); ++i) {
            if (matches_[i].page >= currentPage_) {
                chosen = i;
                break;
            }
        }
        currentMatch_ = chosen;
        scrollToMatch(currentMatch_);
    }

    emit findStatusChanged(currentMatch_ >= 0 ? currentMatch_ + 1 : 0,
                           static_cast<int>(matches_.size()));
    viewport()->update();
    });
}

void ViewerWidget::findNext()
{
    if (matches_.empty())
        return;
    currentMatch_ = (currentMatch_ + 1) % static_cast<int>(matches_.size());
    scrollToMatch(currentMatch_);
    emit findStatusChanged(currentMatch_ + 1, static_cast<int>(matches_.size()));
    viewport()->update();
}

void ViewerWidget::findPrev()
{
    if (matches_.empty())
        return;
    const int n = static_cast<int>(matches_.size());
    currentMatch_ = (currentMatch_ - 1 + n) % n;
    scrollToMatch(currentMatch_);
    emit findStatusChanged(currentMatch_ + 1, n);
    viewport()->update();
}

void ViewerWidget::clearFind()
{
    documentSearch_->cancel();
    findQuery_.clear();
    matches_.clear();
    matchesByPage_.clear();
    currentMatch_ = -1;
    emit findStatusChanged(0, 0);
    viewport()->update();
}

void ViewerWidget::scrollToMatch(int matchIndex)
{
    if (matchIndex < 0 || matchIndex >= static_cast<int>(matches_.size()) || !textIndex_)
        return;
    const TextMatch &m = matches_[matchIndex];

    // In single-page mode, lay out the match's page first.
    if (layoutMode_.scroll == ViewLayout::Scroll::Single && m.page != currentPage_)
        goToPage(m.page);

    const auto rects = textIndex_->rangeRects(m.page, m.start, m.length);
    if (rects.empty()) {
        const QRect box = layout_.pageRect(m.page);
        if (box.isValid())
            verticalScrollBar()->setValue(std::clamp(box.top() - 20, 0, verticalScrollBar()->maximum()));
        return;
    }
    QRectF canvasRect;
    for (const QRectF &pr : rects) {
        const QRectF c = pageRectToCanvas(m.page, pr);
        canvasRect = canvasRect.isNull() ? c : canvasRect.united(c);
    }
    ensureCanvasRectVisible(canvasRect);
    if (layoutMode_.scroll != ViewLayout::Scroll::Single)
        updateCurrentPage();
}

void ViewerWidget::ensureCanvasRectVisible(const QRectF &c)
{
    QScrollBar *vb = verticalScrollBar();
    QScrollBar *hb = horizontalScrollBar();
    const int vh = viewport()->height();
    const int vw = viewport()->width();

    // `c` is canvas-space and the bars are scroll-space; they differ by
    // centreDelta whenever the canvas is smaller than the viewport on that axis.
    // (Harmless before only by accident: an axis with slack has no scroll range,
    // so the misjudged target clamped back to 0 either way.)
    const QPoint off = contentOffset();
    if (c.top() < off.y() || c.bottom() > off.y() + vh) {
        const int target = static_cast<int>(c.center().y() - vh / 2.0) + centerDelta().y();
        vb->setValue(std::clamp(target, vb->minimum(), vb->maximum()));
    }
    if (c.left() < off.x() || c.right() > off.x() + vw) {
        const int target = static_cast<int>(c.center().x() - vw / 2.0) + centerDelta().x();
        hb->setValue(std::clamp(target, hb->minimum(), hb->maximum()));
    }
}

// ---- Selection / copy ------------------------------------------------------

QString ViewerWidget::selectedText() const
{
    if (!textIndex_ || !selection_.hasSelection())
        return {};
    const TextPos s = selection_.start();
    const TextPos e = selection_.end();
    QString out;
    for (int pg = s.page; pg <= e.page; ++pg) {
        const int from = (pg == s.page) ? s.offset : 0;
        const int to = (pg == e.page) ? e.offset : textIndex_->pageTextLength(pg);
        if (to > from)
            out += textIndex_->textRange(pg, from, to - from);
        if (pg != e.page)
            out += QLatin1Char('\n');
    }
    return out;
}

void ViewerWidget::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void ViewerWidget::selectAll()
{
    if (!textIndex_ || pageCount() == 0)
        return;
    const int last = pageCount() - 1;
    selection_.set(TextPos{0, 0}, TextPos{last, textIndex_->pageTextLength(last)});
    viewport()->update();
}

void ViewerWidget::clearSelection()
{
    if (!selection_.hasSelection())
        return;
    selection_.clear();
    viewport()->update();
}

// ---- Mouse -----------------------------------------------------------------

void ViewerWidget::setOcrMode(bool on)
{
    if (on) {
        if (!doc_)
            return;
        if (measureToolEnabled_)
            setMeasureMode(false); // measure and OCR are mutually exclusive
        if (toolMode_ == ToolMode::FillForms)
            setFormMode(false); // forms and OCR are mutually exclusive
        setCommentToolEnabled(false); // OCR is fully exclusive: close the Comment panel too
        toolMode_ = ToolMode::Ocr;
        selection_.clear();
        selecting_ = false;
        viewport()->setCursor(Qt::CrossCursor);
        viewport()->update();
    } else if (toolMode_ == ToolMode::Ocr) {
        toolMode_ = ToolMode::None;
        viewport()->setCursor(Qt::IBeamCursor);
        if (rubberBand_)
            rubberBand_->hide();
    }
}

// ─────────────────────────────── Form filling ──────────────────────────────

bool ViewerWidget::eventFilter(QObject *watched, QEvent *event)
{
    const int idx = editorIndexFor(watched);
    if (idx >= 0 && formModel_) {
        switch (event->type()) {
        case QEvent::FocusIn: {
            // Keep Tab continuity when a field is reached by a direct click.
            const std::vector<std::pair<int, int>> &order = formFieldOrder();
            for (int k = 0; k < static_cast<int>(order.size()); ++k)
                if (order[k].first == formEditors_[idx].page
                    && order[k].second == formEditors_[idx].fieldIndex) {
                    formFocusIndex_ = k;
                    break;
                }
            break;
        }
        case QEvent::FocusOut:
            commitFormEditor(idx);
            break;
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Tab:
                commitFormEditor(idx);
                advanceFormFocus(+1);
                return true;
            case Qt::Key_Backtab:
                commitFormEditor(idx);
                advanceFormFocus(-1);
                return true;
            case Qt::Key_Escape: {
                // Discard the in-progress edit: restore the model's stored value and
                // hand focus back to the page (the tool stays on).
                const std::vector<FormField> &fields =
                    formModel_->pageFields(formEditors_[idx].page);
                const int fi = formEditors_[idx].fieldIndex;
                const QString v =
                    (fi >= 0 && fi < static_cast<int>(fields.size())) ? fields[fi].value : QString();
                if (auto *le = qobject_cast<QLineEdit *>(formEditors_[idx].widget))
                    le->setText(v);
                else if (auto *te = qobject_cast<QPlainTextEdit *>(formEditors_[idx].widget))
                    te->setPlainText(v);
                setFocus();
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                // Enter commits + advances on a single-line field; in a multi-line
                // box it inserts a newline (fall through to the editor).
                if (qobject_cast<QLineEdit *>(formEditors_[idx].widget)) {
                    commitFormEditor(idx);
                    advanceFormFocus(+1);
                    return true;
                }
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

bool ViewerWidget::focusNextPrevChild(bool next)
{
    // In form mode Tab / Shift+Tab walk the fillable fields (incl. toggles) rather
    // than Qt's default child-focus chain.
    if (toolMode_ == ToolMode::FillForms) {
        advanceFormFocus(next ? +1 : -1);
        return true;
    }
    return QAbstractScrollArea::focusNextPrevChild(next);
}

void ViewerWidget::mousePressEvent(QMouseEvent *event)
{
    // Hit-testing (text position, links, form fields, measurement handles) reads
    // the final layout, so a press mid-flight would land on geometry that is not on
    // screen yet. Land the picture first.
    endZoomEase();

    if (event->button() == Qt::LeftButton) {
        pressedExternalLink_.clear();
        pressedPdfLink_.reset();
        pressedProperties_.reset();
    }

    // Middle-button drag pans the view, regardless of the active tool.
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        panStartViewportPos_ = event->pos();
        panStartScroll_ = scrollOffset();
        // A selection drag near a viewport edge may have started the auto-scroll
        // timer; stop it so it does not keep scrolling/extending under the pan.
        stopAutoScroll();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (commentMode() && event->button() == Qt::LeftButton) {
        // Swallow dropdown-dismiss replays / presses over a docked panel so they
        // don't drop a stray note (the measure panel's combos can replay onto the
        // page even while the Comment tool holds the gesture).
        if (swallowToolPress(event)) {
            event->accept();
            return;
        }
        // A press anywhere first dismisses an open inline editor (committing it).
        closeAnnotPopup();
        int pg = -1;
        int id = -1;
        if (annotAt(event->pos(), pg, id))
            openAnnotPopup(pg, id); // click an existing annotation to edit it
        else
            createCommentAt(event->pos()); // empty space: drop a new sticky note
        setFocus();
        event->accept();
        return;
    }
    if (highlightMode() && event->button() == Qt::LeftButton) {
        if (swallowToolPress(event)) { // same guard as comment mode
            event->accept();
            return;
        }
        closeAnnotPopup();
        // A drag selects text to mark up (handled on release); a plain click on an
        // existing annotation opens its editor (also decided on release).
        if (doc_ && textIndex_) {
            const TextPos p = posAt(event->pos());
            if (p.valid()) {
                selection_.begin(p);
                selecting_ = true;
            } else {
                selection_.clear();
            }
            setFocus();
            viewport()->update();
        }
        event->accept();
        return;
    }
    if (toolMode_ == ToolMode::FillForms && event->button() == Qt::LeftButton) {
        // A press anywhere first dismisses a comment opened in the read-only viewer
        // (comments stay clickable in form mode; see the annotation branch below).
        if (annotPopup_ && annotPopup_->isVisible())
            closeAnnotPopup();
        // Text / choice fields have their own editor widget on top of the viewport,
        // so clicks there never reach here. We handle check-box / radio toggles and
        // clicks on empty page area.
        int pg = -1;
        int fi = -1;
        if (formFieldAt(event->pos(), pg, fi)) {
            const std::vector<FormField> &fields = formModel_->pageFields(pg);
            if (fi >= 0 && fi < static_cast<int>(fields.size()) && fields[fi].isToggle()) {
                const std::vector<std::pair<int, int>> &order = formFieldOrder();
                for (int k = 0; k < static_cast<int>(order.size()); ++k)
                    if (order[k].first == pg && order[k].second == fi) {
                        formFocusIndex_ = k; // Tab continuity from the clicked toggle
                        break;
                    }
                if (formModel_->toggle(pg, fi)) {
                    applyFormFieldChange(pg);
                    emit formEditsChanged();
                }
                setFocus();
                viewport()->update();
                event->accept();
                return;
            }
        }
        // Outside an actual form control, Fill Forms retains the viewer's normal
        // text-selection gesture. Record link/property targets as well so a plain
        // click still activates them on release, while a drag selects their text.
        commitActiveFormEditor();
        pressedProperties_ = itemPropertiesAt(event->pos());
        pressedPdfLink_ = pdfLinkAt(event->pos());
        pressedExternalLink_ = pressedPdfLink_ ? QString() : externalLinkAt(event->pos());
        if (doc_ && textIndex_) {
            const TextPos p = posAt(event->pos());
            if (p.valid()) {
                selection_.begin(p);
                selecting_ = true;
                setFocus();
                viewport()->update();
                event->accept();
                return;
            }
        }
        // Not on a form field: a click on a comment (sticky note, or a mark that
        // carries comment text) opens it in the read-only viewer, so comments are
        // reachable without first switching to the Comment tool. Matches the
        // pointer/Select behaviour - editing still requires the Comment tool.
        if (openItemProperties(event->pos())) {
            event->accept();
            return;
        }
        if (const std::optional<PdfLinkTarget> pdfLink = pdfLinkAt(event->pos())) {
            activatePdfLink(*pdfLink);
            setFocus();
            event->accept();
            return;
        }
        const QString link = externalLinkAt(event->pos());
        if (!link.isEmpty()) {
            openExternalLink(link);
            setFocus();
            event->accept();
            return;
        }
        int apg = -1;
        int aid = -1;
        if (annotAt(event->pos(), apg, aid) && annotShowsReadOnly(apg, aid)) {
            openAnnotPopup(apg, aid, /*readOnly=*/true);
            setFocus();
            event->accept();
            return;
        }
        // Empty page area: commit & defocus any in-progress inline editor.
        setFocus();
        event->accept();
        return;
    }
    if (measureMode() && event->button() == Qt::LeftButton) {
        // The page is painted on viewport() and the floating panels are only
        // sibling children of it, so interacting with a panel can still reach this
        // handler; swallowToolPress() rejects dropdown-dismiss replays, open
        // popups, and presses over a docked panel.
        if (swallowToolPress(event)) {
            event->accept();
            return;
        }
        // While measuring (not calibrating) and not mid-creation, a press on a
        // committed vertex handle or value label starts a drag instead of placing
        // a new point. (Calibrate must not hijack a nearby committed measurement.)
        if (toolMode_ == ToolMode::Measure && inProgress_.empty()) {
            int mi = -1;
            int vi = -1;
            bool onLabel = false;
            if (measureHitTest(event->pos(), mi, vi, onLabel)) {
                measureDrag_ = onLabel ? MeasureDrag::Label : MeasureDrag::Vertex;
                dragMeasureIdx_ = mi;
                dragVertexIdx_ = vi;
                if (onLabel) {
                    const Measurement &m = measurements_[mi];
                    // Offset from the pill centre, mapped without page clamping,
                    // so the label stays under the cursor and can be parked in
                    // the margin.
                    const QPointF anchorPage = canvasToPagePoint(
                        m.page, labelRectFor(m).center() + QPointF(contentOffset()), false);
                    const QPointF cursorPage =
                        canvasToPagePoint(m.page, event->pos() + contentOffset(), false);
                    dragLabelGrabPage_ = anchorPage - cursorPage;
                }
                clearSnapState();
                viewport()->setCursor(Qt::ClosedHandCursor);
                event->accept();
                return;
            }
        }
        handleMeasureClick(event->pos());
        return;
    }
    if (toolMode_ == ToolMode::Ocr && event->button() == Qt::LeftButton) {
        ocrOrigin_ = event->pos();
        if (!rubberBand_)
            rubberBand_ = new QRubberBand(QRubberBand::Rectangle, viewport());
        rubberBand_->setGeometry(QRect(ocrOrigin_, QSize()));
        rubberBand_->show();
        return;
    }
    if (event->button() == Qt::LeftButton && doc_ && textIndex_) {
        // A page click also dismisses an inline annotation editor opened from the
        // comments sidebar while in the default pointer mode (mirrors the
        // highlight/comment press paths, which closeAnnotPopup() first).
        if (annotPopup_ && annotPopup_->isVisible())
            closeAnnotPopup();
        if (propertiesPopup_ && propertiesPopup_->isVisible())
            closePropertiesPopup();
        pressedProperties_ = itemPropertiesAt(event->pos());
        pressedPdfLink_ = pdfLinkAt(event->pos());
        pressedExternalLink_ = pressedPdfLink_ ? QString() : externalLinkAt(event->pos());
        const TextPos p = posAt(event->pos());
        if (p.valid()) {
            selection_.begin(p);
            selecting_ = true;
            setFocus();
        } else {
            selection_.clear();
            // In Select mode, clicks without text open comments read-only. Resolve text-
            // overlapping annotations on release so dragging still selects text.
            int pg = -1;
            int id = -1;
            if (annotAt(event->pos(), pg, id) && annotShowsReadOnly(pg, id))
                openAnnotPopup(pg, id, /*readOnly=*/true);
        }
        viewport()->update();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void ViewerWidget::mouseMoveEvent(QMouseEvent *event)
{
    // Middle-button pan: translate the scroll offset by the drag delta.
    if (panning_ && (event->buttons() & Qt::MiddleButton)) {
        clearLinkToolTip();
        const QPoint delta = event->pos() - panStartViewportPos_;
        horizontalScrollBar()->setValue(panStartScroll_.x() - delta.x());
        verticalScrollBar()->setValue(panStartScroll_.y() - delta.y());
        event->accept();
        return;
    }
    if (measureMode()) {
        clearLinkToolTip();
        const QPoint canvas = event->pos() + contentOffset();
        // Dragging a committed vertex (re-snaps) or its value label.
        if (measureDrag_ != MeasureDrag::None && (event->buttons() & Qt::LeftButton)) {
            if (dragMeasureIdx_ < 0 || dragMeasureIdx_ >= static_cast<int>(measurements_.size())) {
                measureDrag_ = MeasureDrag::None;
                return;
            }
            Measurement &m = measurements_[dragMeasureIdx_];
            if (measureDrag_ == MeasureDrag::Vertex && dragVertexIdx_ >= 0
                && dragVertexIdx_ < static_cast<int>(m.pts.size())) {
                m.pts[dragVertexIdx_] = snapPagePoint(m.page, canvasToPagePoint(m.page, canvas));
                emit measurementReadout(formatMeasurement(m.page, m.kind, m.pts)); // live; list on release
            } else if (measureDrag_ == MeasureDrag::Label) {
                m.labelPos = canvasToPagePoint(m.page, canvas, false) + dragLabelGrabPage_;
                m.hasLabelPos = true;
            }
            viewport()->update();
            return;
        }

        const int pg = pageAtCanvas(canvas);
        if (pg >= 0) {
            const QPointF raw = canvasToPagePoint(pg, canvas);
            updateHoverScale(pg, raw);
            // A multi-vertex measurement is locked to one page; don't show a snap
            // marker on a different page the cursor wanders onto (a click there is
            // rejected anyway).
            if (!inProgress_.empty() && pg != inProgressPage_) {
                if (snapValid_) {
                    clearSnapState();
                    viewport()->update();
                }
                return;
            }
            if (!inProgress_.empty() && pg == inProgressPage_) {
                hoverPagePoint_ = snapPagePoint(pg, raw); // also updates the snap marker
                hoverValid_ = true;
                emit measurementReadout(
                    formatMeasurement(inProgressPage_, measureKind_, previewPts()));
                viewport()->update();
            } else {
                // Idle hover: hint draggable handles/labels, and preview the snap
                // target for the next click (suppressed over a handle, where a
                // press would drag rather than place).
                int mi = -1;
                int vi = -1;
                bool onLabel = false;
                const bool overHandle = measureHitTest(event->pos(), mi, vi, onLabel);
                viewport()->setCursor(overHandle ? Qt::OpenHandCursor : Qt::CrossCursor);
                const bool snapWasShown = snapValid_;
                if (overHandle) {
                    if (snapValid_) {
                        clearSnapState();
                        viewport()->update();
                    }
                } else {
                    snapPagePoint(pg, raw); // updates the snap marker
                    if (snapValid_ || snapWasShown)
                        viewport()->update(); // snap marker appeared, moved, or cleared
                }
            }
        } else {
            // Off any page: reset the drag-hint cursor that an earlier hover over
            // a handle may have left set.
            viewport()->setCursor(Qt::CrossCursor);
            if (snapValid_) {
                clearSnapState();
                viewport()->update();
            }
        }
        return;
    }
    if (toolMode_ == ToolMode::Ocr && rubberBand_ && (event->buttons() & Qt::LeftButton)) {
        clearLinkToolTip();
        rubberBand_->setGeometry(QRect(ocrOrigin_, event->pos()).normalized());
        return;
    }
    if (selecting_ && (event->buttons() & Qt::LeftButton)) {
        clearLinkToolTip();
        lastMouseViewportPos_ = event->pos();
        const TextPos p = posAt(event->pos());
        if (p.valid())
            selection_.extendTo(p);
        maybeAutoScroll(event->pos());
        viewport()->update();
        return;
    }
    if (toolMode_ == ToolMode::None && doc_) {
        const QString externalLink = externalLinkAt(event->pos());
        const bool clickable = itemPropertiesAt(event->pos()).has_value()
                               || pdfLinkAt(event->pos()).has_value()
                               || !externalLink.isEmpty();
        viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::IBeamCursor);
        showLinkToolTip(externalLink, event->globalPosition().toPoint());
        return;
    }
    if (toolMode_ == ToolMode::FillForms && doc_) {
        int page = -1;
        int field = -1;
        const bool overField = formFieldAt(event->pos(), page, field);
        const QString externalLink = externalLinkAt(event->pos());
        const bool clickable = overField || itemPropertiesAt(event->pos()).has_value()
                               || pdfLinkAt(event->pos()).has_value()
                               || !externalLink.isEmpty();
        viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::IBeamCursor);
        showLinkToolTip(externalLink, event->globalPosition().toPoint());
        return;
    }
    clearLinkToolTip();
    QAbstractScrollArea::mouseMoveEvent(event);
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    // End a middle-button pan; restore the tool's idle cursor.
    if (event->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        if (toolMode_ == ToolMode::None) {
            viewport()->setCursor(Qt::IBeamCursor);
        } else if (measureMode() && inProgress_.empty()) {
            // Match the idle-hover cursor: OpenHand when resting over a handle.
            int mi = -1;
            int vi = -1;
            bool onLabel = false;
            const bool overHandle = measureHitTest(event->pos(), mi, vi, onLabel);
            viewport()->setCursor(overHandle ? Qt::OpenHandCursor : Qt::CrossCursor);
        } else {
            viewport()->setCursor(Qt::CrossCursor);
        }
        event->accept();
        return;
    }
    if (measureMode()) {
        if (measureDrag_ != MeasureDrag::None) {
            const bool wasVertex = (measureDrag_ == MeasureDrag::Vertex);
            measureDrag_ = MeasureDrag::None;
            dragMeasureIdx_ = -1;
            dragVertexIdx_ = -1;
            clearSnapState();
            viewport()->setCursor(Qt::CrossCursor);
            if (wasVertex)
                emitMeasurementsChanged(); // commit the edited value into the list
            viewport()->update();
        }
        // Measure clicks are handled on press; swallow releases so they don't
        // start a text selection.
        return;
    }
    if (toolMode_ == ToolMode::Ocr && event->button() == Qt::LeftButton) {
        const QRect bandVp = rubberBand_ ? rubberBand_->geometry() : QRect();
        setOcrMode(false); // one-shot: leave OCR mode after the drag

        if (bandVp.width() > 4 && bandVp.height() > 4) {
            // Viewport -> canvas (content) coordinates, then to a page rect.
            const QRect bandCanvas = bandVp.translated(contentOffset());
            const int pageNo = pageAtCanvas(bandCanvas.center());
            if (pageNo >= 0) {
                const QPointF a = canvasToPagePoint(pageNo, bandCanvas.topLeft());
                const QPointF b = canvasToPagePoint(pageNo, bandCanvas.bottomRight());
                const QRectF pageRect = QRectF(a, b).normalized();
                if (!pageRect.isEmpty())
                    emit ocrRegionSelected(pageNo, pageRect);
            }
        }
        return;
    }
    if (highlightMode() && event->button() == Qt::LeftButton) {
        selecting_ = false;
        stopAutoScroll();
        if (selection_.hasSelection()) {
            createHighlightFromSelection(); // drag selected text -> mark it up + edit
        } else {
            int pg = -1;
            int id = -1;
            if (annotAt(event->pos(), pg, id))
                openAnnotPopup(pg, id); // plain click on an existing mark -> edit it
        }
        viewport()->update();
        return;
    }
    if (event->button() == Qt::LeftButton && selecting_) {
        selecting_ = false;
        stopAutoScroll();
        if (!selection_.hasSelection()) {
            selection_.clear(); // a plain click (no drag) clears any selection
            const std::optional<PdfItemProperties> releaseProperties = itemPropertiesAt(event->pos());
            if (pressedProperties_ && releaseProperties
                && releaseProperties->page == pressedProperties_->page
                && releaseProperties->values == pressedProperties_->values
                && openItemProperties(event->pos())) {
                pressedProperties_.reset();
                pressedPdfLink_.reset();
                pressedExternalLink_.clear();
                viewport()->update();
                return;
            }
            const std::optional<PdfLinkTarget> releasePdfLink = pdfLinkAt(event->pos());
            if (pressedPdfLink_ && releasePdfLink && releasePdfLink->uri == pressedPdfLink_->uri
                && releasePdfLink->page == pressedPdfLink_->page && activatePdfLink(*pressedPdfLink_)) {
                pressedProperties_.reset();
                pressedPdfLink_.reset();
                pressedExternalLink_.clear();
                viewport()->update();
                return;
            }
            const QString releaseLink = externalLinkAt(event->pos());
            if (!pressedExternalLink_.isEmpty() && releaseLink == pressedExternalLink_
                && openExternalLink(pressedExternalLink_)) {
                pressedProperties_.reset();
                pressedPdfLink_.reset();
                pressedExternalLink_.clear();
                viewport()->update();
                return;
            }
            // A plain click (no drag) on a comment in the pointer/Select state opens
            // it in a read-only viewer; a drag selects text as usual.
            int pg = -1;
            int id = -1;
            if (annotAt(event->pos(), pg, id) && annotShowsReadOnly(pg, id))
                openAnnotPopup(pg, id, /*readOnly=*/true);
        }
        pressedProperties_.reset();
        pressedPdfLink_.reset();
        pressedExternalLink_.clear();
        viewport()->update();
        return;
    }
    if (event->button() == Qt::LeftButton && (pressedProperties_ || pressedPdfLink_)) {
        const std::optional<PdfItemProperties> releaseProperties = itemPropertiesAt(event->pos());
        if (pressedProperties_ && releaseProperties
            && releaseProperties->page == pressedProperties_->page
            && releaseProperties->values == pressedProperties_->values
            && openItemProperties(event->pos())) {
            pressedProperties_.reset();
            pressedPdfLink_.reset();
            pressedExternalLink_.clear();
            viewport()->update();
            return;
        }
        const std::optional<PdfLinkTarget> releasePdfLink = pdfLinkAt(event->pos());
        if (pressedPdfLink_ && releasePdfLink && releasePdfLink->uri == pressedPdfLink_->uri
            && releasePdfLink->page == pressedPdfLink_->page && activatePdfLink(*pressedPdfLink_)) {
            pressedProperties_.reset();
            pressedPdfLink_.reset();
            pressedExternalLink_.clear();
            viewport()->update();
            return;
        }
        pressedProperties_.reset();
        pressedPdfLink_.reset();
        pressedExternalLink_.clear();
        viewport()->update();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void ViewerWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (toolMode_ == ToolMode::Measure && event->button() == Qt::LeftButton) {
        finishPolyOrArea();
        return;
    }
    if (event->button() == Qt::LeftButton && doc_ && textIndex_) {
        const TextPos p = posAt(event->pos());
        if (p.valid()) {
            int s = 0;
            int e = 0;
            textIndex_->wordBoundsAt(p.page, p.offset, &s, &e);
            if (e > s) {
                selection_.set(TextPos{p.page, s}, TextPos{p.page, e});
                selecting_ = false;
                viewport()->update();
                return;
            }
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void ViewerWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!doc_) {
        QAbstractScrollArea::contextMenuEvent(event);
        return;
    }
    // Right-click on a committed measurement (a vertex handle or its value label)
    // offers to copy that measurement's value. Only while the tool is on, since
    // the overlays are otherwise hidden and not hit-testable.
    if (measureToolEnabled_) {
        int mi = -1;
        int vi = -1;
        bool onLabel = false;
        if (measureHitTest(event->pos(), mi, vi, onLabel)) {
            const QColor ink = theme::chrome(palette()).inkBody; // == Theme::iconInk
            QMenu menu(this);
            QAction *copyAct =
                menu.addAction(icons::glyph(icons::Glyph::Copy, ink), tr("Copy value"));
            menu.addSeparator();
            QAction *delAct = menu.addAction(icons::glyph(icons::Glyph::Delete, ink),
                                             tr("Delete measurement"));
            QAction *chosen = menu.exec(event->globalPos());
            if (chosen == copyAct)
                copyMeasurementValue(mi);
            else if (chosen == delAct)
                removeMeasurement(mi);
            event->accept();
            return;
        }
    }
    // Otherwise the owning window builds the menu from its shared actions (OCR /
    // rotate) so the entries carry the same shortcuts and icons as the menu bar.
    // The menu key has no pointer position, so it means the page on screen.
    const int page = event->reason() == QContextMenuEvent::Keyboard ? currentPage_
                                                                     : pageUnder(event->pos());
    emit contextMenuRequested(event->globalPos(), page);
    event->accept();
}

void ViewerWidget::maybeAutoScroll(QPoint vp)
{
    int dy = 0;
    if (vp.y() < kAutoScrollMargin)
        dy = vp.y() - kAutoScrollMargin;
    else if (vp.y() > viewport()->height() - kAutoScrollMargin)
        dy = vp.y() - (viewport()->height() - kAutoScrollMargin);

    if (dy == 0) {
        stopAutoScroll();
        return;
    }
    autoScrollDy_ = std::clamp(dy, -kAutoScrollMaxStep, kAutoScrollMaxStep);
    if (!autoScrollTimer_->isActive())
        autoScrollTimer_->start();
}

void ViewerWidget::stopAutoScroll()
{
    autoScrollDy_ = 0;
    if (autoScrollTimer_->isActive())
        autoScrollTimer_->stop();
}

void ViewerWidget::onAutoScroll()
{
    if (!selecting_ || autoScrollDy_ == 0) {
        stopAutoScroll();
        return;
    }
    QScrollBar *vb = verticalScrollBar();
    const int before = vb->value();
    vb->setValue(std::clamp(before + autoScrollDy_, vb->minimum(), vb->maximum()));
    if (vb->value() == before) {
        stopAutoScroll();
        return;
    }
    const TextPos p = posAt(lastMouseViewportPos_);
    if (p.valid())
        selection_.extendTo(p);
    viewport()->update();
}

} // namespace mervin
