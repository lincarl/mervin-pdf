#pragma once

#include "render/AnnotTypes.h"
#include "render/Document.h"
#include "render/FormTypes.h"
#include "render/GeometryTypes.h"
#include "render/MeasureModel.h"
#include "render/MeasureSnap.h"
#include "render/MeasureTypes.h"
#include "render/PageCache.h"
#include "render/PreviewLayer.h"
#include "render/RenderTypes.h"
#include "render/SelectionModel.h"
#include "render/TextIndex.h"
#include "render/ViewLayout.h"
#include "ui/MeasureTypes.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QStringList>
#include <QTransform>

#include <memory>
#include <optional>
#include <vector>

class QTimer;
class QRubberBand;

namespace mervin {

class RenderEngine;
class Document;
class FormModel;
class AnnotModel;
class AnnotPopup;
class PdfPropertiesPopup;

// The page canvas: a continuous-scroll viewer that renders pages on demand via
// the RenderEngine and paints cached pixmaps. Also owns the per-document text
// model (TextIndex) that backs find-in-page and text selection.
class ViewerWidget : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class ZoomMode { FitWidth, FitPage, Custom };
    Q_ENUM(ZoomMode)

    explicit ViewerWidget(RenderEngine *engine, QWidget *parent = nullptr);
    ~ViewerWidget() override;

    void setDocument(Document *doc);
    Document *document() const { return doc_; } // non-owning; for Print

    int pageCount() const;
    int currentPage() const { return currentPage_; } // 0-based
    double scale() const { return scale_; }
    ZoomMode zoomMode() const { return zoomMode_; }
    ViewLayout::Mode layoutMode() const { return layoutMode_; }
    PageTheme pageTheme() const { return pageTheme_; }
    int rotation() const { return rotation_; } // 0 / 90 / 180 / 270 degrees

    // The resume anchor: the page under the viewport's top-left corner, plus that
    // corner's position within the page as a fraction of its displayed size.
    // Scale-independent, so it restores at any zoom / window size. Anchored on the
    // CORNER page (not the centre page the readout shows) so the fraction stays a
    // clean within-page value and the round-trip is exact. {0} when there's no doc.
    struct ScrollAnchor
    {
        int page = 0;
        double fracX = 0.0;
        double fracY = 0.0;
    };
    // Paint accounting, for the zoom-preview guard rail (tst_viewer_preview):
    // how each visible page was drawn in the frames since resetPaintStats() -
    // from its sharp render, from a stretched preview, or with nothing at all to
    // put on the paper. `blank` is the zoom blink this feature exists to remove,
    // so a test can assert it stays at zero across a zoom instead of eyeballing
    // the window. `eased` counts pages drawn into an interpolated rect by the zoom
    // ease (see the ZoomEase comment in the .cpp) - it is orthogonal to the other
    // three, which still record where the pixels came from.
    struct PaintStats
    {
        int fresh = 0;
        int preview = 0;
        int blank = 0;
        int eased = 0;
    };
    PaintStats paintStats() const { return paintStats_; }
    void resetPaintStats() { paintStats_ = {}; }

    // --- zoom ease (the short animation that replaces the hard zoom cut) ---
    // Duration in ms for exactly one kZoomStep; scaled by the actual jump. 0
    // disables the ease outright (reduce-motion, and the control case in
    // tst_viewer_preview). Every zoom lands on the same scale either way - the
    // ease only decides where the frozen bitmaps are drawn for a few frames.
    void setZoomEaseMs(int ms);
    int zoomEaseMs() const { return zoomEaseMs_; }
    bool zoomEaseActive() const { return zoomEase_.active; }
    // Freeze the ease at linear progress `t` in [0,1] so a grabbed frame is
    // reproducible; t < 0 hands progress back to the clock. Test hook.
    void setZoomEaseProgressForTest(double t);
    // Which page a canvas point belongs to, as every click path resolves it.
    // Test hook: the off-page answer decides where a measurement vertex or a
    // comment lands, and nothing about a wrong one is visible on screen.
    int pageAtCanvasForTest(QPoint canvas) const { return pageAtCanvas(canvas); }

    ScrollAnchor scrollAnchor() const;
    // Restore an anchor captured by scrollAnchor(). Sticky: re-applied across the
    // resize/fit-scale settling that follows opening a file, so the spot lands
    // precisely even if the viewer is not yet at its final size; an explicit
    // navigation (goToPage) or a user scroll abandons it.
    void restorePageScrollFraction(int page, double fracX, double fracY);

    // Find-in-page.
    void startFind(const QString &query, bool caseSensitive, bool wholeWord);
    void clearFind();
    int matchCount() const { return static_cast<int>(matches_.size()); }
    // Per-tab search state, so the find bar can be restored when this tab is
    // re-selected (search is individual to each document tab).
    QString findQuery() const { return findQuery_; }
    bool findCaseSensitive() const { return findCaseSensitive_; }
    bool findWholeWord() const { return findWholeWord_; }
    int currentMatchNumber() const { return currentMatch_ >= 0 ? currentMatch_ + 1 : 0; } // 1-based; 0 = none

