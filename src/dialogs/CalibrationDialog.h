#pragma once

#include "render/MeasureTypes.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace mervin {

// Modal dialog for setting a page's drawing scale. Two purpose-built variants:
//
//  * Mode::Calibrate - shown after the user draws a calibration line over a known
//    dimension (the panel's "Calibrate" button). Asks only for that line's real
//    length (+ unit).
//  * Mode::SetScale  - shown when the user sets the scale manually (the panel's
//    "Set Scale" button, no line drawn). Asks only for a scale ratio (1 : N).
//
// result() returns the resolved scale once the dialog is accepted.
class CalibrationDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { Calibrate, SetScale };

    // For Mode::Calibrate, lineLengthPoints is the drawn line's length (PDF points)
    // and defaultUnit seeds the unit combo. For Mode::SetScale both are ignored.
    CalibrationDialog(Mode mode, double lineLengthPoints, MeasureUnit defaultUnit,
                      QWidget *parent = nullptr);

    // Valid only after exec() == Accepted.
    MeasureScale result() const;

private:
    Mode mode_ = Mode::Calibrate;
    double lineLengthPoints_ = 0.0;
    QDoubleSpinBox *lengthSpin_ = nullptr; // Calibrate mode only
    QComboBox *unitCombo_ = nullptr;       // Calibrate mode only
    QSpinBox *ratioSpin_ = nullptr;        // SetScale mode only
};

} // namespace mervin
