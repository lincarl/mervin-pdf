#include "ui/TabPage.h"

#include "config/Settings.h"
#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/MeasureMath.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "ui/AnnotPanel.h"
#include "ui/MeasurePanel.h"
#include "ui/PanelStack.h"
#include "ui/ViewerWidget.h"

#include <QFileInfo>
#include <QVBoxLayout>

namespace mervin {

namespace {
MeasureKind kindFromString(const QString &s)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("path") || t == QLatin1String("polyline"))
        return MeasureKind::Polyline;
    if (t == QLatin1String("area"))
        return MeasureKind::Area;
    if (t == QLatin1String("angle"))
        return MeasureKind::Angle;
    return MeasureKind::Distance;
}
} // namespace

TabPage::TabPage(RenderEngine *engine, QWidget *parent)
    : QWidget(parent)
    , engine_(engine)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    viewer_ = new ViewerWidget(engine_, this);
    layout->addWidget(viewer_, 1);

    // Floating measure controls: parented to the viewport so they overlap the
    // page (and don't scroll with content). Hidden until measure mode turns on.
    measurePanel_ = new MeasurePanel(viewer_->viewport());
    measurePanel_->hide();
    viewer_->setMeasurePanel(measurePanel_); // let the viewer reject clicks over it

    // Permanent panel <-> viewer wiring (independent of which window owns the
    // tab), so the panel follows the tab through detach/adopt.
    connect(measurePanel_, &MeasurePanel::kindChanged, viewer_, &ViewerWidget::setMeasureKind);
    connect(measurePanel_, &MeasurePanel::unitChanged, viewer_, &ViewerWidget::setMeasureUnit);
    connect(measurePanel_, &MeasurePanel::precisionChanged, viewer_,
            &ViewerWidget::setMeasurePrecision);
    connect(measurePanel_, &MeasurePanel::lineWidthChanged, viewer_,
            &ViewerWidget::setMeasureLineWidth);
    connect(measurePanel_, &MeasurePanel::cursorActiveChanged, viewer_,
            &ViewerWidget::setMeasureCursorActive);
    connect(viewer_, &ViewerWidget::measureCursorActiveChanged, measurePanel_,
            &MeasurePanel::setCursorActive);
    connect(measurePanel_, &MeasurePanel::popupDismissed, viewer_,
            &ViewerWidget::notifyMeasurePanelPopupClosed);
    connect(measurePanel_, &MeasurePanel::calibrateRequested, viewer_,
            &ViewerWidget::beginCalibration);
    connect(measurePanel_, &MeasurePanel::setScaleRequested, viewer_,
            &ViewerWidget::promptSetScale);
    connect(measurePanel_, &MeasurePanel::resetRequested, viewer_,
            &ViewerWidget::resetPageScale);
    connect(measurePanel_, &MeasurePanel::clearRequested, viewer_,
            &ViewerWidget::clearMeasurements);
    connect(measurePanel_, &MeasurePanel::closeRequested, viewer_,
            [this] { viewer_->setMeasureMode(false); });
    connect(viewer_, &ViewerWidget::measureScaleChanged, measurePanel_,
            &MeasurePanel::setScaleText);
    connect(viewer_, &ViewerWidget::measureScaleResettableChanged, measurePanel_,
            &MeasurePanel::setResetVisible);
    connect(viewer_, &ViewerWidget::measurementReadout, measurePanel_, &MeasurePanel::setReadout);
    connect(viewer_, &ViewerWidget::measurementsChanged, measurePanel_,
            &MeasurePanel::setMeasurements);
    // Queued: the X button lives inside the list row that removeMeasurement()
    // rebuilds (QListWidget::clear deletes the row widgets). Deferring the call
    // past the button's clicked() emission avoids deleting the sender mid-signal.
    connect(measurePanel_, &MeasurePanel::removeMeasurementRequested, viewer_,
            &ViewerWidget::removeMeasurement, Qt::QueuedConnection);
    connect(measurePanel_, &MeasurePanel::copyMeasurementRequested, viewer_,
            &ViewerWidget::copyMeasurementValue);
    connect(measurePanel_, &MeasurePanel::measurementHovered, viewer_,
            &ViewerWidget::onMeasurementHovered);
    // The Comment tool window (style + colour for highlights and notes). Created
    // before the PanelStack so both panels can register with the dock below.
    annotPanel_ = new AnnotPanel(viewer_->viewport());
    annotPanel_->hide();
    viewer_->setAnnotPanel(annotPanel_); // let the viewer reject page presses over it
    connect(annotPanel_, &AnnotPanel::modeChanged, viewer_, &ViewerWidget::setAnnotSubMode);
    connect(annotPanel_, &AnnotPanel::highlightStyleChanged, viewer_, &ViewerWidget::setMarkupStyle);
    connect(annotPanel_, &AnnotPanel::closeRequested, viewer_,
            [this] { viewer_->setCommentToolEnabled(false); });
    // Keep the panel's mode selector in sync when the active gesture is taken over
    // by the measuring tool (annotSubModeChanged(Select)) or set programmatically.
    connect(viewer_, &ViewerWidget::annotSubModeChanged, annotPanel_, &AnnotPanel::setMode);

    // Dock: stack the measure + comment panels vertically (measure on top), drag
    // them as a group, top-right by default. Both can be open at once; show/hide is
    // signal-driven so the dock reflows on any open/close order (incl. a document
    // switch, which resets tool state and emits the *Changed signals below).
    panelStack_ = new PanelStack(viewer_->viewport(), this);
    measurePanel_->setStack(panelStack_);
    annotPanel_->setStack(panelStack_);
    panelStack_->addPanel(measurePanel_);
    panelStack_->addPanel(annotPanel_);
    connect(viewer_, &ViewerWidget::measureModeChanged, this, [this](bool on) {
        measurePanel_->setVisible(on);
        panelStack_->relayout(); // re-stack so the comment panel reflows up/down
    });
    connect(viewer_, &ViewerWidget::commentToolEnabledChanged, this, [this](bool on) {
        annotPanel_->setVisible(on);
        panelStack_->relayout();
    });

    // Seed the panel + viewer from the saved measurement defaults.
    const Settings s = Settings::load();
    const MeasureUnit unit = measure::unitFromString(s.measurementUnit, MeasureUnit::Millimeter);
    const MeasureKind kind = kindFromString(s.measurementType);
    measurePanel_->setUnit(unit);
    measurePanel_->setPrecision(s.measurementPrecision);
    measurePanel_->setLineWidth(s.measurementLineWidth);
    measurePanel_->setKind(kind);
    viewer_->setMeasureUnit(unit);
    viewer_->setMeasurePrecision(s.measurementPrecision);
    viewer_->setMeasureLineWidth(s.measurementLineWidth);
    viewer_->setMeasureKind(kind);
    viewer_->setMeasureSnap(s.measurementSnap);
    // Seed form-fill settings before the document opens, so setDocument can honour
    // them (auto-enter form mode, field highlighting).
    viewer_->setAutoFormFill(s.autoFormFill);
    viewer_->setHighlightFormFields(s.highlightFormFields);

    // Seed annotation defaults: author (fall back to the OS user name so new marks
    // are attributed), and the last-used markup colour + style.
    QString author = s.annotationAuthor.trimmed();
    if (author.isEmpty()) {
        author = qEnvironmentVariable("USERNAME");
        if (author.isEmpty())
            author = qEnvironmentVariable("USER");
    }
    viewer_->setAnnotAuthor(author);
    // One shared default annotation colour drives both new markups and new sticky
    // notes; it is configured in Settings (the panel no longer carries a picker).
    const QColor markupColor(s.annotationColor);
    if (markupColor.isValid())
        viewer_->setMarkupColor(markupColor);
    const QString style = s.annotationStyle.trimmed().toLower();
    const AnnotType markupStyle = style == QLatin1String("underline")  ? AnnotType::Underline
                                  : style == QLatin1String("strikeout") ? AnnotType::StrikeOut
                                                                        : AnnotType::Highlight;
    viewer_->setMarkupStyle(markupStyle);
    annotPanel_->setHighlightStyle(markupStyle);
}