    // Text selection / copy.
    bool hasSelection() const { return selection_.hasSelection(); }
    QString selectedText() const;

    // Measuring tool. measureMode() means the *measure cursor* is active (the
    // crosshair places points); the tool can be enabled with the standard pointer
    // instead, in which case measureMode() is false but the panel + measurements
    // stay visible (see measureToolEnabled()).
    bool measureMode() const
    {
        return toolMode_ == ToolMode::Measure || toolMode_ == ToolMode::Calibrate;
    }
    // The measuring tool is on: the floating panel is shown and committed
    // measurements are drawn. Independent of which cursor is active.
    bool measureToolEnabled() const { return measureToolEnabled_; }
    MeasureKind measureKind() const { return measureKind_; }
    bool measureSnap() const { return measureSnap_; }
    // True when `page` resolves to a usable scale (embedded /Measure data or a
    // manual/calibrated override). A page without one can't be measured until
    // calibrated; measurement attempts there are redirected to calibration.
    bool pageHasScale(int page) const;
    // True when `page` carries an embedded PDF /Measure scale (ignoring any
    // manual/calibrated override sitting on top of it).
    bool pageHasEmbeddedScale(int page) const;
    // Apply a manual/calibrated scale override for one page (invalid clears it).
    void setPageScaleOverride(int page, const MeasureScale &scale);

    // --- saving / exporting / loading measurements ---
    // Read-out accessors used by the save/export/print flows.
    bool hasMeasurements() const { return !measurements_.empty(); }
    const std::vector<Measurement> &committedMeasurements() const { return measurements_; }
    const MeasureModel &measureOverrides() const { return measureOverrides_; }
    MeasureUnit measureUnit() const { return measureUnit_; }
    int measurePrecision() const { return measurePrecision_; }
    double measureLineWidth() const { return measureLineWidth_; }
    // Replace the current measurements/overrides with a persisted set loaded from
    // an opened PDF's Mervin blob, and turn the tool on so the marks show and are
    // editable. Called once right after setDocument().
    void loadMeasurements(std::vector<Measurement> measurements, MeasureModel overrides,
                          MeasureUnit unit, int precision, double lineWidth);

    // The floating measure panel that overlaps the page. The viewer uses it to
    // reject presses that fall over the panel and to coordinate the popup-replay
    // guard. Non-owning; the panel outlives the viewer's use of it.
    void setMeasurePanel(QWidget *panel) { measurePanel_ = panel; }
    // The floating Comment panel (same guard purpose: a press over it must never
    // place an annotation). Non-owning.
    void setAnnotPanel(QWidget *panel) { annotPanel_ = panel; }

    // --- form filling (AcroForm) ---
    // The form-fill tool is active (Ctrl+Shift+F): fillable fields are highlighted
    // and clickable, with inline editors over text/choice fields. Mutually
    // exclusive with OCR / Measure.
    bool formMode() const { return toolMode_ == ToolMode::FillForms; }
    // True when the open document carries fillable AcroForm fields (the model was
    // built). Drives the Fill-Forms action's enabled state.
    bool hasFormFields() const { return formModel_ != nullptr; }
    bool highlightFormFields() const { return highlightFormFields_; }
    // True when a field has been filled / changed since open or the last save.
    bool hasFormEdits() const;
    // The form model (nullptr for a non-form document). Used by the save / print
    // flows to persist filled values. Non-owning view.
    FormModel *formModel() const { return formModel_.get(); }
    // Commit any in-progress inline edit (called before save / print so the model
    // carries the latest typed value).
    void commitActiveFormEditor();
    // Clear the form's dirty flag after a successful save.
    void clearFormDirty();

