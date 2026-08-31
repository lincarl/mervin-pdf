#include "ui/MeasurePanel.h"

#include "render/MeasureMath.h"
#include "ui/Icons.h"
#include "ui/PanelStack.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <cmath>

namespace mervin {

namespace {
constexpr int kClearXSize = 22; // square side for the clear-all / per-row X buttons
} // namespace

// The panel's row captions ("Unit", "Decimals", "Line width"). They carry an
// objectName because the app sheet has to give them an explicit colour: see the
// backgroundRole note in the constructor - a panel label that falls back to the
// palette resolves to QPalette::Light and is invisible in both themes.
QLabel *MeasurePanel::fieldLabel(const QString &text)
{
    auto *label = new QLabel(text, this);
    label->setObjectName(QStringLiteral("measureFieldLabel"));
    return label;
}

MeasurePanel::MeasurePanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("measurePanel"));
    setAttribute(Qt::WA_StyledBackground, true); // honour the QSS background
    // Anchor the background role here as well as on the viewport (see the comment
    // at the top of ViewerWidget's constructor): QWidget::foregroundRole() derives
    // child ink from the inherited background role, and a Dark/Shadow role turns
    // every plain QLabel in this panel into invisible QPalette::Light text. This
    // is a declaration, not a paint instruction - the surface comes from the QSS.
    setBackgroundRole(QPalette::Window);
    setCursor(Qt::ArrowCursor);
    // Geometry (position, group-drag, on-resize re-anchoring) is owned by the
    // PanelStack the panel is registered with; it watches the viewport itself.

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 10);
    outer->setSpacing(8);

    // ── Header: title + close (the title doubles as the drag handle) ──
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(tr("Measure"), this);
    title->setObjectName(QStringLiteral("measureTitle"));
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    auto *closeBtn = new QToolButton(this);
    closeBtn->setObjectName(QStringLiteral("measureClearX")); // square, muted ✕
    closeBtn->setFixedSize(kClearXSize, kClearXSize);
    closeBtn->setText(QStringLiteral("✕")); // ✕
    closeBtn->setAutoRaise(true);
    closeBtn->setToolTip(tr("Close measuring tool"));
    connect(closeBtn, &QToolButton::clicked, this, &MeasurePanel::closeRequested);
    headerRow->addWidget(title);
    headerRow->addStretch(1);
    headerRow->addWidget(closeBtn);
    outer->addLayout(headerRow);

    // ── Measure toggle: a single checkable button styled like the measure-kind
    // buttons below. Checked = the measuring cursor is active (click the page to
    // place points); unchecked = the tool falls back to the normal text-selection
    // cursor. It carries the same dimension symbol as the toolbar/menu Measure
    // action so the whole tool reads with one consistent mark. ──
    auto *cursorRow = new QHBoxLayout;
    cursorRow->setContentsMargins(0, 0, 0, 0);
    cursorRow->setSpacing(4);

    measureToggleBtn_ = new QToolButton(this);
    measureToggleBtn_->setObjectName(QStringLiteral("measureToggleBtn"));
    measureToggleBtn_->setCheckable(true);
    measureToggleBtn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    measureToggleBtn_->setIconSize(QSize(16, 16));
    measureToggleBtn_->setToolTip(
        tr("Measure - click the page to place points; toggle off to select text"));
    cursorRow->addWidget(measureToggleBtn_);
    cursorRow->addStretch(1);
    outer->addLayout(cursorRow);

    measureToggleBtn_->setChecked(true); // enabling the tool arms measuring
    refreshCursorIcons();
    connect(measureToggleBtn_, &QToolButton::toggled, this, [this](bool on) {
        refreshCursorIcons(); // armed = accent fill, so the glyph flips to onAccent
        emit cursorActiveChanged(on);
    }); // checked = measuring

    // ── Measure-type selector: four mutually exclusive choices, drawn as the
    // app's segmented control (the same element as All|Favorites on the Recent
    // screen), so the active kind reads as a solid accent segment rather than the
    // faint generic checked wash it used to get. ──
    auto *kindBar = new QWidget(this);
    kindBar->setObjectName(QStringLiteral("segmentBar"));
    auto *kindRow = new QHBoxLayout(kindBar);
    kindRow->setContentsMargins(0, 0, 0, 0);
    kindRow->setSpacing(0); // segments share their borders
    kindGroup_ = new QButtonGroup(this);
    kindGroup_->setExclusive(true);
    struct KindDef
    {
        MeasureKind kind;
        const char *label;
        const char *tip;
    };
    const KindDef defs[] = {
        {MeasureKind::Distance, QT_TR_NOOP("Distance"), QT_TR_NOOP("Measure a straight distance")},
        {MeasureKind::Polyline, QT_TR_NOOP("Path"), QT_TR_NOOP("Measure a multi-segment length")},
        {MeasureKind::Area, QT_TR_NOOP("Area"), QT_TR_NOOP("Measure a polygon area and perimeter")},
        {MeasureKind::Angle, QT_TR_NOOP("Angle"), QT_TR_NOOP("Measure an angle (three points)")},
    };
    const int kindCount = static_cast<int>(std::size(defs));
    for (int i = 0; i < kindCount; ++i) {
        const KindDef &d = defs[i];
        auto *btn = new QToolButton(kindBar);
        btn->setText(tr(d.label));
        btn->setToolTip(tr(d.tip));
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // Corner rounding and shared borders are keyed off `segpos` (Qt QSS does
        // not reliably match :first-child/:last-child on plain child widgets).
        btn->setProperty("segpos", i == 0 ? QStringLiteral("first")
                                          : i == kindCount - 1 ? QStringLiteral("last")
                                                               : QStringLiteral("mid"));
        kindGroup_->addButton(btn, static_cast<int>(d.kind));
        kindRow->addWidget(btn, 1); // equal-width segments
    }
    if (auto *first = kindGroup_->button(static_cast<int>(MeasureKind::Distance)))
        first->setChecked(true);
    connect(kindGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        kind_ = static_cast<MeasureKind>(id);
        emit kindChanged(kind_);
    });
    outer->addWidget(kindBar);

    // ── Detected / active scale ──
    scaleLabel_ = new QLabel(tr("Scale -"), this);
    scaleLabel_->setObjectName(QStringLiteral("measureScale"));
    outer->addWidget(scaleLabel_);

    // ── Unit + precision ──
    auto *unitRow = new QHBoxLayout;
    unitRow->setContentsMargins(0, 0, 0, 0);
    unitRow->setSpacing(6);
    unitRow->addWidget(fieldLabel(tr("Unit")));
    unitCombo_ = new QComboBox(this);
    unitCombo_->addItem(tr("mm"), static_cast<int>(MeasureUnit::Millimeter));
    unitCombo_->addItem(tr("cm"), static_cast<int>(MeasureUnit::Centimeter));
    unitCombo_->addItem(tr("m"), static_cast<int>(MeasureUnit::Meter));
    unitCombo_->addItem(tr("in"), static_cast<int>(MeasureUnit::Inch));
    unitCombo_->addItem(tr("ft"), static_cast<int>(MeasureUnit::Foot));
    connect(unitCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        emit unitChanged(unit());
    });
    unitRow->addWidget(unitCombo_);
    unitRow->addSpacing(6);
    unitRow->addWidget(fieldLabel(tr("Decimals")));

    // A clean stepper: [-] [value] [+]. The spin box keeps the range/clamping
    // and stays type-/wheel-editable, but its native (ugly) arrows are hidden in
    // favour of the two flanking buttons.
    precisionDownBtn_ = new QToolButton(this);
    precisionDownBtn_->setObjectName(QStringLiteral("measureStepBtn"));
    // TextOnly so the style centres the glyph; the QToolButton default
    // (IconOnly with a text fallback) left-aligns it in the button.
    precisionDownBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    precisionDownBtn_->setText(QString(QChar(0x2212))); // − minus sign
    precisionDownBtn_->setToolTip(tr("Fewer decimals"));
    precisionDownBtn_->setAutoRepeat(true);
    precisionDownBtn_->setFocusPolicy(Qt::NoFocus);

    precisionSpin_ = new QSpinBox(this);
    precisionSpin_->setRange(0, 4);
    precisionSpin_->setValue(2);
    // Typed-only, and via the Theme helper: the app sheet reserves 22px on the
    // right of a spin box for its stepper column, which on this 40px-wide field
    // pushed the digit clean out of the content rect - the panel showed an empty
    // box in both themes. useTypedSpinBox drops the reservation with the buttons.
    mervin::Theme::useTypedSpinBox(precisionSpin_);
    precisionSpin_->setAlignment(Qt::AlignHCenter);
    precisionSpin_->setFixedWidth(44);
    // The value is driven only by the − / + steppers: make the field itself inert
    // so the digit can't be focused, typed into, or selected. Without this the
    // spin box takes focus and selects-all on each step, leaving the number
    // visibly "marked"; a stray drag could also highlight it.
    precisionSpin_->setReadOnly(true);
    precisionSpin_->setFocusPolicy(Qt::NoFocus);
    if (QLineEdit *edit = precisionSpin_->findChild<QLineEdit *>()) {
        edit->setFocusPolicy(Qt::NoFocus);
        edit->setContextMenuPolicy(Qt::NoContextMenu);
        // Undo any selection the spin box makes on step (and defeat mouse drags),
        // so the number is never shown selected. deselect() on an empty selection
        // is a no-op, so this can't recurse.
        connect(edit, &QLineEdit::selectionChanged, edit, [edit] {
            if (edit->hasSelectedText())
                edit->deselect();
        });
    }

    precisionUpBtn_ = new QToolButton(this);
    precisionUpBtn_->setObjectName(QStringLiteral("measureStepBtn"));
    precisionUpBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly); // centre the + glyph
    precisionUpBtn_->setText(QStringLiteral("+"));
    precisionUpBtn_->setToolTip(tr("More decimals"));
    precisionUpBtn_->setAutoRepeat(true);
    precisionUpBtn_->setFocusPolicy(Qt::NoFocus);

    // Drive the clamped value directly rather than via stepUp/stepDown: the field
    // is read-only (to block manual edits and the wheel), which disables the spin
    // box's own stepping, so the − / + buttons set the value themselves. setValue
    // clamps to the range, and updatePrecisionStepButtons already gates the ends.
    connect(precisionDownBtn_, &QToolButton::clicked, this,
            [this] { precisionSpin_->setValue(precisionSpin_->value() - 1); });
    connect(precisionUpBtn_, &QToolButton::clicked, this,
            [this] { precisionSpin_->setValue(precisionSpin_->value() + 1); });
    connect(precisionSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        updatePrecisionStepButtons(v);
        emit precisionChanged(v);
    });

    unitRow->addWidget(precisionDownBtn_);
    unitRow->addWidget(precisionSpin_);
    unitRow->addWidget(precisionUpBtn_);
    unitRow->addStretch(1);
    outer->addLayout(unitRow);
    updatePrecisionStepButtons(precisionSpin_->value());

    // ── Line width: the stroke weight of drawn (and exported) measurement marks.
    // The values are in points; on screen they read as pixels at 100% zoom and map
    // 1:1 to the PDF stroke width when measurements are exported/printed. ──
    auto *widthRow = new QHBoxLayout;
    widthRow->setContentsMargins(0, 0, 0, 0);
    widthRow->setSpacing(6);
    widthRow->addWidget(fieldLabel(tr("Line width")));
    lineWidthCombo_ = new QComboBox(this);
    const double widths[] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0};
    for (double w : widths)
        lineWidthCombo_->addItem(tr("%1 pt").arg(w, 0, 'g', 2), w);
    lineWidthCombo_->setCurrentIndex(3); // 2 pt default
    connect(lineWidthCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { emit lineWidthChanged(lineWidth()); });
    widthRow->addWidget(lineWidthCombo_);
    widthRow->addStretch(1);
    outer->addLayout(widthRow);

    // Watch the unit dropdown's popup window so we can tell the viewer when it
    // closes (its dismissal over the page otherwise drops a stray point - Qt
    // replays the closing press onto the viewport). view() builds the popup.
    if (auto *view = unitCombo_->view()) {
        unitPopup_ = view->window();
        if (unitPopup_)
            unitPopup_->installEventFilter(this);
    }

    // ── Live readout ──
    readout_ = new QLabel(this);
    readout_->setObjectName(QStringLiteral("measureReadout"));
    readout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    readout_->setMinimumWidth(220);
    readout_->hide(); // empty until a live value / calibration prompt arrives
    outer->addWidget(readout_);

    // ── Calibrate ──
    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(6);
    auto *calibrateBtn = new QToolButton(this);
    calibrateBtn->setText(tr("Calibrate"));
    calibrateBtn->setToolTip(tr("Set the scale by drawing a line of known length"));
    connect(calibrateBtn, &QToolButton::clicked, this, &MeasurePanel::calibrateRequested);
    actionRow->addWidget(calibrateBtn);

    // "Set Scale" sits to the right of "Calibrate": it sets the page scale manually
    // by typing a ratio (1 : N), without drawing a calibration line.
    auto *setScaleBtn = new QToolButton(this);
    setScaleBtn->setText(tr("Set Scale"));
    setScaleBtn->setToolTip(tr("Set the scale manually by typing a ratio (1 : N)"));
    connect(setScaleBtn, &QToolButton::clicked, this, &MeasurePanel::setScaleRequested);
    actionRow->addWidget(setScaleBtn);

    // "Reset" sits beside "Calibrate" but is shown only when this page has an
    // embedded PDF scale that a manual calibration is currently overriding (the
    // viewer drives its visibility via setResetVisible). Clicking it discards the
    // override so the PDF's own scale is used again.
    resetBtn_ = new QToolButton(this);
    resetBtn_->setText(tr("Reset"));
    resetBtn_->setToolTip(tr("Discard the manual calibration and use the scale embedded in the PDF"));
    resetBtn_->hide();
    connect(resetBtn_, &QToolButton::clicked, this, &MeasurePanel::resetRequested);
    actionRow->addWidget(resetBtn_);

    actionRow->addStretch(1);
    outer->addLayout(actionRow);

    // ── Committed-measurement list. The header row pairs the "Measurements" title
    // with a clear-all X on the right: sitting at the top of the column of per-row
    // X buttons, it reads clearly as "remove every measurement" (replacing the old
    // Clear button, which was easy to confuse with Calibrate beside it). The whole
    // row is shown only when there is at least one measurement. ──
    measuresHeader_ = new QWidget(this);
    auto *measuresHeaderRow = new QHBoxLayout(measuresHeader_);
    measuresHeaderRow->setContentsMargins(0, 0, 0, 0);
    measuresHeaderRow->setSpacing(6);
    listHeader_ = new QLabel(tr("Measurements"), measuresHeader_);
    listHeader_->setObjectName(QStringLiteral("measureListHeader"));
    auto *clearAllBtn = new QToolButton(measuresHeader_);
    clearAllBtn->setObjectName(QStringLiteral("measureClearX"));
    clearAllBtn->setToolButtonStyle(Qt::ToolButtonTextOnly); // centre the ✕ glyph
    clearAllBtn->setText(QStringLiteral("✕"));
    clearAllBtn->setAutoRaise(true);
    clearAllBtn->setCursor(Qt::ArrowCursor);
    clearAllBtn->setFixedSize(kClearXSize, kClearXSize); // square, matching the row X
    clearAllBtn->setToolTip(tr("Clear all measurements"));
    connect(clearAllBtn, &QToolButton::clicked, this, &MeasurePanel::clearRequested);
    measuresHeaderRow->addWidget(listHeader_);
    measuresHeaderRow->addStretch(1);
    measuresHeaderRow->addWidget(clearAllBtn);
    measuresHeader_->hide();
    outer->addWidget(measuresHeader_);

    measureList_ = new QListWidget(this);
    measureList_->setObjectName(QStringLiteral("measureList"));
    measureList_->setSelectionMode(QAbstractItemView::NoSelection);
    measureList_->setFocusPolicy(Qt::NoFocus);
    measureList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    measureList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    measureList_->setUniformItemSizes(true);
    measureList_->setContextMenuPolicy(Qt::CustomContextMenu);
    measureList_->hide();
    connect(measureList_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        // For an item view, customContextMenuRequested delivers `pos` already in
        // viewport coordinates - exactly what itemAt() and the viewport's
        // mapToGlobal() expect; no frame-inset remapping is needed.
        QListWidgetItem *item = measureList_->itemAt(pos);
        if (!item)
            return;
        const int row = measureList_->row(item);
        const QColor ink = theme::chrome(palette()).inkBody;
        QMenu menu(this);
        QAction *copyAct =
            menu.addAction(icons::glyph(icons::Glyph::Copy, ink), tr("Copy value"));
        QAction *delAct =
            menu.addAction(icons::glyph(icons::Glyph::Delete, ink), tr("Delete measurement"));
        QAction *chosen = menu.exec(measureList_->viewport()->mapToGlobal(pos));
        if (chosen == copyAct)
            emit copyMeasurementRequested(row);
        else if (chosen == delAct)
            emit removeMeasurementRequested(row);
    });
    outer->addWidget(measureList_);

    adjustSize();
}