TabPage::~TabPage() = default;

bool TabPage::open(const QString &path, const QString &password, QString *error,
                   bool *needsPassword)
{
    auto doc = engine_->openDocument(path, password, error, needsPassword);
    if (!doc)
        return false;

    doc_ = std::move(doc);
    viewer_->setDocument(doc_.get());

    // Restore any measurements Mervin previously embedded in this PDF (a private
    // catalog blob; invisible to other viewers). Shown + editable immediately.
    //
    // Gated on the MuPDF-side catalog check: readMervinBlob opens and parses the
    // whole file a SECOND time through qpdf, which on a cold file cache costs
    // about as much as the MuPDF open itself - and virtually no file carries the
    // blob. hasMervinMeasurements() answers from the catalog MuPDF has already
    // parsed, so the qpdf reopen now happens only for files that really do.
    if (doc_->hasMervinMeasurements()) {
        if (auto blob = MeasureExport::readMervinBlob(path, password)) {
            MeasureDoc md;
            // Restore when the blob carries measurements OR page-scale overrides: a
            // calibration (override) is persisted independently of any committed
            // measurement, so a calibrate-only file must still restore its override
            // (otherwise the scale silently reverts and Reset never appears).
            // loadMeasurements tolerates an empty measurements vector - it assigns
            // the overrides and only force-enables the tool when measurements exist.
            if (parseMeasurements(*blob, &md)
                && (!md.measurements.empty() || !md.pageScales.empty())) {
                viewer_->loadMeasurements(md.measurements, md.overridesModel(), md.unit,
                                          md.precision, md.lineWidth);
                measurePanel_->setUnit(md.unit);
                measurePanel_->setPrecision(md.precision);
                measurePanel_->setLineWidth(md.lineWidth);
            }
        }
    }

    const QFileInfo fi(path);
    path_ = fi.absoluteFilePath();
    canonicalPath_ = fi.canonicalFilePath();
    if (canonicalPath_.isEmpty())
        canonicalPath_ = path_;
    return true;
}

void TabPage::detachDocument()
{
    viewer_->setDocument(nullptr); // clear the viewer's raw Document* first
    doc_.reset();                  // then drop the owner, closing the file handle
}

QString TabPage::tabTitle() const
{
    return QFileInfo(path_).fileName();
}

QString TabPage::documentTitle() const
{
    const QString t = doc_ ? doc_->title() : QString();
    return t.isEmpty() ? tabTitle() : t;
}

} // namespace mervin
