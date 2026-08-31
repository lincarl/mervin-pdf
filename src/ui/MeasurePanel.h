#pragma once

#include "render/MeasureTypes.h"
#include "ui/MeasureTypes.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QEvent;
class QLabel;
class QListWidget;
class QSpinBox;
class QToolButton;

namespace mervin {

class PanelStack;

// A small floating tool palette shown over the page while the measuring tool is
// active. Parented to the viewer's viewport so it overlaps the page (and does not
// scroll with content). Holds the measure-type selector, the detected/active
// scale, a unit + precision picker, the live readout, and Calibrate / Clear
// buttons. It emits intent signals; the viewer/MainWindow own the behaviour. Its
// position (and group-drag) is owned by a PanelStack so it can dock with the
// Comment panel.
class MeasurePanel : public QWidget
{
    Q_OBJECT

public:
    explicit MeasurePanel(QWidget *parent = nullptr);

    void setScaleText(const QString &text);
    // Show/hide the "Reset" button beside "Calibrate". Shown only when the page
    // carries an embedded PDF scale AND the user has applied a manual/calibrated
    // override on it, so the override can be discarded to fall back to the PDF's
    // own scale. Toggling it resizes the panel like the readout does.
    void setResetVisible(bool visible);
    void setReadout(const QString &text);
    // Rebuild the committed-measurement list (one row per item, in order, each
    // with an X to remove it). Empty hides the section.
    void setMeasurements(const QStringList &items);

    // Sync the controls without emitting change signals (seed from settings/state).
    void setKind(MeasureKind kind);
    void setUnit(MeasureUnit unit);
    void setPrecision(int decimals);
    void setLineWidth(double width); // snap to the nearest offered width
    // Reflect the viewer's active cursor (crosshair vs standard pointer) on the
    // toggle button, without emitting cursorActiveChanged.
    void setCursorActive(bool active);

    // The dock that owns this panel's geometry (vertical stacking + group drag).
    void setStack(PanelStack *stack) { stack_ = stack; }

    MeasureKind kind() const { return kind_; }
    MeasureUnit unit() const;
    int precision() const;
    double lineWidth() const;

signals:
    void kindChanged(MeasureKind kind);
    void unitChanged(MeasureUnit unit);
    void precisionChanged(int decimals);
    void lineWidthChanged(double width);
    void cursorActiveChanged(bool active); // toggle: crosshair (true) vs standard pointer (false)
    void calibrateRequested();
    void setScaleRequested(); // set the scale manually via a ratio (1 : N), no line drawn
    void resetRequested(); // discard the manual calibration; use the PDF's embedded scale
    void clearRequested();
    void closeRequested();
    void removeMeasurementRequested(int index); // the row's X button was clicked
    void copyMeasurementRequested(int index);   // "Copy value" chosen on a list row
    // The cursor entered (hovered=true) or left (false) a committed-measurement
    // row; `index` identifies the measurement. The viewer emphasises that mark
    // (a thicker stroke) while the row is hovered.
    void measurementHovered(int index, bool hovered);
    // The unit dropdown (a Qt::Popup) just closed. The viewer uses this to
    // swallow the synthetic press Qt replays when the popup is dismissed over the
    // page, which would otherwise drop a stray measurement point.
    void popupDismissed();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // A row caption ("Unit", "Decimals", "Line width"), tagged so the app sheet
    // can give it an explicit ink - a panel label that falls back to the palette
    // is invisible (see the constructor's backgroundRole note).
    QLabel *fieldLabel(const QString &text);
    // Re-elide each measurement row's label to its current width, so the text
    // uses the full panel width (and only truncates what genuinely overflows).
    void reelideMeasurements();
    // Re-tint the measure-toggle's dimension icon for its current state (its
    // QPainter pixmap isn't recoloured by QSS). Called on construct, on toggle,
    // and on palette/style change.
    void refreshCursorIcons();
    // Enable/disable the - / + stepper at the precision range ends.
    void updatePrecisionStepButtons(int value);

    QButtonGroup *kindGroup_ = nullptr;
    QToolButton *measureToggleBtn_ = nullptr;  // checkable: measuring cursor on/off
    QLabel *scaleLabel_ = nullptr;
    QComboBox *unitCombo_ = nullptr;
    QComboBox *lineWidthCombo_ = nullptr;
    QToolButton *resetBtn_ = nullptr; // "Reset": discard the override, use the embedded scale
    QSpinBox *precisionSpin_ = nullptr;
    QToolButton *precisionDownBtn_ = nullptr; // - : fewer decimals
    QToolButton *precisionUpBtn_ = nullptr;   // + : more decimals
    QLabel *readout_ = nullptr;
    QWidget *measuresHeader_ = nullptr; // "Measurements" title + clear-all X row
    QLabel *listHeader_ = nullptr;
    QListWidget *measureList_ = nullptr;
    QObject *unitPopup_ = nullptr; // the unit combo's dropdown window (watched for Hide)

    QStringList measureItems_;      // full (un-elided) measurement strings
    QList<QLabel *> measureLabels_; // row labels, re-elided to the list width
    QList<QWidget *> measureRows_;        // row container widgets, indexed like measureItems_
    QHash<QObject *, int> rowHoverIndex_; // row/label/X widget -> measurement index (hover)
    int hoveredRow_ = -1;                 // measurement index currently emphasised (or -1)

    MeasureKind kind_ = MeasureKind::Distance;
    PanelStack *stack_ = nullptr; // owns this panel's geometry (non-owning back-ptr)
    bool dragging_ = false;
    QPoint dragLastGlobal_; // last cursor position during a group drag (global px)
};

} // namespace mervin