void MeasurePanel::setScaleText(const QString &text)
{
    if (scaleLabel_)
        scaleLabel_->setText(text);
}

void MeasurePanel::setResetVisible(bool visible)
{
    if (!resetBtn_)
        return;
    // Compare against the button's own shown/hidden flag (isHidden), not
    // isVisible(): this is often called while the panel is still hidden (the
    // resettable signal fires before measureModeChanged shows it), where
    // isVisible() is always false and would mis-gate the toggle.
    const bool currentlyShown = !resetBtn_->isHidden();
    if (currentlyShown == visible)
        return;
    resetBtn_->setVisible(visible);
    // The button shares the Calibrate row, so the panel width can grow/shrink;
    // re-fit and let the dock re-stack/clamp.
    adjustSize();
    if (stack_)
        stack_->relayout();
}

void MeasurePanel::setReadout(const QString &text)
{
    if (!readout_)
        return;
    const bool wasVisible = readout_->isVisible();
    readout_->setText(text);
    const bool visible = !text.isEmpty();
    if (visible != wasVisible) {
        // An empty readout would otherwise reserve a blank 15px line, leaving an
        // inconsistent gap above Calibrate; collapse it when there's nothing to
        // show. Only resize on the actual visibility flip - same-state live value
        // updates must not jitter the panel while a measurement streams in.
        readout_->setVisible(visible);
        adjustSize();
        if (stack_)
            stack_->relayout();
    }
}