    // --- annotations (highlight / underline / strike-out + sticky-note comments) ---
    // The single active page gesture is the markup (highlight) gesture: a drag
    // marks up selected text. (Sub-mode "Markup" in the Comment panel.)
    bool highlightMode() const { return toolMode_ == ToolMode::Highlight; }
    // The active page gesture is the sticky-note gesture: a click drops a note.
    bool commentMode() const { return toolMode_ == ToolMode::Comment; }
    bool annotationMode() const { return highlightMode() || commentMode(); }
    // The Comment tool window is open. Independent of whether its gesture is the
    // active one (the Measure tool can hold the single active gesture while the
    // Comment panel stays docked alongside the measure panel).
    bool commentToolEnabled() const { return commentToolEnabled_; }
    // True for any PDF document (every PDF can carry annotations), so the Highlight
    // / Comment actions are enabled. False for non-PDF documents.
    bool hasAnnotationSupport() const { return annotModel_ != nullptr; }
    // True when an annotation has been created/edited/removed since open or the
    // last save.
    bool hasAnnotEdits() const;
    AnnotModel *annotModel() const { return annotModel_.get(); } // non-owning
    // Every annotation in the document (for the comments sidebar).
    std::vector<Annotation> allAnnotations() const;
    void clearAnnotDirty();
    // Commit any pending comment edit in the open inline editor (called before
    // save / print so the model carries the latest typed text).
    void commitActiveAnnotEditor();
    // The author name stamped on new annotations (from settings).
    void setAnnotAuthor(const QString &author) { annotAuthor_ = author; }

public slots:
    void setZoomMode(ZoomMode mode);
    void setScale(double scale); // switches to Custom
    void zoomIn();
    void zoomOut();
    void rotateLeft();
    void rotateRight();
    void setRotation(int degrees); // absolute; snapped to 0/90/180/270
    // The two page-layout axes are set independently: turning the spread on never
    // disturbs the scrolling choice, and vice versa. setLayoutMode() moves both at
    // once (session/settings restore), which is why it is one function and not
    // two calls - two calls would relayout and re-fit twice.
    void setScrollMode(ViewLayout::Scroll scroll);
    void setSpread(bool on);
    void setLayoutMode(ViewLayout::Mode mode);
    void setPageTheme(PageTheme theme);
    void goToPage(int pageNo); // 0-based
    void nextPage();
    void prevPage();
    void findNext();
    void findPrev();
    void copySelection();
    void selectAll();
    void clearSelection();
    // Begin "OCR selection" mode (Ctrl+Shift+O): the next drag rubber-bands a
    // region and emits ocrRegionSelected, then the mode ends automatically.
    void setOcrMode(bool on);

    // Form-fill tool (Ctrl+Shift+F): mutually exclusive with OCR / Measure. When
    // on, fillable fields are tinted (if highlightFormFields), text/choice fields
    // get inline editors, and clicks toggle check boxes / radios.
    void setFormMode(bool on);
    void setHighlightFormFields(bool on);
    // When on (the default), opening a document that has fillable fields enters
    // form-fill mode automatically (applied in setDocument). User setting.
    void setAutoFormFill(bool on) { autoFormFill_ = on; }

    // Comment tool: opens/closes the floating Comment panel (toolbar button). The
    // panel docks alongside the measure panel and they can both be open at once;
    // only the SINGLE active gesture is exclusive (managed via setAnnotSubMode /
    // the measure cursor). Disables OCR / Forms (those stay exclusive). Enabling
    // arms the Markup sub-mode by default.
    void setCommentToolEnabled(bool on);
    // The active annotation sub-mode chosen in the Comment panel: Select (pointer,
    // no annotation gesture), Markup (drag highlights text), Note (click drops a
    // sticky note). Activating Markup/Note takes the single active gesture from the
    // measure tool; Select releases it. No-op unless the Comment tool is open.
    void setAnnotSubMode(AnnotSubMode mode);
    void setMarkupStyle(AnnotType type);      // Highlight / Underline / StrikeOut for new marks
    void setMarkupColor(const QColor &color); // colour for new markups AND new sticky notes
    // Jump to an annotation (from the comments sidebar) and open its inline editor.
    void revealAnnotation(int page, int id);

    // Measuring tool (mutually exclusive with OCR / text selection).
    void setMeasureMode(bool on); // enable/disable the whole tool (panel + overlays)
    // Switch the active cursor while the tool stays enabled: true = the measuring
    // crosshair (clicks place points), false = the standard pointer (text
    // selection; clicks no longer measure). A no-op when the tool is disabled.
    void setMeasureCursorActive(bool active);
    void setMeasureKind(MeasureKind kind);
    void setMeasureUnit(MeasureUnit unit);
    void setMeasurePrecision(int decimals);
    void setMeasureLineWidth(double width); // measurement stroke width (points)
    void setMeasureSnap(bool on);  // snap endpoints to CAD vertices/edges
    void beginCalibration();      // next drawn line becomes a calibration
    void cancelCalibration();     // abandon a pending calibration line
    // Set the scale manually (no line): abandon any pending calibration / in-progress
    // vector, then emit setScaleRequested(pageNo) for the page whose scale the panel
    // is showing, so MainWindow can pop the Set Scale (ratio) dialog.
    void promptSetScale();
    // Discard the manual/calibrated override on the page whose scale the panel is
    // currently showing, so its embedded PDF scale takes over again. A no-op when
    // that page has no override or no embedded scale to fall back to (a scale-less
    // page keeps its calibration - it's the only scale it has).
    void resetPageScale();
    void clearMeasurements();     // drop all committed + in-progress measurements
    void removeMeasurement(int index); // drop one committed measurement (list X button)
    void copyMeasurementValue(int index); // copy one measurement's formatted value to the clipboard
    // Emphasise (a thicker stroke) the committed measurement whose panel row is
    // hovered; hovered=false or an out-of-range index clears the emphasis.
    void onMeasurementHovered(int index, bool hovered);
    // The measure panel's unit dropdown just closed: arm a one-shot guard so the
    // synthetic press Qt replays over the page does not drop a stray point.
    void notifyMeasurePanelPopupClosed();

signals:
    void pageChanged(int current, int total); // 1-based current
    void scaleChanged(double scale);
    void zoomModeChanged(ViewerWidget::ZoomMode mode);
    void layoutModeChanged(ViewLayout::Mode mode);
    void findStatusChanged(int current, int total); // current 1-based (0 = none)
    void ocrRegionSelected(int pageNo, const QRectF &pageRect); // page-point rect
    void contextMenuRequested(const QPoint &globalPos); // right-click on the page

