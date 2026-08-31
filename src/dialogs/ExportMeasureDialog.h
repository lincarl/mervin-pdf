#pragma once

#include <QDialog>

namespace mervin {

// Shown before exporting a copy of a document that carries measurements. There
// is no choice to make - the marks are always burned in (visible in any viewer,
// not editable) - so this just tells the user what will happen before they
// confirm. Accepted() means proceed with the export.
class ExportMeasureDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportMeasureDialog(QWidget *parent = nullptr);
};

} // namespace mervin
