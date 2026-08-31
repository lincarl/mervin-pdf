#include "ui/ViewerWidget.h"

#include "render/AnnotModel.h"
#include "render/ComfortTransform.h"
#include "render/Document.h"
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

// The zoom ladder: the levels the +/- buttons, Ctrl+= / Ctrl+- and the wheel step
// through. Round percentages, alternating x1.5 and x1.33 so the series doubles
// every two rungs - about twice the old step, which was a flat 1.25x multiplier
// off whatever the current scale happened to be. That both landed on values like
// 137% and moved so little that a click read as a snap rather than a zoom. The
// zoom box is still free to set any value, and a trackpad pinch still zooms
// continuously - the ladder is for the discrete gestures.
//
// Every consecutive ratio here is above kZoomLadderMinStep, which is what stops
// the skip rule below from eating every other rung.
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
// The step the zoom ease's duration is calibrated against - not the zoom step
// itself (see kZoomLadder). A ladder step is ~1.4x, i.e. ~1.5 of these, so an
// ordinary +/- gesture glides for ~200 ms and only bigger typed-in jumps reach
// the kZoomEaseMaxMs cap.
constexpr double kZoomEaseRefRatio = 1.25;

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

// Zoom ease. A zoom step is a whole ladder rung (~1.4x), so without this the view
// cuts straight from one magnification to the next. The ease does NOT add intermediate
// zoom levels: the zoom itself still lands on its target in one go - scale_, the
// layout, the scrollbars and the render requests are all final before the gesture
// returns - and this only decides WHERE the frozen page bitmaps are drawn for the
// next few frames. That is what makes it safe: a missed cancel site costs one
// stale-looking frame, never a wrong scale or a page stuck soft.
constexpr int kZoomEaseMs = 130;     // for exactly one kZoomStep
constexpr int kZoomEaseMinMs = 90;   // must stay above keyboard auto-repeat (~33 ms)
constexpr int kZoomEaseMaxMs = 260;  // hard bound on how far the picture may trail
constexpr int kZoomEaseTickMs = 16;
// Below this magnification the ease is skipped: input this fine is already
// continuous (a trackpad pinch), and easing it would only make it feel late.
constexpr double kZoomEaseMinRatio = 1.05;

double easeOutCubic(double t)
{
    return 1.0 - std::pow(1.0 - std::clamp(t, 0.0, 1.0), 3.0);
}

// Progress reparameterized so that a plain lerp between the captured rect and the
// final one grows the drawn size GEOMETRICALLY: size(e) == size0 * k^e, which is
// the path the eye reads as a constant-rate zoom (lerping the size itself lunges
// on the way in and stalls on the way out). u(0) == 0 and u(1) == 1, so the last
// frame lands exactly on the final layout.
double easeU(double t, double k)
{
    const double e = easeOutCubic(t);
    if (!(k > 0.0) || std::abs(k - 1.0) < 1e-6)
        return e;
    return (std::pow(k, e) - 1.0) / (k - 1.0);
}

QRectF lerpRect(const QRectF &a, const QRectF &b, double u)
{
    return QRectF(a.x() + (b.x() - a.x()) * u, a.y() + (b.y() - a.y()) * u,
                  a.width() + (b.width() - a.width()) * u,
                  a.height() + (b.height() - a.height()) * u);
}

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

// Form-fill highlights (painted over fillable field rects in form mode).
const QColor &kFormFieldColor = theme::doc().formField;            // fillable field tint
const QColor &kFormRequiredColor = theme::doc().formFieldRequired; // required-but-empty
const QColor &kFormFieldBorder = theme::doc().formFieldBorder;     // field outline
const QColor &kFormFocusBorder = theme::doc().formFieldFocus;      // Tab-focused outline

constexpr int kAutoScrollMargin = 24; // px from the viewport edge
constexpr int kAutoScrollMaxStep = 40;

// Measuring tool: how close (in screen pixels) the cursor must be to a CAD
// vertex/edge to snap, and how long after the unit dropdown closes a press is
// treated as Qt's popup-replay (and swallowed) rather than a real page click.
constexpr double kSnapRadiusPx = 10.0;
constexpr qint64 kPopupReplayWindowMs = 150;

// Hand out a unique id per viewer instance. Viewers are only created on the UI
// thread, so a plain counter is sufficient; it is monotonic (never reused) so a
// late result for a destroyed viewer cannot be mistaken for a freshly created one.
quint64 nextViewerId()
{
    static quint64 counter = 0;
    return ++counter;
}

// Grab radius (px) for picking a committed measurement's vertex handle.
constexpr double kHandleGrabPx = 8.0;

