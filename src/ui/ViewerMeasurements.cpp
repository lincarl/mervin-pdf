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
// Measuring tool: how close (in screen pixels) the cursor must be to a CAD
// vertex/edge to snap, and how long after the unit dropdown closes a press is
// treated as Qt's popup-replay (and swallowed) rather than a real page click.
constexpr double kSnapRadiusPx = 10.0;
constexpr qint64 kPopupReplayWindowMs = 150;

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
    markMeasurementsSaved();
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

    // Keep stroke, handle, arc, and label sizes fixed in viewport pixels while measurement points scale.
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


MeasureDoc ViewerWidget::measurementDocument() const
{
    mervin::MeasureDoc md;
    md.version = 1;
    md.unit = measureUnit();
    md.precision = measurePrecision();
    md.lineWidth = measureLineWidth();
    md.measurements = committedMeasurements();
    // Persist only the manual/calibrated page overrides (embedded scales are
    // re-derived from the PDF). MeasureModel has no enumerator, so scan pages.
    const MeasureModel &ov = measureOverrides();
    for (int p = 0; p < pageCount(); ++p) {
        if (!ov.hasOverride(p))
            continue;
        const MeasureScale s = ov.override(p);
        mervin::PageScale ps;
        ps.page = p;
        ps.mmPerPointX = s.mmPerPointX;
        ps.mmPerPointY = s.mmPerPointY;
        ps.label = s.label;
        ps.source = s.source;
        md.pageScales.push_back(ps);
    }
    return md;
}

void ViewerWidget::markMeasurementsSaved()
{
    savedMeasurements_ = serializeMeasurements(measurementDocument());
    savedMeasureData_ = hasMeasurements() || measureOverrides_.hasAnyOverride();
}

bool ViewerWidget::hasMeasurementEdits() const
{
    if (!savedMeasureData_ && !hasMeasurements() && !measureOverrides_.hasAnyOverride())
        return false;
    return savedMeasurements_ != serializeMeasurements(measurementDocument());
}

bool ViewerWidget::hasUnsavedEdits() const
{
    return hasFormEdits() || hasAnnotEdits() || hasMeasurementEdits();
}

} // namespace mervin