void MeasurePanel::setMeasurements(const QStringList &items)
{
    if (!measureList_)
        return;

    constexpr int kRowHeight = 26; // px per row
    constexpr int kMaxRows = 10;   // cap the visible height; scroll beyond this

    measureItems_ = items;
    measureLabels_.clear();
    measureRows_.clear();
    rowHoverIndex_.clear();
    if (hoveredRow_ != -1) {
        hoveredRow_ = -1;
        emit measurementHovered(-1, false); // the list is rebuilding; drop stale emphasis
    }
    measureList_->clear();
    const bool any = !items.isEmpty();
    measuresHeader_->setVisible(any); // title + clear-all X
    measureList_->setVisible(any);

    for (int i = 0; i < items.size(); ++i) {
        auto *row = new QWidget(measureList_);
        // The row widget carries its own hover wash: it covers the item rect edge
        // to edge, so the view never sees the mouse move and ::item:hover cannot
        // match (WA_StyledBackground is what lets the QSS background paint).
        row->setObjectName(QStringLiteral("measureRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(8, 1, 4, 1);
        h->setSpacing(6);

        // Hold the full text; reelideMeasurements() trims it to the row's actual
        // width. An ignored width keeps the label from forcing the panel wider,
        // so it fills (and elides to) whatever the panel width allows.
        auto *label = new QLabel(items[i], row);
        label->setObjectName(QStringLiteral("measureRowText"));
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setToolTip(items[i]);
        measureLabels_.append(label);
        h->addWidget(label, 1);

        auto *del = new QToolButton(row);
        del->setObjectName(QStringLiteral("measureClearX"));
        del->setToolButtonStyle(Qt::ToolButtonTextOnly); // centre the ✕ glyph
        del->setText(QStringLiteral("✕"));
        del->setAutoRaise(true);
        del->setCursor(Qt::ArrowCursor);
        del->setFixedSize(kClearXSize, kClearXSize); // square (width == height)
        del->setToolTip(tr("Remove this measurement"));
        // The list is rebuilt wholesale on every change, so the build-time index
        // stays valid until the next rebuild (which this click triggers).
        connect(del, &QToolButton::clicked, this,
                [this, i] { emit removeMeasurementRequested(i); });
        h->addWidget(del, 0);

        // Hovering anywhere on the row emphasises this measurement in the page.
        // Entering a child (the label or X) fires Leave on the row, so watch the
        // row and its children together, all keyed back to the measurement index.
        measureRows_.append(row);
        row->installEventFilter(this);
        label->installEventFilter(this);
        del->installEventFilter(this);
        rowHoverIndex_.insert(row, i);
        rowHoverIndex_.insert(label, i);
        rowHoverIndex_.insert(del, i);

        auto *item = new QListWidgetItem(measureList_);
        item->setSizeHint(QSize(0, kRowHeight));
        measureList_->addItem(item);
        measureList_->setItemWidget(item, row);
    }

    const int rows = std::min(static_cast<int>(items.size()), kMaxRows);
    const int frame = 2 * measureList_->frameWidth();
    measureList_->setFixedHeight(any ? rows * kRowHeight + frame : 0);

    // The panel grew/shrank with the list; keep it sized and let the dock re-stack.
    adjustSize();
    if (stack_)
        stack_->relayout();
    // Fit the row text to the (now-final) panel width. Also do it deferred, once
    // the layout has settled and the labels have their real widths.
    reelideMeasurements();
    QTimer::singleShot(0, this, [this] { reelideMeasurements(); });
}

void MeasurePanel::reelideMeasurements()
{
    for (int i = 0; i < measureLabels_.size() && i < measureItems_.size(); ++i) {
        QLabel *label = measureLabels_[i];
        if (!label)
            continue;
        const int avail = label->width();
        const QFontMetrics fm(label->font());
        label->setText(avail > 8 ? fm.elidedText(measureItems_[i], Qt::ElideRight, avail)
                                 : measureItems_[i]);
    }
}

void MeasurePanel::setKind(MeasureKind kind)
{
    kind_ = kind;
    if (auto *btn = kindGroup_->button(static_cast<int>(kind)))
        btn->setChecked(true);
}

void MeasurePanel::setUnit(MeasureUnit unit)
{
    const int idx = unitCombo_->findData(static_cast<int>(unit));
    if (idx >= 0) {
        QSignalBlocker block(unitCombo_);
        unitCombo_->setCurrentIndex(idx);
    }
}

void MeasurePanel::setPrecision(int decimals)
{
    QSignalBlocker block(precisionSpin_);
    precisionSpin_->setValue(decimals);
    updatePrecisionStepButtons(precisionSpin_->value());
}

void MeasurePanel::setLineWidth(double width)
{
    if (!lineWidthCombo_)
        return;
    // Snap to the nearest offered width so a persisted/seeded value always lands
    // on a concrete combo entry.
    int best = 0;
    double bestDelta = 1e9;
    for (int i = 0; i < lineWidthCombo_->count(); ++i) {
        const double delta = std::abs(lineWidthCombo_->itemData(i).toDouble() - width);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = i;
        }
    }
    QSignalBlocker block(lineWidthCombo_);
    lineWidthCombo_->setCurrentIndex(best);
}

double MeasurePanel::lineWidth() const
{
    return lineWidthCombo_ ? lineWidthCombo_->currentData().toDouble() : 2.0;
}

void MeasurePanel::updatePrecisionStepButtons(int value)
{
    if (precisionDownBtn_)
        precisionDownBtn_->setEnabled(value > precisionSpin_->minimum());
    if (precisionUpBtn_)
        precisionUpBtn_->setEnabled(value < precisionSpin_->maximum());
}

void MeasurePanel::setCursorActive(bool active)
{
    if (!measureToggleBtn_)
        return;
    // Block the toggle so reflecting the viewer's state doesn't re-emit
    // cursorActiveChanged back to it.
    QSignalBlocker block(measureToggleBtn_);
    measureToggleBtn_->setChecked(active);
}

void MeasurePanel::refreshCursorIcons()
{
    if (!measureToggleBtn_)
        return;
    // The QPainter-drawn dimension mark isn't recoloured by QSS; repaint it in
    // the ink the current state needs. Armed, the button carries a solid accent
    // fill (see #measureToggleBtn:checked), so the glyph has to switch to the
    // on-accent ink or it disappears into the fill.
    const theme::Chrome t = theme::chrome(palette());
    const QColor ink = measureToggleBtn_->isChecked() ? t.onAccent : t.inkBody;
    measureToggleBtn_->setIcon(icons::glyph(icons::Glyph::Measure, ink));
}

MeasureUnit MeasurePanel::unit() const
{
    return static_cast<MeasureUnit>(unitCombo_->currentData().toInt());
}

int MeasurePanel::precision() const
{
    return precisionSpin_->value();
}

void MeasurePanel::mousePressEvent(QMouseEvent *event)
{
    // Dragging the panel body drags the whole dock as a group (the PanelStack
    // moves every stacked panel together). Presses on the interactive controls
    // (buttons, combos) are consumed by them and never reach here.
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragLastGlobal_ = event->globalPosition().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MeasurePanel::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        const QPoint g = event->globalPosition().toPoint();
        if (stack_)
            stack_->nudge(g - dragLastGlobal_);
        dragLastGlobal_ = g;
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void MeasurePanel::mouseReleaseEvent(QMouseEvent *event)
{
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
}

void MeasurePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    adjustSize();
    if (stack_)
        stack_->relayout(); // dock places this panel (and re-stacks the others)
    reelideMeasurements();
}

void MeasurePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    reelideMeasurements();
}

void MeasurePanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Repaint the toggle icons in the new ink when the colour scheme changes
    // (QSS recolours text, but not these QPainter-drawn pixmaps).
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange)
        refreshCursorIcons();
}

bool MeasurePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        const auto it = rowHoverIndex_.constFind(watched);
        if (it != rowHoverIndex_.constEnd()) {
            const int idx = it.value();
            if (event->type() == QEvent::Enter) {
                if (hoveredRow_ != idx) {
                    hoveredRow_ = idx;
                    emit measurementHovered(idx, true);
                }
            } else {
                // Crossing from the row onto its own label/X fires Leave here and
                // Enter on the child; only drop the emphasis once the cursor has
                // truly left the row's rectangle.
                QWidget *rowW =
                    (idx >= 0 && idx < measureRows_.size()) ? measureRows_[idx] : nullptr;
                const bool stillInside =
                    rowW && rowW->rect().contains(rowW->mapFromGlobal(QCursor::pos()));
                if (!stillInside && hoveredRow_ == idx) {
                    hoveredRow_ = -1;
                    emit measurementHovered(idx, false);
                }
            }
            return QWidget::eventFilter(watched, event);
        }
    }
    if (watched == unitPopup_ && event->type() == QEvent::Hide) {
        emit popupDismissed(); // arm the viewer's one-shot replay guard
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace mervin