    // Form filling.
    void formModeChanged(bool on); // tool enabled / disabled (button + menu sync)
    void formEditsChanged();       // a field value was filled / changed (enables Save)

    // Annotations.
    void commentToolEnabledChanged(bool on); // Comment panel shown/hidden (toolbar + dock)
    void annotSubModeChanged(AnnotSubMode mode); // active sub-mode (panel selector sync)
    void annotEditsChanged();           // an annotation was added/edited/removed (enables Save)
    void annotationsChanged();          // the document's annotation set changed (sidebar refresh)

    // Measuring tool.
    void measureModeChanged(bool on);                 // tool enabled/disabled (panel show/hide)
    void measureCursorActiveChanged(bool active);     // crosshair vs standard pointer (e.g. via Esc)
    void measurementReadout(const QString &text);     // live + committed value
    void measureScaleChanged(const QString &description); // resolved scale at the cursor
    // The displayed scale can/can't be reset: true when the shown page has an
    // embedded PDF scale that a manual/calibrated override is masking. Drives the
    // panel's "Reset" button visibility.
    void measureScaleResettableChanged(bool resettable);
    void calibrationLineDrawn(int pageNo, double lengthPoints); // a calibration line was drawn
    void setScaleRequested(int pageNo); // the "Set Scale" button: prompt for a manual ratio
    // The committed-measurement list changed (added/removed/edited/reformatted);
    // carries each measurement's formatted value, in measurement order. The
    // measure panel mirrors this into its scrollable list.
    void measurementsChanged(const QStringList &items);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    // Intercept the viewport's native pinch-zoom gesture (trackpad) so it zooms
    // toward the cursor like Ctrl+wheel; everything else falls through to the base.
    bool viewportEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    // Catches focus-out (commit) and Tab/Enter/Esc on the inline form editors.
    bool eventFilter(QObject *watched, QEvent *event) override;
    // In form mode, Tab / Shift+Tab walk the fillable fields instead of Qt's child
    // focus chain.
    bool focusNextPrevChild(bool next) override;

private slots:
    void onResultReady(const mervin::RenderResult &result);
    void onAutoScroll();
    void onZoomEaseTick();

private:
    void relayout();
    void applyFitScale();
    // Apply the pending scroll-fraction restore (if any) at the current scale.
    void applyPendingRestore();
    // Request a render of `pageNo` covering at least `neededCanvas` (canvas-space
    // visible region). Small pages render whole; pages whose full bitmap exceeds
    // the tiling budget render only the visible band (deep-zoom clipping).
    void ensureRendered(int pageNo, const QRect &neededCanvas);
    // What happens to the frozen preview images (see PreviewLayer) when the
    // cached renders are thrown away: Keep for a change that only re-lays the
    // same page content out (scale, fit mode, device pixel ratio, page mode), so
    // the old images can be stretched over the gap; Drop when the pixels
    // themselves would be wrong (rotation flips the page's aspect, a page-theme
    // change re-tones it, a new document replaces it outright).
    enum class PreviewPolicy { Keep, Drop };
    void invalidateRenders(PreviewPolicy preview);
    // Freeze what we have for the pages at/near the viewport into preview_.
    // Must run BEFORE cache_.clear() and before relayout(), while layout_ still
    // holds the rects those images were rendered for.
    void seedPreview();
    // Paint page `pageNo`'s frozen preview stretched into `easedCanvas`, as a
    // stand-in until the sharp render lands. Returns whether anything was drawn.
    // `pageCanvas` is the page's rect in the CURRENT layout and `easedCanvas` where
    // it is actually drawn this frame - the two differ only while the zoom ease is
    // running. The magnification guard is judged on `pageCanvas`, so its verdict
    // holds for a whole gesture instead of flipping part way through.
    bool drawPreview(QPainter &p, int pageNo, const QRect &pageCanvas, const QPoint &off,
                     const QRect &easedCanvas) const;
    void updateScrollBars();
    void updateCurrentPage();
    QPoint scrollOffset() const;
    QPoint centerDelta() const;   // px to centre content smaller than the viewport
    QPoint contentOffset() const; // content->viewport translation (scroll minus centring)
    double clampScale(double s) const;
    // The zoom level one gesture away from `current`: the next rung of the zoom
    // ladder up (dir > 0) or down (dir < 0), skipping any rung too close to be
    // worth a gesture. Returns the clamp at either end, which the caller turns into
    // a no-op. Used by zoomIn/zoomOut (buttons, menu, Ctrl+= / Ctrl+-) and by the
    // Ctrl+wheel branch; the zoom box and the trackpad pinch bypass it.
    double nextZoomLevel(double current, int dir) const;
    // Re-scale to `newScale` while keeping the document point under `viewportPos`
    // pinned there. The wheel and pinch pass the cursor (zoom toward the cursor);
    // setScale - the toolbar, menu, keyboard and zoom box - passes
    // viewportCenter(), so a zoom with no cursor behind it holds the middle of
    // the view still instead of jumping to the top of the page.
    void zoomAtViewportPos(double newScale, QPointF viewportPos);
    // The shared core of every zoom: re-lay the document out at `newScale` with
    // the document point under `viewportPos` pinned there. Leaves zoomMode_ alone
    // and emits nothing - its three callers own both.
    void rescaleKeeping(double newScale, QPointF viewportPos);
    QPointF viewportCenter() const;

