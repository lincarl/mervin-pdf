#pragma once

#include "render/MeasureTypes.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace mervin {

// Calibrate asks for the real length/unit of a drawn line; SetScale asks for a manual 1:N
// ratio. result() returns the resolved scale after acceptance.
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
