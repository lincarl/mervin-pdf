#pragma once

#include "print/PrintLayout.h"

#include <QDialog>
#include <QList>
#include <QMarginsF>
#include <QPageLayout>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPrinter;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;

namespace mervin {
class Document;
class PrintPreviewWidget;
class RenderEngine;
}

// Configures the supplied printer and previews the same layout and settings used by the
// caller on acceptance. The printer and document must outlive this dialog.
class PrintDialog : public QDialog
{
    Q_OBJECT

public:
    PrintDialog(QPrinter *printer, mervin::RenderEngine *engine, mervin::Document *document,
                int rotation, QPageLayout::Orientation initialOrientation, int currentPage,
                const QString &suggestedFileName, QWidget *parent = nullptr);

    // Valid after exec() == Accepted, when useSystemDialog() == false.
    QList<int> selectedPages() const { return pages_; } // 1-based, in print order
    mervin::printing::Settings printSettings() const;

    // The caller should open the native print dialog instead of using this selection.
    bool useSystemDialog() const { return useSystemDialog_; }

private:
    void accept() override;
    QList<int> resolvePages(QString *error) const;
    void schedulePreview();
    bool refreshPreview();
    bool applyPrinterSettings(QString *error);
    void updateNavigation();

    QPrinter *printer_; // non-owning
    int pageCount_ = 1;
    int currentPage_ = 1;
    int previewIndex_ = 0; // index into pages_, including repeated custom pages
    QString suggestedFileName_;
    QMarginsF preferredMargins_;
    QList<int> pages_;
    bool useSystemDialog_ = false;

    QComboBox *printerCombo_ = nullptr;
    QComboBox *paperSizeCombo_ = nullptr;
    QRadioButton *portraitRadio_ = nullptr;
    QRadioButton *landscapeRadio_ = nullptr;
    QSpinBox *copiesSpin_ = nullptr;
    QRadioButton *allPagesRadio_ = nullptr;
    QRadioButton *currentPageRadio_ = nullptr;
    QRadioButton *rangeRadio_ = nullptr;
    QRadioButton *customRadio_ = nullptr;
    QLineEdit *customEdit_ = nullptr;
    QSpinBox *fromSpin_ = nullptr;
    QSpinBox *toSpin_ = nullptr;
    QComboBox *colorCombo_ = nullptr;
    QComboBox *qualityCombo_ = nullptr;
    QComboBox *scaleCombo_ = nullptr;
    QSpinBox *scalePercentSpin_ = nullptr;
    QComboBox *alignmentCombo_ = nullptr;
    QCheckBox *twoSidedCheck_ = nullptr;
    QCheckBox *printToFile_ = nullptr;
    mervin::PrintPreviewWidget *preview_ = nullptr;
    QTimer *previewTimer_ = nullptr;
    QLabel *pageLabel_ = nullptr;
    QLabel *paperLabel_ = nullptr;
    QPushButton *previousButton_ = nullptr;
    QPushButton *nextButton_ = nullptr;
    QPushButton *printButton_ = nullptr;
};