    // --- zoom ease -----------------------------------------------------------
    // Purely visual: the zoom itself is already complete when the ease starts, so
    // the ease can never leave the viewer in a wrong state. See the block comment
    // above captureZoomEase() in the .cpp for the whole story.
    struct ZoomEase
    {
        bool active = false;
        // page -> the viewport rect it was being DRAWN at when this ease began.
        QHash<int, QRectF> from;
        int repPage = -1;      // stands in for pages the new layout reveals
        double k = 1.0;        // repPage's final width / its captured width
        int durMs = 0;
        double forcedT = -1.0; // test hook; < 0 means clock-driven
        QElapsedTimer clock;
    };
    bool zoomEaseAllowed() const;
    // Snapshot where the pages are drawn right now. False when the ease is not
    // wanted (see zoomEaseAllowed) or there is nothing on screen to capture.
    bool captureZoomEase(double newScale, ZoomEase *out) const;
    void startZoomEase(ZoomEase &&e); // no-op when the jump is too small to bother
    void endZoomEase();
    double zoomEaseU() const;                                       // eased progress, 0..1
    QRectF zoomEaseFromRect(int pageNo, const QRectF &finalRect) const;
    QRectF zoomEaseRect(int pageNo, const QRectF &finalRect) const; // where to draw it now
    // Maps a page's final rect onto the rect it is actually being drawn into, so the
    // page border and every overlay positioned from layout_ travels with the bitmap.
    // Takes the eased rect rather than deriving it, so it agrees exactly with what
    // was blitted (paintEvent snaps that to whole pixels).
    QTransform zoomEaseTransform(const QRectF &finalRect, const QRectF &easedRect) const;
    // Append pages that are on screen only because the ease is drawing them
    // smaller/larger than the final layout would.
    void addPagesHeldByEase(std::vector<int> *pages) const;

    // Coordinate mapping between page-point space (TextIndex) and the widget.
    QRectF pageRectToCanvas(int pageNo, const QRectF &pageRect) const;
    QRectF pageRectToWidget(int pageNo, const QRectF &pageRect) const;
    QPointF pagePointToCanvas(int pageNo, QPointF pagePoint) const;
    QPointF pagePointToWidget(int pageNo, QPointF pagePoint) const;
    // Map a widget/canvas point to page-point space. Clamped to the page rect by
    // default (endpoints/snaps stay on the page); pass clampToPage=false for the
    // free-floating value label, which may be parked in the margin.
    QPointF canvasToPagePoint(int pageNo, QPointF canvas, bool clampToPage = true) const;
    int pageAtCanvas(QPoint canvas) const;
    TextPos posAt(QPoint viewportPos) const;
    QString externalLinkAt(QPoint viewportPos) const;
    void showLinkToolTip(const QString &url, QPoint globalPos);
    void clearLinkToolTip();
    std::optional<PdfLinkTarget> pdfLinkAt(QPoint viewportPos) const;
    std::optional<PdfItemProperties> itemPropertiesAt(QPoint viewportPos) const;
    bool activatePdfLink(const PdfLinkTarget &target);
    bool openExternalLink(const QString &url);
    bool openItemProperties(QPoint viewportPos);
    void closePropertiesPopup();
    void syncPropertiesPopup();

