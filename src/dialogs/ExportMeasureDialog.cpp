#include "dialogs/ExportMeasureDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

namespace mervin {

ExportMeasureDialog::ExportMeasureDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Export with Measurements"));

    auto *layout = new QVBoxLayout(this);

    auto *label = new QLabel(
        //: One sentence per line. Keep the line breaks.
        tr("This document contains Mervin PDF measurements.\n"
           "When you export a copy, the measurements are added to the PDF.\n"
           "These exported measurements are non-editable.\n"
           "The exported file is viewable in any PDF viewer."),
        this);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Keep every sentence visible with longer translations and larger fonts.
    // Qt's automatic dialog width cap can otherwise add lines without height.
    ensurePolished();
    const QSize preferred = sizeHint();
    setMinimumSize(preferred.width(),
                   qMax(preferred.height(), layout->totalHeightForWidth(preferred.width())));
    resize(minimumSize());
}

} // namespace mervin
