#pragma once

#include <QDialog>
#include <QList>
#include <QPageLayout>
#include <QString>

class QComboBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QCheckBox;
class QPrinter;

// Configures the supplied QPrinter; the caller renders on acceptance using selectedPages (one-
// based, ordered), scaleMode/scalePercent and qualityDpi.
// The in-app dialog supports page-derived orientation, themed labels and a system-dialog
// fallback. Windows' native dialog ignores the initial orientation and exposes driver
// accelerator strings.
class PrintDialog : public QDialog
{
    Q_OBJECT

public:
    enum class ScaleMode {
        FitToPage,  // shrink/grow each page to fill the printable area (default)
        ActualSize, // 1:1 - 1 PDF point = 1/72", may clip into the margins
        Custom,     // ActualSize scaled by scalePercent()
    };

    PrintDialog(QPrinter *printer, QPageLayout::Orientation initialOrientation, int pageCount,
                int currentPage, const QString &suggestedFileName, QWidget *parent = nullptr);

    // Valid after exec() == Accepted, when useSystemDialog() == false.
    QList<int> selectedPages() const { return pages_; } // 1-based, in print order
    ScaleMode scaleMode() const { return scaleMode_; }
    int scalePercent() const { return scalePercent_; } // meaningful for Custom
    int qualityDpi() const { return qualityDpi_; }      // rasterization DPI cap

    // True when the user chose "Print using system dialogue…": the caller should
    // discard the selections above and drive the native QPrintDialog instead.
    bool useSystemDialog() const { return useSystemDialog_; }

private:
    void accept() override; // apply the selections to printer_, then close

    QPrinter *printer_; // non-owning
    int pageCount_ = 1;
    int currentPage_ = 1; // 1-based page the user is viewing
    QString suggestedFileName_;

    QList<int> pages_;
    ScaleMode scaleMode_ = ScaleMode::FitToPage;
    int scalePercent_ = 100;
    int qualityDpi_ = 300;
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
    QCheckBox *twoSidedCheck_ = nullptr;
    QCheckBox *printToFile_ = nullptr;
};