    // Measuring helpers.
    void handleMeasureClick(QPoint viewportPos);
    // When the tool is enabled on a document that carries no usable scale (no
    // embedded /Measure data and no override) and nothing has been measured yet,
    // arm calibration so the user's first drawn line establishes the scale.
    void maybeAutoCalibrate();
    void commitInProgress();
    void cancelInProgressMeasure();
    void finishPolyOrArea();
    bool pressIsOverMeasurePanel(QMouseEvent *event) const; // press lands on the panel
    // Shared gesture-press guard for the measure / highlight / comment branches: a
    // synthetic dropdown-dismiss replay, a still-open popup, or a press physically
    // over either docked tool panel must never place a mark/point. Consumes the
    // one-shot replay window when it fires.
    bool swallowToolPress(QMouseEvent *event);
    std::vector<QPointF> previewPts() const; // in-progress points + live hover point
    MeasureScale resolvedScale(int page, QPointF pagePoint) const;
    QString formatMeasurement(int page, MeasureKind kind, const std::vector<QPointF> &pts) const;
    void emitMeasurementsChanged(); // push the formatted committed list to listeners
    // Editing committed measurements: hit-test the press against vertex handles
    // (preferred) then value labels; returns true with the hit identified.
    bool measureHitTest(QPoint viewportPos, int &measureIdx, int &vertexIdx, bool &onLabel) const;
    QPointF labelAnchorWidget(const Measurement &m) const; // unclamped label centre (widget space)
    QRectF labelRectForAnchor(const QString &text, QPointF anchorWidget) const; // clamped pill rect
    QRectF labelRectFor(const Measurement &m) const; // the drawn label's widget rect (or invalid)
    QString scaleDescription(int page, QPointF pagePoint) const;
    // True when `page`'s scale could be reset to the PDF's embedded one: it has an
    // embedded scale AND a manual/calibrated override masking it.
    bool canResetScale(int page) const;
    void updateHoverScale(int page, QPointF pagePoint);
    // Snap the page point `pp` (on `page`) to the nearest CAD vertex/edge when
    // snapping is on; returns the (possibly unchanged) point and records the live
    // snap target for the indicator. A no-op returning `pp` when snap is off.
    QPointF snapPagePoint(int page, QPointF pp);
    void ensureMeasureGeometry(int page); // lazily load + cache `page`'s geometry
    void clearSnapState();
    void drawMeasurements(QPainter &p, int pageNo) const;
    void drawSnapIndicator(QPainter &p) const;
    // `pointXf` maps widget points during a zoom ease (identity otherwise): the
    // stroke widths, handle radii and value pill are screen-space chrome, so the
    // ease is applied to the geometry here instead of to the painter.
    void paintShape(QPainter &p, int pageNo, MeasureKind kind,
                    const std::vector<QPointF> &pagePts, const QColor &accent, bool inProgress,
                    bool hasLabelOverride = false, QPointF labelOverridePage = {},
                    bool isHovered = false, const QTransform &pointXf = {}) const;
    void drawLabelPill(QPainter &p, QPointF anchor, const QString &text) const;
    // The paper colour the active document theme produces, and a legible ink for
    // it. Overlays drawn ON the page (the measurement value pill, unrendered page
    // backing) must key off these rather than the chrome palette: a dark-chrome
    // pill on white paper is the wrong way round.
    QColor pagePaper() const;
    QColor pageInk() const;

    // Form-fill helpers.
    void rebuildFormModel();              // (re)create formModel_ for the current document
    void syncFormEditors();               // create/position/hide editors for visible pages
    void destroyFormEditors();
    QWidget *createFormEditor(const FormField &f, int page, int fieldIndex);
    void commitFormEditor(int editorIndex); // push the editor's value into the model
    int editorIndexFor(QObject *widget) const;
    void applyFormFieldChange(int page);  // erase the page image + re-render + repaint
    const std::vector<std::pair<int, int>> &formFieldOrder(); // (page,fieldIndex), Tab order
    void focusFormFieldAt(int orderIndex);  // scroll into view + focus editor / mark toggle
    void advanceFormFocus(int delta);       // Tab (+1) / Shift+Tab (-1)
    void drawFormHighlights(QPainter &p, int pageNo) const;
    // The editable field under a viewport position (for click-to-toggle / focus).
    bool formFieldAt(QPoint viewportPos, int &page, int &fieldIndex) const;

    // Annotation helpers.
    void rebuildAnnotModel();                   // (re)create annotModel_ for the current doc
    // The managed annotation under a viewport position (topmost wins), or false.
    bool annotAt(QPoint viewportPos, int &page, int &id) const;
    // Whether a plain pointer click on this annotation (Comment tool closed) should
    // open the read-only comment viewer: sticky notes always, plus any mark that
    // carries comment text. Comment-less highlights are skipped.
    bool annotShowsReadOnly(int page, int id) const;
    void createHighlightFromSelection();        // selection -> markup, then open its editor
    void createCommentAt(QPoint viewportPos);   // drop a sticky note + open its editor
    // Show the inline editor for an annotation. `readOnly` forces a view-only card
    // (no swatches, no delete, no text editing) - used when the Comment tool is
    // closed, so a comment can be read without entering an editing mode.
    void openAnnotPopup(int page, int id, bool readOnly = false);
    void closeAnnotPopup();                     // commit + hide the inline editor
    void syncAnnotPopup();                      // reposition the open editor (scroll/zoom)
    void applyAnnotChange(int page);            // erase page image + re-render + notify
    QRectF annotWidgetRect(int page, int id) const; // the annotation's rect in viewport coords
    void drawAnnotSelection(QPainter &p, int pageNo) const; // outline the open annotation
    // Single-active-gesture coordination between the measure and comment tools
    // (both panels can be docked at once, but only one gesture is live):
    // idleMeasureCursor() stops the measuring crosshair (panel + marks stay) and
    // tells the measure panel; idleAnnotGesture() drops the annotation gesture
    // (closes the popup, clears selection) and tells the comment panel.
    void idleMeasureCursor();
    void idleAnnotGesture();