// Default value-label centre (widget space) for a measurement, given its points
// already mapped to widget coordinates. Mirrors the anchor logic in paintShape so
// hit-testing matches what is drawn. Returns a null point when there is no label.
QPointF computeLabelAnchor(MeasureKind kind, const std::vector<QPointF> &w)
{
    if (w.size() < 2)
        return {};
    switch (kind) {
    case MeasureKind::Distance:
        return (w.front() + w.back()) / 2.0;
    case MeasureKind::Polyline:
        return (w[w.size() - 2] + w[w.size() - 1]) / 2.0;
    case MeasureKind::Area: {
        QPointF c;
        for (const QPointF &pt : w)
            c += pt;
        return c / double(w.size());
    }
    case MeasureKind::Angle:
        return w[1] + QPointF(0, -28);
    }
    return {};
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
    // Window, not Dark. The canvas colour comes from paintEvent (which fills the
    // whole damaged rect before anything else), so the role's only real effect is
    // on the widgets parented to this viewport - the floating tool panels and the
    // page popups. QWidget::backgroundRole() inherits down the parent chain and
    // QWidget::foregroundRole() maps a Dark/Shadow background to QPalette::Light,
    // so with Dark here every plain QLabel in those children painted its text in
    // Light: #2a313a on the dark panel, pure white on the light one - invisible in
    // both themes, which is exactly how the measuring panel lost its captions.
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

ViewerWidget::~ViewerWidget() = default;

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

void ViewerWidget::setDocument(Document *doc)
{
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
    measurements_.clear();
    inProgress_.clear();
    inProgressPage_ = -1;
    hoverValid_ = false;
    measureOverrides_.clearAll();
    lastScaleDesc_.clear();
    lastScaleResettable_ = -1;
    scalePage_ = -1;
    measureGeo_ = PageGeometry{};
    measureGeoPage_ = -1;
    clearSnapState();
    sincePanelPopupClosed_.invalidate();
    viewport()->setCursor(Qt::IBeamCursor);
    emit measureModeChanged(false);
    emit measurementReadout(QString());

    // Form filling: tear down any editors and (re)build the model for the new doc.
    destroyFormEditors();
    formFieldOrder_.clear();
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
    matches_.clear();
    matchesByPage_.clear();
    currentMatch_ = -1;
    findQuery_.clear();
    emit findStatusChanged(0, 0);

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

    // Fit against the viewport size WITHOUT scrollbars, not viewport()->size().
    // The viewport shrinks when a bar is shown, and the fit scale derived from
    // it decides whether that bar is needed at all. Under ScrollBarAsNeeded the
    // two states can each imply the other (content a hair taller than the
    // viewport only while the bar is hidden), and every visibility toggle
    // resizes the viewport back into resizeEvent -> applyFitScale, oscillating
    // forever: blank pages with jumping edges until the user leaves the fit
    // mode. A bar-independent basis yields the same scale in both states, so
    // there is nothing to feed the loop.
    //
    // Two Qt subtleties here: AsNeeded toggles an internal container widget,
    // never the QScrollBar itself, so isHidden() is constant-false and only
    // isVisibleTo(this) tracks the bar's real state (and keeps working while
    // the whole viewer is a hidden tab). And a bar that was never laid out
    // reports a default width()/height() of 100/30, so the extent must come
    // from sizeHint(), which honours the themed 12px even before first layout.
    QScrollBar *vsb = verticalScrollBar();
    QScrollBar *hsb = horizontalScrollBar();
    const int vsbW = vsb->sizeHint().width();
    const int hsbH = hsb->sizeHint().height();
    const double fullW = std::max(1, viewport()->width() + (vsb->isVisibleTo(this) ? vsbW : 0));
    const double fullH = std::max(1, viewport()->height() + (hsb->isVisibleTo(this) ? hsbH : 0));

    // Fit against what the LAYOUT will be, not against one page. The canvas width
    // is a document-wide quantity in both scrolling modes (the widest page in
    // Continuous, the width of the two columns in TwoPage), so a scale derived
    // from the current page must either overflow or leave a gutter as soon as the
    // pages differ in size - and which one you got depended on where the reader
    // happened to be scrolled, because nothing re-fits on scroll. ViewLayout owns
    // the margins, the inner gap, the breathing space and the per-page rounding,
    // so it does the arithmetic: the `- 40` that used to be here duplicated
    // 2 * margin_ + slack_ and only happened to be right for a two-page row.
    //
    // This buys an invariant worth keeping: in a fit mode the canvas is never
    // wider than the viewport in either scrollbar state, so a fit mode cannot
    // produce a horizontal scrollbar and the vertical-bar prediction below is
    // exact. Fit Width is now a pure function of (viewport, document, mode,
    // rotation); only Fit Page keeps a currentPage_ term, on the height axis,
    // because it fits the row you are looking at.
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
    ++viewEpoch_; // per-viewer epoch; results from older epochs are discarded
    cache_.clear();
    pending_.clear();
}

void ViewerWidget::seedPreview()
{
    if (!doc_ || pageCount() == 0)
        return;
    const QRect vpCanvas(contentOffset(), viewport()->size());
    // How much more of the document the new scale pulls into view: layout_ still
    // holds the OLD scale here while scale_ is already the new one, so a ratio
    // above 1 means zooming out. Freeze that much extra around the viewport so
    // the pages a zoom-out reveals have something to show too - capped, so a
    // jump from 800% to Fit Width does not try to freeze the whole document.
    // (In Single page mode only the current page is laid out, so the slack has
    // nothing to find and only that page is seeded.)
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
    // Past a certain magnification the stretch is mush rather than a preview of
    // the page; clean paper reads better than that. A ladder step is ~1.4x, so this
    // rules out a typed-in jump of more than about six rungs, or that many gestures
    // fired off faster than a single render completes. Judged
    // against the page's FINAL rect even mid-ease, so a jump too big for the tile
    // shows gliding paper throughout instead of blinking from content to white
    // half way through the gesture.
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

// --- zoom ease ---------------------------------------------------------------
//
// A zoom step is a full 1.25x, and rescaleKeeping applies it between two paint
// events, so the view cuts from one magnification to the next. The ease removes
// that cut WITHOUT touching the zoom itself: every zoom still lands on its final
// scale, layout, scroll offsets and render requests inside the one synchronous
// call, and only the DRAWING lags for ~130 ms, each page's frozen PreviewLayer
// tile being stretched from where the page was a moment ago onto where it now
// belongs. Two consequences worth keeping in mind when editing this:
//
//  - Nothing is deferred, so nothing can be left broken. Every cancel site below
//    is a cosmetic safeguard: missing one costs a single stale-looking frame, not
//    a wrong scale, a permanently soft page or a leaked render suppression.
//  - Each page is interpolated on its OWN rect rather than through one global
//    transform. ViewLayout adds fixed logical pixels (margin/gap) to scaled page
//    sizes, so a single similarity about one anchor would scale those too and the
//    pages would visibly slide against each other and snap shut at the landing.
//    Per-page rects keep the gaps constant, which is what a genuine intermediate
//    scale looks like.
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

bool ViewerWidget::captureZoomEase(double newScale, ZoomEase *out) const
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
        out->from.insert(pg, zoomEase_.active ? zoomEaseRect(pg, shown) : shown);
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

void ViewerWidget::startZoomEase(ZoomEase &&e)
{
    const QRect repFinal = layout_.pageRect(e.repPage);
    const auto rep = e.from.constFind(e.repPage);
    if (!repFinal.isValid() || rep == e.from.constEnd() || rep.value().width() <= 0.0)
        return;
    const double k = repFinal.width() / rep.value().width();
    if (!(k > 0.0) || std::abs(std::log(k)) < std::log(kZoomEaseMinRatio))
        return; // too small to be worth easing, or a retarget the display caught up with
    e.k = k;
    e.durMs = std::clamp(static_cast<int>(zoomEaseMs_ * std::abs(std::log(k))
                                          / std::log(kZoomEaseRefRatio)),
                         kZoomEaseMinMs, kZoomEaseMaxMs);
    e.active = true;
    e.clock.start();
    zoomEase_ = std::move(e);
    zoomEaseTimer_->start();
    viewport()->update();
}

void ViewerWidget::endZoomEase()
{
    if (!zoomEase_.active)
        return;
    zoomEase_ = ZoomEase{};
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
    if (!zoomEase_.active) {
        zoomEaseTimer_->stop();
        return;
    }
    // Progress comes from the clock, never from a tick count: a slow frame
    // shortens the remaining path instead of stretching the gesture, and a stall
    // (or a cancel site we missed) self-heals on the next tick.
    if (zoomEase_.forcedT < 0.0 && zoomEase_.clock.elapsed() >= zoomEase_.durMs) {
        endZoomEase();
        return;
    }
    viewport()->update();
}

double ViewerWidget::zoomEaseU() const
{
    if (!zoomEase_.active)
        return 1.0;
    const double t = zoomEase_.forcedT >= 0.0
                         ? zoomEase_.forcedT
                         : (zoomEase_.durMs <= 0
                                ? 1.0
                                : double(zoomEase_.clock.elapsed()) / zoomEase_.durMs);
    return easeU(t, zoomEase_.k);
}

QRectF ViewerWidget::zoomEaseFromRect(int pageNo, const QRectF &finalRect) const
{
    const auto it = zoomEase_.from.constFind(pageNo);
    if (it != zoomEase_.from.constEnd())
        return it.value();
    // A page the new layout revealed beyond what we captured: put it where the
    // same similarity that maps repPage's final rect back onto its captured one
    // would put it. On a zoom-out that factor is > 1, which pushes the page OFF
    // screen at u == 0, so a revealed page with no tile of its own arrives roughly
    // when its render does instead of sitting there as flat paper. Exact in the
    // scaling part and off only by the layout's fixed margins/gaps - invisible on
    // a page entering from the edge, and gone by the last frame.
    const auto rep = zoomEase_.from.constFind(zoomEase_.repPage);
    const QRect repFinal = layout_.pageRect(zoomEase_.repPage);
    if (rep == zoomEase_.from.constEnd() || !repFinal.isValid() || repFinal.width() <= 0)
        return finalRect;
    const QRectF repFinalW(repFinal.translated(-contentOffset()));
    if (repFinalW.width() <= 0.0)
        return finalRect;
    const double a = rep.value().width() / repFinalW.width();
    return QRectF(a * finalRect.x() + (rep.value().x() - a * repFinalW.x()),
                  a * finalRect.y() + (rep.value().y() - a * repFinalW.y()),
                  a * finalRect.width(), a * finalRect.height());
}

QRectF ViewerWidget::zoomEaseRect(int pageNo, const QRectF &finalRect) const
{
    if (!zoomEase_.active || finalRect.width() <= 0.0)
        return finalRect;
    const QRectF from = zoomEaseFromRect(pageNo, finalRect);
    if (from.width() <= 0.0)
        return finalRect;
    return lerpRect(from, finalRect, zoomEaseU());
}

// Per-axis rather than one factor from the width, and fed the SAME rect the bitmap
// was blitted into rather than re-deriving it: ViewLayout rounds a page's width and
// height independently, and paintEvent snaps the eased rect to whole pixels, so a
// single width-derived factor would leave the border and the overlays up to a few
// pixels clear of the bottom of the drawn image on a tall page. The anisotropy this
// introduces is under a third of a percent, well below the pixel snap it replaces.
QTransform ViewerWidget::zoomEaseTransform(const QRectF &finalRect, const QRectF &easedRect) const
{
    if (finalRect.width() <= 0.0 || finalRect.height() <= 0.0 || easedRect.width() <= 0.0
        || easedRect.height() <= 0.0) {
        return {};
    }
    const double ax = easedRect.width() / finalRect.width();
    const double ay = easedRect.height() / finalRect.height();
    QTransform xf;
    xf.translate(easedRect.x() - ax * finalRect.x(), easedRect.y() - ay * finalRect.y());
    xf.scale(ax, ay);
    return xf;
}

void ViewerWidget::addPagesHeldByEase(std::vector<int> *pages) const
{
    const QRectF vp(QPointF(0, 0), QSizeF(viewport()->size()));
    const QPoint off = contentOffset();
    for (auto it = zoomEase_.from.cbegin(); it != zoomEase_.from.cend(); ++it) {
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
    zoomEase_.forcedT = t; // < 0 hands progress back to the clock
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
    if (zoomEase_.active) {
        // Mid-ease the page is drawn somewhere other than `covered`, so it has to
        // keep going through the stretched path - hence no erase here; endZoomEase
        // catches up. Re-tiling rather than stretching the cache image directly
        // matters for cost: the render cache is RGB888, which the raster engine has
        // no fast transformed path for (see PreviewLayer.h), and add() pays the
        // RGB32 re-encode once so every remaining frame is the cheap blit. Only for
        // a whole-page render or a page with no tile yet, though: a clipped
        // deep-zoom band covers less of the page than the tile it would replace,
        // and swapping it in would open white gaps mid-flight.
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
    // Canvas backdrop behind the pages: neutral dark in both themes, warmed a
    // step under the slate dark chrome (theme::Chrome::canvas).
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
    if (zoomEase_.active)
        addPagesHeldByEase(&pages);

    for (std::size_t idx = 0; idx < pages.size(); ++idx) {
        const int i = pages[idx];
        const bool live = idx < liveCount; // in the FINAL viewport
        const QRect pageCanvas = layout_.pageRect(i);
        if (!pageCanvas.isValid())
            continue;
        const QRect r = pageCanvas.translated(-off);
        // Where this page is drawn this frame. Equals r whenever no ease is running.
        const QRect rDraw = zoomEase_.active ? zoomEaseRect(i, QRectF(r)).toAlignedRect() : r;
        const QRect easedCanvas = rDraw.translated(off);
        const QRect neededCanvas = pageCanvas.intersected(vpCanvas);
        const PageCache::Entry *e = cache_.get(i);
        // Mid-ease a landed render is re-tiled into preview_ (see onResultReady),
        // so the drawing always goes through the cheap stretched path and the page
        // sharpens in place instead of snapping to its final size.
        if (!zoomEase_.active && e && e->covered.contains(neededCanvas)) {
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
            // Missing or only partially covered (e.g. a deep-zoom tile we have
            // just scrolled past): paint the theme's paper colour, stretch the
            // page's frozen preview over it (so a zoom shows the old image
            // scaled instead of blank paper until the sharp one lands), redraw
            // whatever we still have anchored to its page position, then
            // request the needed region.
            p.fillRect(rDraw, pageBase);
            bool drew = drawPreview(p, i, pageCanvas, off, easedCanvas);
            if (!zoomEase_.active && e && !e->image.isNull()) {
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
        if (zoomEase_.active) {
            ++paintStats_.eased;
            easeGuard.emplace(p);
            p.setTransform(zoomEaseTransform(QRectF(r), QRectF(rDraw)), /*combine=*/true);
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

    // Frozen previews are only worth keeping for pages at or near the viewport.
    // Pruning here rather than at the next zoom is what stops a tile for a page
    // whose render never arrived (scrolled away first) from holding memory
    // indefinitely, and covers the paths that re-lay the document out without
    // invalidating the renders - Single page mode leaves every other page
    // unlaid-out and unpaintable. Normally a no-op: the layer is empty as soon as
    // the pages on screen have rendered.
    // Skipped mid-ease: retain() prunes by the FINAL viewport and would drop the
    // tile of a page that is on screen only because the ease is drawing it.
    if (!zoomEase_.active && !preview_.isEmpty()) {
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
        // Same unchanged-scale guard as showEvent: a resize that does not move
        // the fit scale (notably the viewport shrinking/growing because a
        // scrollbar toggled) must not invalidate the render cache - during the
        // former scrollbar oscillation that per-cycle invalidation was what
        // kept pages permanently blank. A device-pixel-ratio change must still
        // re-render, though: dpr_ is only refreshed in relayout(), and cached
        // images carry the old ratio.
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

    // Re-anchor the inline overlay editors on every resize. The centring offset
    // (centerDelta) shifts with the viewport size whenever the content is smaller
    // than the viewport, but in Custom zoom the branch above neither re-fits
    // (relayout -> syncFormEditors) nor necessarily moves the scrollbar
    // (scrollContentsBy -> syncFormEditors), so the form-fill editors would keep
    // their pre-resize geometry and drift off their fields. Sync them - and the
    // open annotation editor - here so they always track the page. Idempotent in
    // the fit modes, where relayout() already synced.
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
        // Single mode lays out only the current row, so the scrollbar covers just
        // that row and the base-class scroll can never reach the next one. Flip
        // once the row's own content is fully scrolled (a fitted row has no
        // scroll range, so the flip is immediate); a zoomed-in row scrolls its
        // remaining content first.
        //
        // A ROW, not a page: with the spread on, the row already on screen holds
        // both facing sheets, so stepping one page would re-show the same spread
        // and the key would look dead every other press.
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
    rescaleKeeping(fitted, viewportCenter());
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
        rescaleKeeping(target, viewportCenter());
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

void ViewerWidget::rescaleKeeping(double newScale, QPointF viewportPos)
{
    // Snapshot where the pages are DRAWN right now, before anything moves, then
    // arm the ease as the very LAST statement. That ordering is load-bearing: the
    // scrollbar writes below (and relayout's range clamp) both land in
    // scrollContentsBy, which ends an ease on the rule that a scroll the ease did
    // not cause is the user taking over. Arming first would cancel it immediately.
    ZoomEase ease;
    const bool wantEase = captureZoomEase(newScale, &ease);
    endZoomEase(); // the snapshot has consumed any running one

    // Pin the document point currently under `viewportPos` (the cursor for wheel
    // and pinch zoom, the middle of the viewport for the toolbar / menu /
    // keyboard): find which page point sits there now, rescale, then scroll so
    // that same page point lands back there. Using a page point (not a raw canvas
    // point) keeps the anchor exact even though the layout's margins/gaps are
    // fixed pixels that do not scale with the page.
    const QPointF canvasBefore = viewportPos + QPointF(contentOffset());
    const int pg = (doc_ && pageCount() > 0) ? pageAtCanvas(canvasBefore.toPoint()) : -1;
    const QPointF anchorPage =
        (pg >= 0) ? canvasToPagePoint(pg, canvasBefore, /*clampToPage=*/false) : QPointF();

    scale_ = newScale;
    invalidateRenders(PreviewPolicy::Keep);
    relayout(); // recomputes the layout and scrollbar ranges at the new scale

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

    if (textIndex_ && !query.isEmpty())
        matches_ = textIndex_->search(query, caseSensitive, wholeWord);
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

void ViewerWidget::rebuildFormModel()
{
    formModel_.reset();
    if (doc_ && doc_->hasForm())
        formModel_ = std::make_unique<FormModel>(*doc_);
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
    if (doc_ && doc_->isPdf()) // any PDF can receive annotations; other formats can't
        annotModel_ = std::make_unique<AnnotModel>(*doc_);
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
    return annotModel_ ? annotModel_->allAnnots() : std::vector<Annotation>{};
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
    int firstPage = -1;
    int firstId = -1;
    for (int pg = s.page; pg <= e.page; ++pg) {
        const int from = (pg == s.page) ? s.offset : 0;
        const int to = (pg == e.page) ? e.offset : textIndex_->pageTextLength(pg);
        if (to <= from)
            continue;
        const std::vector<QRectF> rects = textIndex_->rangeRects(pg, from, to - from);
        if (rects.empty())
            continue;
        const int id =
            annotModel_->addTextMarkup(pg, markupStyle_, rects, markupColor_, annotAuthor_);
        if (id < 0)
            continue;
        applyAnnotChange(pg);
        if (firstId < 0) {
            firstPage = pg;
            firstId = id;
        }
    }
    selection_.clear();
    if (firstId >= 0) {
        emit annotEditsChanged();
        emit annotationsChanged();
        openAnnotPopup(firstPage, firstId); // let the user add a comment immediately
    }
    viewport()->update();
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
    const int id = annotModel_->addTextNote(pg, pagePoint, markupColor_, annotAuthor_, QString());
    if (id < 0)
        return;
    applyAnnotChange(pg);
    emit annotEditsChanged();
    emit annotationsChanged();
    // A placed note is one-shot: return the tool to Select so the next click does
    // not drop another note. Switch BEFORE opening the editor - the Select
    // transition closes any open popup, so doing it afterwards would close the very
    // card we open here. The card stays editable (the Comment tool is still open).
    setAnnotSubMode(AnnotSubMode::Select);
    openAnnotPopup(pg, id);
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
            if (annotModel_ && openAnnotId_ >= 0
                && annotModel_->setContents(openAnnotPage_, openAnnotId_, text)) {
                applyAnnotChange(openAnnotPage_);
                emit annotEditsChanged();
                emit annotationsChanged();
            }
        });
        connect(annotPopup_, &AnnotPopup::colorPicked, this, [this](const QColor &color) {
            if (annotModel_ && openAnnotId_ >= 0
                && annotModel_->setColor(openAnnotPage_, openAnnotId_, color)) {
                applyAnnotChange(openAnnotPage_);
                emit annotEditsChanged();
                emit annotationsChanged();
            }
        });
        connect(annotPopup_, &AnnotPopup::deleteRequested, this, [this] {
            if (annotModel_ && openAnnotId_ >= 0) {
                const int pg = openAnnotPage_;
                if (annotModel_->remove(pg, openAnnotId_)) {
                    openAnnotPage_ = -1;
                    openAnnotId_ = -1;
                    annotPopup_->hide();
                    applyAnnotChange(pg);
                    emit annotEditsChanged();
                    emit annotationsChanged();
                }
            }
        });
        connect(annotPopup_, &AnnotPopup::dismissed, this, [this] {
            openAnnotPage_ = -1;
            openAnnotId_ = -1;
            viewport()->update(); // clear the selection outline
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
    const QColor accent = palette().color(QPalette::Highlight);
    p.setPen(QPen(accent, 1.5, Qt::DashLine));
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
        cb->addItems(f.options);
        int idx = f.options.indexOf(f.value);
        if (idx < 0 && !f.value.isEmpty()) {
            cb->addItem(f.value);
            idx = cb->count() - 1;
        }
        if (idx >= 0)
            cb->setCurrentIndex(idx);
        connect(cb, &QComboBox::activated, this, [this, page, fieldIndex, cb](int) {
            if (formModel_ && formModel_->setChoiceValue(page, fieldIndex, cb->currentText())) {
                applyFormFieldChange(page);
                emit formEditsChanged();
            }
        });
        w = cb;
        break;
    }
    case FormFieldType::ListBox: {
        auto *lw = new QListWidget(viewport());
        lw->setSelectionMode(QAbstractItemView::SingleSelection);
        lw->addItems(f.options);
        for (int i = 0; i < lw->count(); ++i)
            if (lw->item(i)->text() == f.value) {
                lw->setCurrentRow(i);
                break;
            }
        connect(lw, &QListWidget::itemSelectionChanged, this, [this, page, fieldIndex, lw]() {
            const QString v = lw->currentItem() ? lw->currentItem()->text() : QString();
            if (formModel_ && formModel_->setChoiceValue(page, fieldIndex, v)) {
                applyFormFieldChange(page);
                emit formEditsChanged();
            }
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
                       // blue input whose list opens as a dark slate menu.
                       "QComboBox QAbstractItemView { background:%1; color:%2;"
                       " selection-background-color:%4; selection-color:%5; }")
            .arg(theme::css(d.formEditorSurface), theme::css(d.formEditorInk),
                 theme::css(d.formEditorBorder), theme::css(d.formEditorSelection),
                 theme::css(d.formEditorSelectionInk)));
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
            // Match the inline editor's glyph height to the rendered (printed)
            // field text: pixelSize = effective point size * scale_ (logical px;
            // scale_ ONLY, never *dpr_ - the editor box is in logical pixels and
            // Qt applies the device pixel ratio internally). setPixelSize, not
            // setPointSizeF, so the OS logical-DPI point->pixel step can't
            // double-scale against the box. Recomputed every sync so it tracks
            // zoom (createFormEditor runs once; only sync sees the new scale_).
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
    bool changed = false;
    switch (e.type) {
    case FormFieldType::Text:
        if (auto *le = qobject_cast<QLineEdit *>(e.widget))
            changed = formModel_->setTextValue(e.page, e.fieldIndex, le->text());
        else if (auto *te = qobject_cast<QPlainTextEdit *>(e.widget))
            changed = formModel_->setTextValue(e.page, e.fieldIndex, te->toPlainText());
        break;
    case FormFieldType::ComboBox:
        if (auto *cb = qobject_cast<QComboBox *>(e.widget))
            changed = formModel_->setChoiceValue(e.page, e.fieldIndex, cb->currentText());
        break;
    case FormFieldType::ListBox:
        if (auto *lw = qobject_cast<QListWidget *>(e.widget))
            changed = formModel_->setChoiceValue(
                e.page, e.fieldIndex, lw->currentItem() ? lw->currentItem()->text() : QString());
        break;
    default:
        break;
    }
    if (changed) {
        applyFormFieldChange(e.page);
        emit formEditsChanged();
    }
}

void ViewerWidget::commitActiveFormEditor()
{
    // Idempotent (unchanged values are no-ops), so flushing every editor before a
    // save / print or mode change is safe and catches the field still being typed.
    for (int i = static_cast<int>(formEditors_.size()) - 1; i >= 0; --i)
        commitFormEditor(i);
}

void ViewerWidget::applyFormFieldChange(int page)
{
    // Re-render just the touched page: drop its cached image and any stale in-flight
    // request (so the new request's token supersedes), then request a fresh tile.
    // Its frozen preview goes too - it predates the edit.
    cache_.erase(page);
    preview_.erase(page);
    pending_.remove(page);
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

const std::vector<std::pair<int, int>> &ViewerWidget::formFieldOrder()
{
    static const std::vector<std::pair<int, int>> kEmpty;
    if (!formModel_)
        return kEmpty;
    if (!formFieldOrderBuilt_) {
        formFieldOrder_.clear();
        const int n = doc_ ? doc_->pageCount() : 0;
        for (int p = 0; p < n; ++p) {
            const std::vector<FormField> &fields = formModel_->pageFields(p);
            for (int fi = 0; fi < static_cast<int>(fields.size()); ++fi)
                if (fields[fi].editable())
                    formFieldOrder_.push_back({p, fi});
        }
        formFieldOrderBuilt_ = true;
    }
    return formFieldOrder_;
}

void ViewerWidget::focusFormFieldAt(int orderIndex)
{
    const std::vector<std::pair<int, int>> &order = formFieldOrder();
    if (orderIndex < 0 || orderIndex >= static_cast<int>(order.size()) || !formModel_)
        return;
    formFocusIndex_ = orderIndex;
    const int pg = order[orderIndex].first;
    const int fi = order[orderIndex].second;
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
            // No text under the cursor (e.g. a sticky-note icon in the margin): in
            // the pointer/Select state (this branch only runs when no annotation
            // gesture is active - i.e. the Comment tool is closed OR open on Select)
            // a click on a comment opens it read-only. Marks that overlap text are
            // decided on release so a drag still selects text (see the selecting_
            // branch in mouseReleaseEvent).
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
    emit contextMenuRequested(event->globalPos());
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

// ---- Measuring tool --------------------------------------------------------

void ViewerWidget::setMeasureMode(bool on)
{
    if (on) {
        if (!doc_)
            return;
        if (toolMode_ == ToolMode::Ocr)
            setOcrMode(false);
        if (toolMode_ == ToolMode::FillForms)
            setFormMode(false); // forms and measure are mutually exclusive
        const bool wasEnabled = measureToolEnabled_;
        measureToolEnabled_ = true;
        toolMode_ = ToolMode::Measure; // enabling arms the measuring crosshair
        // Measure and the Comment tool can be open together; taking the crosshair
        // just idles the annotation gesture (the Comment panel stays docked).
        idleAnnotGesture();
        selection_.clear();
        selecting_ = false;
        if (rubberBand_)
            rubberBand_->hide();
        viewport()->setCursor(Qt::CrossCursor);
        lastScaleDesc_.clear();
        lastScaleResettable_ = -1; // panel just shown: force a fresh Reset-button sync
        const QSizeF sz = doc_->pageSize(currentPage_);
        updateHoverScale(currentPage_, QPointF(sz.width() / 2, sz.height() / 2));
        if (!wasEnabled)
            emit measureModeChanged(true);
        emit measureCursorActiveChanged(true);
        maybeAutoCalibrate(); // scale-less document: the first line calibrates
        viewport()->update();
    } else if (measureToolEnabled_) {
        // Disable the whole tool: cancel any vector, hide the panel and the
        // overlays (the measurements themselves are kept, just not drawn).
        cancelInProgressMeasure();
        clearSnapState();
        measureDrag_ = MeasureDrag::None;
        dragMeasureIdx_ = -1;
        dragVertexIdx_ = -1;
        sincePanelPopupClosed_.invalidate(); // don't swallow a later real click
        measureToolEnabled_ = false;
        // Only release the single active gesture if Measure actually holds it - the
        // Comment tool may own it (Highlight/Comment) while its panel stays docked.
        if (measureMode()) {
            toolMode_ = ToolMode::None;
            viewport()->setCursor(Qt::IBeamCursor);
        } else if (commentMode()) {
            viewport()->setCursor(Qt::PointingHandCursor); // Comment tool keeps the Note gesture
        } // else Highlight/None already use the I-beam cursor
        emit measureModeChanged(false);
        viewport()->update();
    }
}

bool ViewerWidget::pageHasScale(int page) const
{
    if (!doc_ || page < 0)
        return false;
    const QSizeF sz = doc_->pageSize(page);
    return resolvedScale(page, QPointF(sz.width() / 2, sz.height() / 2)).valid();
}

bool ViewerWidget::pageHasEmbeddedScale(int page) const
{
    if (!doc_ || page < 0)
        return false;
    // Resolve with an empty override so an override on this page doesn't mask the
    // underlying embedded scale; it's embedded only if the PDF itself supplies one.
    const PageMeasurement pm = doc_->pageMeasurement(page);
    const QSizeF sz = doc_->pageSize(page);
    const MeasureScale s =
        measure::resolveScale(pm, QPointF(sz.width() / 2, sz.height() / 2), MeasureScale{});
    return s.valid() && s.source == MeasureSource::Embedded;
}

bool ViewerWidget::canResetScale(int page) const
{
    // Resettable only when a manual/calibrated override is masking an embedded PDF
    // scale: clearing it then falls back to the PDF's scale. On a scale-less page
    // the override is the only scale, so it's kept (not resettable).
    return measureOverrides_.hasOverride(page) && pageHasEmbeddedScale(page);
}

void ViewerWidget::maybeAutoCalibrate()
{
    if (!measureToolEnabled_ || !doc_)
        return;
    if (toolMode_ != ToolMode::Measure)
        return; // already calibrating, or on the standard pointer
    if (!measurements_.empty() || !inProgress_.empty())
        return; // not the very first measurement
    if (pageHasScale(currentPage_))
        return; // the document already has a usable scale - nothing to calibrate
    beginCalibration(); // first drawn line becomes the calibration line
}

void ViewerWidget::setMeasureCursorActive(bool active)
{
    if (!measureToolEnabled_)
        return; // only meaningful while the tool is on
    if (active) {
        if (measureMode())
            return; // already on the crosshair (Measure or Calibrate)
        toolMode_ = ToolMode::Measure;
        idleAnnotGesture(); // taking the crosshair idles the Comment gesture (panel stays)
        selection_.clear();
        selecting_ = false;
        viewport()->setCursor(Qt::CrossCursor);
        emit measureCursorActiveChanged(true);
        viewport()->update();
    } else {
        if (!measureMode())
            return; // not currently measuring (the gesture may belong to the Comment tool)
        cancelInProgressMeasure();
        clearSnapState();
        measureDrag_ = MeasureDrag::None;
        dragMeasureIdx_ = -1;
        dragVertexIdx_ = -1;
        toolMode_ = ToolMode::None; // standard pointer: clicks select text, not points
        viewport()->setCursor(Qt::IBeamCursor);
        emit measurementReadout(QString()); // drop the live readout
        emit measureCursorActiveChanged(false);
        viewport()->update();
    }
}

void ViewerWidget::setMeasureKind(MeasureKind kind)
{
    if (measureKind_ == kind)
        return;
    measureKind_ = kind;
    cancelInProgressMeasure();
    emit measurementReadout(QString());
    viewport()->update();
}

void ViewerWidget::setMeasureUnit(MeasureUnit unit)
{
    if (measureUnit_ == unit)
        return;
    measureUnit_ = unit;
    lastScaleDesc_.clear();
    if (measureToolEnabled_ && doc_) {
        // Re-sync the panel for the page whose scale it is showing (the last
        // hovered/calibrated page), not the viewport-centre page: otherwise a unit
        // change with the cursor parked would flip the label and the Reset target
        // to a different page. scalePage_ < 0 (no hover yet) falls back to current.
        const int p = scalePage_ >= 0 ? scalePage_ : currentPage_;
        const QSizeF sz = doc_->pageSize(p);
        updateHoverScale(p, QPointF(sz.width() / 2, sz.height() / 2));
    }
    emitMeasurementsChanged(); // list values reflect the new unit
    viewport()->update();      // labels reflect the new unit
}

void ViewerWidget::setMeasurePrecision(int decimals)
{
    decimals = std::clamp(decimals, 0, 6);
    if (measurePrecision_ == decimals)
        return;
    measurePrecision_ = decimals;
    emitMeasurementsChanged(); // list values reflect the new precision
    viewport()->update();
}

void ViewerWidget::setMeasureLineWidth(double width)
{
    width = std::clamp(width, 0.25, 10.0);
    if (qFuzzyCompare(measureLineWidth_, width))
        return;
    measureLineWidth_ = width;
    viewport()->update(); // redraw marks at the new stroke width
}

void ViewerWidget::setMeasureSnap(bool on)
{
    if (measureSnap_ == on)
        return;
    measureSnap_ = on;
    if (!on)
        clearSnapState();
    viewport()->update();
}

void ViewerWidget::notifyMeasurePanelPopupClosed()
{
    // Arm the one-shot guard: the imminent replayed press (if any) is swallowed.
    sincePanelPopupClosed_.start();
}

void ViewerWidget::beginCalibration()
{
    if (!doc_)
        return;
    const bool wasCursorActive = measureMode();
    cancelInProgressMeasure();
    toolMode_ = ToolMode::Calibrate;
    idleAnnotGesture(); // calibrating takes the crosshair; idle the Comment gesture
    viewport()->setCursor(Qt::CrossCursor);
    if (!wasCursorActive)
        emit measureCursorActiveChanged(true); // calibrating uses the crosshair
    emit measurementReadout(tr("Draw a line over a known dimension…"));
    viewport()->update();
}

void ViewerWidget::cancelCalibration()
{
    if (toolMode_ != ToolMode::Calibrate)
        return;
    cancelInProgressMeasure();
    toolMode_ = ToolMode::Measure;
    emit measurementReadout(QString());
    viewport()->update();
}

void ViewerWidget::promptSetScale()
{
    if (!doc_)
        return;
    // Manual scale entry draws no line: drop any half-drawn vector and abandon a
    // pending calibration so the tool returns to a clean measuring state.
    cancelInProgressMeasure();
    if (toolMode_ == ToolMode::Calibrate)
        toolMode_ = ToolMode::Measure;
    emit measurementReadout(QString()); // clear the "Draw a line…" prompt if shown
    // Target the page whose scale the panel is showing (set by updateHoverScale),
    // falling back to the current page - same as resetPageScale.
    const int page = scalePage_ >= 0 ? scalePage_ : currentPage_;
    emit setScaleRequested(page);
    viewport()->update();
}

void ViewerWidget::resetPageScale()
{
    // Target the page whose scale the panel is showing (set by updateHoverScale),
    // falling back to the current page. Re-check the guard: the button is only
    // shown when resettable, but a stale click shouldn't drop a calibration that
    // is a page's only scale.
    const int page = scalePage_ >= 0 ? scalePage_ : currentPage_;
    if (!canResetScale(page))
        return;
    // An invalid scale clears the override; setPageScaleOverride re-emits the scale
    // description + resettable state so the panel updates (label reverts to the
    // embedded scale, Reset hides).
    setPageScaleOverride(page, MeasureScale{});
}

void ViewerWidget::clearMeasurements()
{
    measurements_.clear();
    cancelInProgressMeasure();
    measureDrag_ = MeasureDrag::None;
    dragMeasureIdx_ = -1;
    dragVertexIdx_ = -1;
    emit measurementReadout(QString());
    emitMeasurementsChanged();
    viewport()->update();
}

void ViewerWidget::removeMeasurement(int index)
{
    if (index < 0 || index >= static_cast<int>(measurements_.size()))
        return;
    measurements_.erase(measurements_.begin() + index);
    // A drag in flight referring to a now-shifted index would be unsafe; the X
    // button is only reachable when not dragging, but cancel defensively.
    measureDrag_ = MeasureDrag::None;
    dragMeasureIdx_ = -1;
    dragVertexIdx_ = -1;
    emitMeasurementsChanged();
    viewport()->update();
}

void ViewerWidget::copyMeasurementValue(int index)
{
    if (index < 0 || index >= static_cast<int>(measurements_.size()))
        return;
    const Measurement &m = measurements_[index];
    const QString value = formatMeasurement(m.page, m.kind, m.pts);
    if (!value.isEmpty())
        QGuiApplication::clipboard()->setText(value);
}

void ViewerWidget::onMeasurementHovered(int index, bool hovered)
{
    const int next = (hovered && index >= 0 && index < static_cast<int>(measurements_.size()))
                         ? index
                         : -1;
    if (next == hoveredMeasurementIndex_)
        return;
    hoveredMeasurementIndex_ = next;
    viewport()->update(); // repaint so the emphasis appears/clears immediately
}

void ViewerWidget::emitMeasurementsChanged()
{
    QStringList items;
    items.reserve(static_cast<int>(measurements_.size()));
    for (const Measurement &m : measurements_) {
        // The list rows are single-line and elided; collapse an area's two-line
        // value (area + perimeter) into one inline row so it reads cleanly there.
        QString s = formatMeasurement(m.page, m.kind, m.pts);
        s.replace(QLatin1Char('\n'), QStringLiteral(" · "));
        items << s;
    }
    emit measurementsChanged(items);
}

void ViewerWidget::setPageScaleOverride(int page, const MeasureScale &scale)
{
    if (scale.valid())
        measureOverrides_.setOverride(page, scale);
    else
        measureOverrides_.clearOverride(page);
    lastScaleDesc_.clear();
    if (doc_) {
        const QSizeF sz = doc_->pageSize(page);
        updateHoverScale(page, QPointF(sz.width() / 2, sz.height() / 2));
    }
    emit measurementReadout(QString());
    emitMeasurementsChanged(); // committed values on this page rescale
    viewport()->update();
}

void ViewerWidget::handleMeasureClick(QPoint vpPos)
{
    const QPoint canvas = vpPos + contentOffset();
    const int pg = pageAtCanvas(canvas);
    if (pg < 0)
        return;
    // A measurement isn't valid without a scale: starting one on a page that has
    // none arms calibration instead, so this click becomes the first calibration
    // point and the user must set the scale before any measurement is committed.
    if (toolMode_ == ToolMode::Measure && inProgress_.empty() && !pageHasScale(pg))
        beginCalibration();
    if (!inProgress_.empty() && pg != inProgressPage_)
        return; // a multi-vertex measurement stays on one page (check before snapping)
    const QPointF pp = snapPagePoint(pg, canvasToPagePoint(pg, canvas));

    if (inProgress_.empty())
        inProgressPage_ = pg;

    inProgress_.push_back(pp);
    hoverPagePoint_ = pp;
    hoverValid_ = true;

    if (toolMode_ == ToolMode::Calibrate) {
        if (inProgress_.size() >= 2) {
            const double lenPts = QLineF(inProgress_[0], inProgress_[1]).length();
            const int page = inProgressPage_;
            cancelInProgressMeasure();
            toolMode_ = ToolMode::Measure;
            viewport()->update();
            emit calibrationLineDrawn(page, lenPts);
        } else {
            viewport()->update();
        }
        return;
    }

    const int need = (measureKind_ == MeasureKind::Distance) ? 2
                     : (measureKind_ == MeasureKind::Angle)  ? 3
                                                             : -1;
    if (need > 0 && static_cast<int>(inProgress_.size()) >= need) {
        commitInProgress();
    } else {
        emit measurementReadout(formatMeasurement(inProgressPage_, measureKind_, inProgress_));
        viewport()->update();
    }
}

void ViewerWidget::commitInProgress()
{
    if (inProgress_.empty())
        return;
    Measurement m;
    m.page = inProgressPage_;
    m.kind = measureKind_;
    m.pts = inProgress_;
    const QString text = formatMeasurement(m.page, m.kind, m.pts);
    measurements_.push_back(std::move(m));
    inProgress_.clear();
    inProgressPage_ = -1;
    hoverValid_ = false;
    emit measurementReadout(text);
    emitMeasurementsChanged();
    viewport()->update();
}

void ViewerWidget::cancelInProgressMeasure()
{
    inProgress_.clear();
    inProgressPage_ = -1;
    hoverValid_ = false;
}

void ViewerWidget::finishPolyOrArea()
{
    if (measureKind_ != MeasureKind::Polyline && measureKind_ != MeasureKind::Area)
        return;
    // Drop a near-duplicate final vertex left by the finishing double-click.
    if (inProgress_.size() >= 2 && inProgressPage_ >= 0) {
        const QPointF a = pagePointToWidget(inProgressPage_, inProgress_[inProgress_.size() - 1]);
        const QPointF b = pagePointToWidget(inProgressPage_, inProgress_[inProgress_.size() - 2]);
        if (QLineF(a, b).length() < 4.0)
            inProgress_.pop_back();
    }
    const int minPts = (measureKind_ == MeasureKind::Area) ? 3 : 2;
    if (static_cast<int>(inProgress_.size()) >= minPts)
        commitInProgress();
    else
        cancelInProgressMeasure();
    viewport()->update();
}

std::vector<QPointF> ViewerWidget::previewPts() const
{
    std::vector<QPointF> pts = inProgress_;
    if (hoverValid_ && !pts.empty())
        pts.push_back(hoverPagePoint_);
    return pts;
}

MeasureScale ViewerWidget::resolvedScale(int page, QPointF pp) const
{
    if (!doc_)
        return {};
    const PageMeasurement pm = doc_->pageMeasurement(page);
    return measure::resolveScale(pm, pp, measureOverrides_.override(page));
}

QString ViewerWidget::formatMeasurement(int page, MeasureKind kind,
                                        const std::vector<QPointF> &pts) const
{
    if (pts.empty())
        return {};
    // Shared with the burned-in / annotated PDF labels, so they read identically.
    return formatMeasurementValue(kind, pts, resolvedScale(page, pts.front()), measureUnit_,
                                  measurePrecision_);
}

void ViewerWidget::loadMeasurements(std::vector<Measurement> measurements, MeasureModel overrides,
                                    MeasureUnit unit, int precision, double lineWidth)
{
    measurements_ = std::move(measurements);
    measureOverrides_ = std::move(overrides);
    measureUnit_ = unit;
    measurePrecision_ = precision;
    measureLineWidth_ = std::clamp(lineWidth, 0.25, 10.0);
    // Marks are only painted while the tool is enabled (see paintEvent), so turn
    // it on to surface the loaded measurements and the panel.
    if (!measurements_.empty())
        setMeasureMode(true);
    emitMeasurementsChanged();
    viewport()->update();
}

QString ViewerWidget::scaleDescription(int page, QPointF pp) const
{
    const MeasureScale s = resolvedScale(page, pp);
    const QString unit = measure::unitSuffix(measureUnit_);
    if (!s.valid())
        return tr("No scale - Calibrate (paper · %1)").arg(unit);
    QString suffix;
    if (s.source == MeasureSource::Calibrated)
        suffix = tr(" · calibrated");
    else if (s.source == MeasureSource::Manual)
        suffix = tr(" · manual");
    return tr("Scale %1 · %2%3").arg(s.label, unit, suffix);
}

void ViewerWidget::updateHoverScale(int page, QPointF pp)
{
    scalePage_ = page; // the page Reset would act on (the one whose scale is shown)
    const QString desc = scaleDescription(page, pp);
    if (desc != lastScaleDesc_) {
        lastScaleDesc_ = desc;
        emit measureScaleChanged(desc);
    }
    const int resettable = canResetScale(page) ? 1 : 0;
    if (resettable != lastScaleResettable_) {
        lastScaleResettable_ = resettable;
        emit measureScaleResettableChanged(resettable == 1);
    }
}

bool ViewerWidget::pressIsOverMeasurePanel(QMouseEvent *event) const
{
    const QPoint g = event->globalPosition().toPoint();
    for (QWidget *panel : {static_cast<QWidget *>(measurePanel_), static_cast<QWidget *>(annotPanel_)}) {
        if (panel && panel->isVisible() && panel->rect().contains(panel->mapFromGlobal(g)))
            return true;
    }
    return false;
}

bool ViewerWidget::swallowToolPress(QMouseEvent *event)
{
    // (1) the synthetic press Qt replays when a panel dropdown (unit / line-width)
    //     is dismissed over the page - the common case, since the list extends onto
    //     the page so its dismiss-click lands on the viewport;
    if (sincePanelPopupClosed_.isValid()
        && sincePanelPopupClosed_.elapsed() < kPopupReplayWindowMs) {
        sincePanelPopupClosed_.invalidate();
        return true;
    }
    // (2) a popup is still up - never a real page click;
    if (QApplication::activePopupWidget())
        return true;
    // (3) a press physically over a visible docked tool panel (measure OR comment).
    return pressIsOverMeasurePanel(event);
}

void ViewerWidget::clearSnapState()
{
    snapValid_ = false;
    snapPage_ = -1;
    snapType_ = snap::SnapType::None;
}

void ViewerWidget::ensureMeasureGeometry(int page)
{
    if (!doc_ || page < 0 || measureGeoPage_ == page)
        return;
    measureGeo_ = doc_->pageGeometry(page); // cached in Document; one copy per page change
    measureGeoPage_ = page;
}

QPointF ViewerWidget::snapPagePoint(int page, QPointF pp)
{
    if (!measureSnap_ || !doc_ || page < 0) {
        clearSnapState();
        return pp;
    }
    ensureMeasureGeometry(page);
    if (measureGeo_.empty()) {
        clearSnapState();
        return pp;
    }
    // Keep the snap reach a constant on-screen distance regardless of zoom.
    const double radiusPts = (scale_ > 0.0) ? kSnapRadiusPx / scale_ : kSnapRadiusPx;
    const snap::SnapResult r = snap::snap(measureGeo_, pp, radiusPts);
    if (r.snapped()) {
        snapValid_ = true;
        snapPage_ = page;
        snapPagePoint_ = r.point;
        snapType_ = r.type;
        return r.point;
    }
    clearSnapState();
    return pp;
}

void ViewerWidget::drawSnapIndicator(QPainter &p) const
{
    if (!snapValid_ || !measureMode() || snapPage_ < 0)
        return;
    QPointF w = pagePointToWidget(snapPage_, snapPagePoint_);
    // Drawn after the page loop, so it does not inherit the loop's ease transform:
    // carry it to the eased position by hand, or the marker would sit off the point
    // it is locked to for the length of a zoom.
    if (zoomEase_.active) {
        const QRect pr = layout_.pageRect(snapPage_);
        if (pr.isValid()) {
            const QRectF fin(pr.translated(-contentOffset()));
            w = zoomEaseTransform(fin, zoomEaseRect(snapPage_, fin)).map(w);
        }
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor accent = theme::chrome(palette()).accent;
    p.setPen(QPen(accent, 2.0));
    p.setBrush(Qt::NoBrush);
    // Sized to roughly match the crosshair cursor's arms so the snap marker reads
    // as "the cursor is locked here" rather than a tiny separate dot.
    const double r = 12.0;
    if (snapType_ == snap::SnapType::Vertex) {
        p.drawRect(QRectF(w.x() - r, w.y() - r, 2 * r, 2 * r)); // endpoint marker
    } else {
        p.drawLine(QPointF(w.x() - r, w.y() - r), QPointF(w.x() + r, w.y() + r)); // edge marker (×)
        p.drawLine(QPointF(w.x() - r, w.y() + r), QPointF(w.x() + r, w.y() - r));
    }
    p.restore();
}

void ViewerWidget::drawMeasurements(QPainter &p, int page) const
{
    bool hasCommitted = false;
    for (const Measurement &m : measurements_) {
        if (m.page == page) {
            hasCommitted = true;
            break;
        }
    }
    const bool hasInProgress = (inProgressPage_ == page && !inProgress_.empty());
    if (!hasCommitted && !hasInProgress)
        return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor accent = theme::chrome(palette()).accent;

    // A measurement's decoration - stroke width, vertex handles, the angle arc, the
    // value pill and its font - is screen-space chrome in device-independent pixels,
    // so it must NOT ride the zoom ease's transform (a 4x zoom-out would draw
    // 8-pixel strokes and a giant pill for a few frames, then snap back). Take the
    // transform off the painter and apply it to the POINTS instead: identical
    // placement, true screen sizes. Whatever the painter carries is what gets
    // mapped, so this holds no matter what put it there.
    QTransform pointXf;
    if (zoomEase_.active) {
        pointXf = p.transform();
        p.setWorldTransform(QTransform());
    }

    for (int i = 0; i < static_cast<int>(measurements_.size()); ++i) {
        const Measurement &m = measurements_[i];
        if (m.page == page)
            paintShape(p, page, m.kind, m.pts, accent, false, m.hasLabelPos, m.labelPos,
                       i == hoveredMeasurementIndex_, pointXf);
    }

    if (hasInProgress)
        paintShape(p, page, measureKind_, previewPts(), accent, true, false, {}, false, pointXf);

    p.restore();
}

void ViewerWidget::paintShape(QPainter &p, int page, MeasureKind kind,
                              const std::vector<QPointF> &pagePts, const QColor &accent,
                              bool inProgress, bool hasLabelOverride, QPointF labelOverridePage,
                              bool isHovered, const QTransform &pointXf) const
{
    if (pagePts.empty())
        return;
    // pointXf is identity except during a zoom ease, where the caller hands us the
    // page's ease transform to apply here instead of on the painter (see
    // drawMeasurements) so the chrome keeps its true screen size.
    std::vector<QPointF> w;
    w.reserve(pagePts.size());
    for (const QPointF &pp : pagePts)
        w.push_back(pointXf.map(pagePointToWidget(page, pp)));

    QPolygonF poly;
    for (const QPointF &pt : w)
        poly << pt;

    QPen pen(accent);
    // Hovering the measurement's row in the panel thickens its stroke by 2 pt.
    pen.setWidthF(measureLineWidth_ + (isHovered ? 2.0 : 0.0));
    if (inProgress)
        pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (kind == MeasureKind::Area && w.size() >= 3) {
        QColor fill = accent;
        fill.setAlpha(theme::doc().measureAreaAlpha);
        p.setBrush(fill);
        p.drawPolygon(poly);
        p.setBrush(Qt::NoBrush);
    } else if (kind == MeasureKind::Angle && w.size() >= 3) {
        p.drawPolyline(poly);
        const QPointF v = w[1];
        const double r = 22.0;
        auto angOf = [&](const QPointF &q) {
            return std::atan2(-(q.y() - v.y()), q.x() - v.x()) * 180.0
                   / 3.14159265358979323846;
        };
        const double a0 = angOf(w[0]);
        double span = angOf(w[2]) - a0;
        while (span <= -180.0)
            span += 360.0;
        while (span > 180.0)
            span -= 360.0;
        QPen ap(accent);
        ap.setWidthF(1.4);
        p.setPen(ap);
        p.drawArc(QRectF(v.x() - r, v.y() - r, 2 * r, 2 * r), qRound(a0 * 16),
                  qRound(span * 16));
        p.setPen(pen);
    } else {
        p.drawPolyline(poly);
    }

    // Vertex handles.
    p.setPen(QPen(accent, 1.4));
    p.setBrush(theme::doc().measureHandle);
    for (const QPointF &pt : w)
        p.drawEllipse(pt, 3.5, 3.5);

    // Value label - at the user-pinned position when set, else auto-anchored.
    if (w.size() >= 2) {
        const QString text = formatMeasurement(page, kind, pagePts);
        if (!text.isEmpty()) {
            const QPointF anchor = hasLabelOverride
                                       ? pointXf.map(pagePointToWidget(page, labelOverridePage))
                                       : computeLabelAnchor(kind, w);
            drawLabelPill(p, anchor, text);
        }
    }
}

// The value-pill rect for `text` centred on `anchorWidget`. The anchor is in
// widget space (already shifted by the scroll offset), so the pill scrolls with
// the page rather than being pinned inside the viewport. Shared by drawing and
// hit-testing so they agree.
QRectF ViewerWidget::labelRectForAnchor(const QString &text, QPointF anchorWidget) const
{
    const QFontMetricsF fm(font());
    // boundingRect(const QString&) treats the text as one line; the rect+flags
    // overload honours embedded '\n' (an area's value with its perimeter below),
    // so the pill grows to fit every line.
    const QRectF tb = fm.boundingRect(QRectF(0, 0, 10000, 10000), Qt::AlignLeft, text);
    QRectF pill(0, 0, tb.width() + 14.0, tb.height() + 8.0);
    pill.moveCenter(anchorWidget);
    return pill;
}

QColor ViewerWidget::pagePaper() const
{
    const theme::Doc &d = theme::doc();
    switch (pageTheme_) {
    case PageTheme::Comfort:
        return d.paperComfort;
    case PageTheme::Inverted:
        return d.paperInverted;
    default:
        return d.paperNormal;
    }
}

QColor ViewerWidget::pageInk() const
{
    // The ink the page itself renders text in, so a pill drawn over the page
    // matches the page rather than the chrome.
    if (pageTheme_ == PageTheme::Comfort)
        return QColor(comfort::kRampFg[0], comfort::kRampFg[1], comfort::kRampFg[2]);
    return pageTheme_ == PageTheme::Inverted ? theme::doc().paperNormal
                                             : theme::doc().paperInverted;
}

void ViewerWidget::drawLabelPill(QPainter &p, QPointF anchor, const QString &text) const
{
    p.save();
    const QRectF pill = labelRectForAnchor(text, anchor);
    // The pill sits ON the page, so it takes the page's paper and ink - not the
    // chrome palette, which would put a near-black pill on white paper as soon as
    // the UI theme went dark.
    const theme::Doc &d = theme::doc();
    QColor bg = pagePaper();
    bg.setAlpha(d.measureLabelAlpha);
    const QColor ink = pageInk();
    QColor bd = ink;
    bd.setAlpha(d.measureLabelEdgeAlpha);
    p.setBrush(bg);
    p.setPen(QPen(bd, 1.0));
    p.drawRoundedRect(pill, 6, 6);
    p.setPen(ink);
    p.setFont(font());
    p.drawText(pill, Qt::AlignCenter, text);
    p.restore();
}

QPointF ViewerWidget::labelAnchorWidget(const Measurement &m) const
{
    if (m.hasLabelPos)
        return pagePointToWidget(m.page, m.labelPos);
    std::vector<QPointF> w;
    w.reserve(m.pts.size());
    for (const QPointF &pp : m.pts)
        w.push_back(pagePointToWidget(m.page, pp));
    return computeLabelAnchor(m.kind, w);
}

QRectF ViewerWidget::labelRectFor(const Measurement &m) const
{
    if (m.pts.size() < 2)
        return {};
    const QString text = formatMeasurement(m.page, m.kind, m.pts);
    if (text.isEmpty())
        return {};
    return labelRectForAnchor(text, labelAnchorWidget(m));
}

bool ViewerWidget::measureHitTest(QPoint vpPos, int &measureIdx, int &vertexIdx,
                                  bool &onLabel) const
{
    measureIdx = -1;
    vertexIdx = -1;
    onLabel = false;
    const QPointF cursor(vpPos);
    // Vertex handles first (topmost measurement wins), so a handle on top of a
    // label is grabbed for an endpoint drag rather than a label move. Skip pages
    // that are not currently laid out (e.g. non-current page in Single mode):
    // pagePointToWidget collapses their points to (0,0), which would otherwise
    // create a phantom hit target at the viewport's top-left corner.
    for (int i = static_cast<int>(measurements_.size()) - 1; i >= 0; --i) {
        const Measurement &m = measurements_[i];
        if (!layout_.pageRect(m.page).isValid())
            continue;
        for (int v = 0; v < static_cast<int>(m.pts.size()); ++v) {
            if (QLineF(cursor, pagePointToWidget(m.page, m.pts[v])).length() <= kHandleGrabPx) {
                measureIdx = i;
                vertexIdx = v;
                return true;
            }
        }
    }
    // Then value labels.
    for (int i = static_cast<int>(measurements_.size()) - 1; i >= 0; --i) {
        if (!layout_.pageRect(measurements_[i].page).isValid())
            continue;
        const QRectF pill = labelRectFor(measurements_[i]);
        if (pill.isValid() && pill.contains(cursor)) {
            measureIdx = i;
            onLabel = true;
            return true;
        }
    }
    return false;
}

} // namespace mervin
