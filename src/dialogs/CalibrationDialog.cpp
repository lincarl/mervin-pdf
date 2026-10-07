#include "dialogs/CalibrationDialog.h"

#include "render/MeasureMath.h"
#include "render/MeasureModel.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace mervin {

CalibrationDialog::CalibrationDialog(Mode mode, double lineLengthPoints, MeasureUnit defaultUnit,
                                     QWidget *parent)
    : QDialog(parent)
    , mode_(mode)
    , lineLengthPoints_(lineLengthPoints)
{
    auto *layout = new QVBoxLayout(this);
    auto *grid = new QGridLayout;

    if (mode_ == Mode::Calibrate) {
        //: Window title (verb): set the drawing scale from a line of known length.
        setWindowTitle(tr("Calibrate"));

        auto *intro = new QLabel(
            tr("Enter the real length of the line you drew to set the drawing scale."), this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        // ── Known length row ──
        //: Label before a number field and a unit picker: the real length of the
        //: line the user drew.
        auto *lengthLabel = new QLabel(tr("Known length:"), this);
        lengthSpin_ = new QDoubleSpinBox(this);
        lengthSpin_->setRange(0.001, 1.0e9);
        lengthSpin_->setDecimals(3);
        lengthSpin_->setValue(1000.0);
        mervin::Theme::useTypedSpinBox(lengthSpin_); // typed, no stepper column
        unitCombo_ = new QComboBox(this);
        // Unit symbols are not translated (see measure::unitSuffix).
        for (const MeasureUnit u : {MeasureUnit::Millimeter, MeasureUnit::Centimeter,
                                    MeasureUnit::Meter, MeasureUnit::Inch, MeasureUnit::Foot})
            unitCombo_->addItem(measure::unitSuffix(u), static_cast<int>(u));
        if (const int idx = unitCombo_->findData(static_cast<int>(defaultUnit)); idx >= 0)
            unitCombo_->setCurrentIndex(idx);
        grid->addWidget(lengthLabel, 0, 0);
        grid->addWidget(lengthSpin_, 0, 1);
        grid->addWidget(unitCombo_, 0, 2);
    } else {
        //: Window title (verb): type the drawing scale as a ratio.
        setWindowTitle(tr("Set Scale"));

        auto *intro = new QLabel(tr("Set the drawing scale by typing a scale ratio."), this);
        intro->setWordWrap(true);
        layout->addWidget(intro);

        // ── Ratio row ──
        //: Label directly before a number field N. Together they read as the ratio
        //: "1 : N", such as "Scale ratio 1 : 100" (1 unit on paper is N real units).
        //: Keep "1 :" at the end so the number follows it.
        auto *ratioLabel = new QLabel(tr("Scale ratio 1 :"), this);
        ratioSpin_ = new QSpinBox(this);
        ratioSpin_->setRange(1, 1000000);
        ratioSpin_->setValue(100);
        mervin::Theme::useTypedSpinBox(ratioSpin_); // typed, no stepper column
        grid->addWidget(ratioLabel, 0, 0);
        grid->addWidget(ratioSpin_, 0, 1);
        // Spare width (a short label under a wider button row) goes to the field, so
        // the label's "1 :" stays right next to the number.
        grid->setColumnStretch(1, 1);
    }

    layout->addLayout(grid);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Wrapped text needs the height for this width, not the layout's
    // width-independent minimum, which can squeeze the explanation and buttons.
    ensurePolished();
    const QSize preferred = sizeHint();
    setMinimumSize(preferred.width(),
                   qMax(preferred.height(), layout->totalHeightForWidth(preferred.width())));
    resize(minimumSize());
}

MeasureScale CalibrationDialog::result() const
{
    if (mode_ == Mode::Calibrate && lineLengthPoints_ > 0.0) {
        const auto unit = static_cast<MeasureUnit>(unitCombo_->currentData().toInt());
        return MeasureModel::fromCalibration(lineLengthPoints_, lengthSpin_->value(), unit);
    }
    if (mode_ == Mode::SetScale && ratioSpin_)
        return MeasureModel::fromRatio(ratioSpin_->value());
    return MeasureScale{};
}

} // namespace mervin