    // Find helpers.
    void rebuildMatchIndex();
    void scrollToMatch(int matchIndex);
    void ensureCanvasRectVisible(const QRectF &canvasRect);

    // Selection drag autoscroll.
    void maybeAutoScroll(QPoint viewportPos);
    void stopAutoScroll();

    RenderEngine *engine_;
    Document *doc_ = nullptr;
    ViewLayout layout_;
    PageCache cache_;
    // Last-good page images kept across a zoom so the viewer stretches them
    // instead of flashing blank paper while the new renders arrive.
    PreviewLayer preview_;
    std::unique_ptr<TextIndex> textIndex_;

    double scale_ = 1.0;
    int rotation_ = 0;
    double dpr_ = 1.0;
    ZoomMode zoomMode_ = ZoomMode::FitWidth;
    ViewLayout::Mode layoutMode_;
    PageTheme pageTheme_ = PageTheme::Light;

    // Unique per viewer instance. The RenderEngine broadcasts every result to
    // all viewers, so each request carries this id and the matching result is
    // routed back here; results addressed to other viewers are ignored.
    quint64 viewerId_ = 0;
    quint64 viewEpoch_ = 0;
    quint64 nextToken_ = 1;
    // In-flight render per page: the token of the request we last issued (so a
    // superseded, out-of-order result can be discarded) and the canvas-space
    // region it will cover (so we don't re-request what is already coming).
    struct PendingRender
    {
        quint64 token = 0;
        QRect region;
    };
    QHash<int, PendingRender> pending_;
    PaintStats paintStats_;
    int currentPage_ = 0;

    // Sticky scroll-fraction restore (resume-where-you-left-off). While
    // pendingRestore_ is set, the target page + fraction is re-applied after every
    // resize/show-driven re-fit so the spot survives the layout settling that
    // follows opening a file; the first genuine user scroll clears it.
    // restoring_ guards scrollContentsBy from mistaking our own programmatic
    // scrolls (and resize-induced clamps) for user input.
    bool pendingRestore_ = false;
    bool restoring_ = false;
    // Set while goToPage() writes the scrollbars. Those writes reach
    // scrollContentsBy -> updateCurrentPage synchronously, which would otherwise
    // decide the current page from the scroll position and emit its own
    // pageChanged before goToPage emits the page actually asked for.
    bool navigating_ = false;
    int pendingPage_ = 0;
    double pendingFracX_ = 0.0;
    double pendingFracY_ = 0.0;

    // Find state.
    std::vector<TextMatch> matches_;
    QHash<int, QList<int>> matchesByPage_; // page -> indices into matches_
    int currentMatch_ = -1;
    QString findQuery_;
    bool findCaseSensitive_ = false;
    bool findWholeWord_ = false;

    // Selection state.
    SelectionModel selection_;
    bool selecting_ = false;
    QString hoveredLinkToolTip_;
    QString pressedExternalLink_;
    std::optional<PdfLinkTarget> pressedPdfLink_;
    std::optional<PdfItemProperties> pressedProperties_;
    QPoint lastMouseViewportPos_;
    QTimer *autoScrollTimer_ = nullptr;
    int autoScrollDy_ = 0;

    // Zoom ease state. zoomEaseSuppress_ is set around a single zoom call by input
    // that is already continuous (trackpad pinch, a high-resolution wheel tick):
    // easing those would only make direct input feel late.
    ZoomEase zoomEase_;
    QTimer *zoomEaseTimer_ = nullptr;
    int zoomEaseMs_ = 130;
    bool zoomEaseSuppress_ = false;
    // Ctrl+wheel distance not yet spent on a ladder rung (see wheelEvent), so a
    // high-resolution wheel steps once per notch travelled rather than per event.
    int wheelZoomAccum_ = 0;

    // Middle-button panning (press-drag-release). Works in any tool mode and
    // takes precedence over selection/measure/OCR for the middle button.
    bool panning_ = false;
    QPoint panStartViewportPos_; // cursor position when the pan began
    QPoint panStartScroll_;      // scroll offset when the pan began

    // Interaction tool. Only one is active at a time; None means text selection.
    enum class ToolMode { None, Ocr, Measure, Calibrate, FillForms, Highlight, Comment };
    ToolMode toolMode_ = ToolMode::None;
    // The measuring tool is on: panel shown, committed measurements drawn. The
    // cursor sub-mode (crosshair vs standard pointer) lives in toolMode_ -
    // ToolMode::Measure/Calibrate while the tool is enabled means the crosshair is
    // active; ToolMode::None while enabled means the standard pointer.
    bool measureToolEnabled_ = false;

    // OCR rubber-band selection (Ctrl+Shift+O).
    QPoint ocrOrigin_;
    QRubberBand *rubberBand_ = nullptr;

    // Measuring tool state.
    MeasureKind measureKind_ = MeasureKind::Distance;
    MeasureUnit measureUnit_ = MeasureUnit::Millimeter;
    int measurePrecision_ = 2;
    double measureLineWidth_ = 2.0;         // stroke width (points) for drawn marks
    std::vector<Measurement> measurements_; // committed, page-point space
    std::vector<QPointF> inProgress_;       // current vertices, page-point space
    int inProgressPage_ = -1;               // page the current draw is locked to
    QPointF hoverPagePoint_;                // live cursor in page space (preview)
    bool hoverValid_ = false;
    MeasureModel measureOverrides_;         // per-page manual/calibrated scales
    QString lastScaleDesc_;                 // last emitted scale description (de-dupe)
    int scalePage_ = -1;                    // page whose scale the panel currently shows (Reset target)
    int lastScaleResettable_ = -1;          // last emitted resettable state (-1 unknown / 0 / 1), de-dupe
    int hoveredMeasurementIndex_ = -1;      // measurement emphasised via a panel-row hover (or -1)

    // Dragging a committed measurement: a vertex (re-snaps, value updates) or its
    // value label (re-positioned). Active only between press and release.
    enum class MeasureDrag { None, Vertex, Label };
    MeasureDrag measureDrag_ = MeasureDrag::None;
    int dragMeasureIdx_ = -1;
    int dragVertexIdx_ = -1;
    QPointF dragLabelGrabPage_; // label-centre minus cursor at grab (page space)

    // Snapping to CAD vertices/edges.
    bool measureSnap_ = true;
    PageGeometry measureGeo_;               // cached geometry for measureGeoPage_
    int measureGeoPage_ = -1;
    bool snapValid_ = false;                // a live snap target is shown
    int snapPage_ = -1;
    QPointF snapPagePoint_;                 // snapped point (page space) for the indicator
    snap::SnapType snapType_ = snap::SnapType::None;

    // Bug guard: clicks belonging to the floating panel must never place a point.
    QPointer<QWidget> measurePanel_;        // non-owning; the page-overlapping panel
    QPointer<QWidget> annotPanel_;          // non-owning; the Comment panel (press guard)
    QElapsedTimer sincePanelPopupClosed_;   // started when the unit dropdown closes

    // Form-fill (AcroForm) state. formModel_ is null for non-form documents.
    std::unique_ptr<FormModel> formModel_;
    bool highlightFormFields_ = true;
    bool autoFormFill_ = true; // auto-enter form mode on opening a doc with fields
    // One live inline editor over a visible text/choice field. Re-positioned on
    // scroll/zoom/rotation; created lazily for pages in the viewport and torn down
    // when their page scrolls away. (Check boxes / radios are not editors.)
    struct FormEditor
    {
        int page = -1;
        int fieldIndex = -1;
        FormFieldType type = FormFieldType::Text;
        QWidget *widget = nullptr; // QLineEdit / QPlainTextEdit / QComboBox / QListWidget
    };
    std::vector<FormEditor> formEditors_;
    bool syncingFormEditors_ = false; // re-entrancy guard for syncFormEditors()
    // Tab / Shift+Tab order: every editable field, page then field order. Built
    // lazily on first Tab and dropped when the document changes.
    std::vector<std::pair<int, int>> formFieldOrder_;
    bool formFieldOrderBuilt_ = false;
    int formFocusIndex_ = -1; // index into formFieldOrder_ (or -1 for none)

    // Annotation state. annotModel_ is non-null for any PDF document.
    // commentToolEnabled_ tracks whether the Comment panel is open (independent of
    // the active gesture). markupStyle_ and markupColor_ are driven by the Comment
    // panel; markupColor_ is the single shared colour applied to NEW text markups
    // AND NEW sticky-note comments (the panel has one swatch row). annotAuthor_
    // stamps the /T author. The inline editor (annotPopup_) is a lazily-created
    // child of viewport() that edits the annotation identified by
    // openAnnotPage_/openAnnotId_ (both -1 when closed).
    std::unique_ptr<AnnotModel> annotModel_;
    bool commentToolEnabled_ = false;
    AnnotType markupStyle_ = AnnotType::Highlight;
    QColor markupColor_ = annot::defaultColor();
    QString annotAuthor_;
    AnnotPopup *annotPopup_ = nullptr;
    int openAnnotPage_ = -1;
    int openAnnotId_ = -1;

    PdfPropertiesPopup *propertiesPopup_ = nullptr;
    std::optional<PdfItemProperties> openProperties_;
};

} // namespace mervin
