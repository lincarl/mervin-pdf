#pragma once

#include "print/PrintLayout.h"
#include "print/PrintPreferences.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPrinter>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPageSize;
class QPushButton;
class QRadioButton;
class QScrollArea;
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
    // Mirrors QPrinterInfo's media metadata without reducing driver media to an ID.
    struct PaperCapabilities
    {
        QList<QPageSize> supported;
        bool custom = false;
        QPageSize minimum;
        QPageSize maximum;

        // Returns the driver's exact definition, valid custom paper, or invalid paper.
        // An empty list without custom support means metadata is unavailable. In that
        // case QPrinter::setPageSize remains the final best-effort validation.
        QPageSize acceptedPaper(const QPageSize &paper) const;
    };
    using PaperQuery = std::function<PaperCapabilities(const QString &)>;

    PrintDialog(QPrinter *printer, mervin::RenderEngine *engine, mervin::Document *document,
                int rotation, QPageLayout::Orientation initialOrientation, int currentPage,
                const QString &suggestedFileName, QWidget *parent = nullptr,
                PaperQuery paperQuery = {});

    // Valid after exec() == Accepted, when useSystemDialog() == false.
    QList<int> selectedPages() const { return pages_; } // 1-based, in print order
    mervin::printing::Settings printSettings() const;

    // Captures dialog choices without the output filename or computed automatic scale.
    mervin::printing::Preferences preferences() const;
    // Restores saved choices against this document and the available printers.
    void restorePreferences(const mervin::printing::Preferences &preferences);

    // The caller should open the native print dialog instead of using this selection.
    bool useSystemDialog() const { return useSystemDialog_; }

private:
    void accept() override;
    QList<int> resolvePages(QString *error) const;
    QPageSize selectedPrinterDefaultPageSize();
    const PaperCapabilities &selectedPaperCapabilities();
    void refreshPaperSizes(const QPageSize &preferred);
    void selectPaperSize(const QPageSize &size);
    void updatePageLayoutControls();
    void schedulePreview();
    bool refreshPreview();
    bool applyPrinterSettings(QString *error);
    void updateNavigation();

    QPrinter *printer_; // non-owning
    int pageCount_ = 1;
    int currentPage_ = 1;
    int previewIndex_ = 0; // index into pages_, including repeated custom pages
    int customScalePercent_ = 100;
    QString suggestedFileName_;
    QMarginsF preferredMargins_;
    QList<int> pages_;
    bool useSystemDialog_ = false;
    bool printerSelectionChanged_ = false;
    // Saved choices replace constructor defaults for native handoff. Live custom
    // edits leave this baseline intact, as they did before preference persistence.
    struct NativeSettings
    {
        QPageLayout layout;
        int copies = 1;
        QPrinter::ColorMode colour = QPrinter::Color;
        QPrinter::DuplexMode duplex = QPrinter::DuplexNone;
        QPrinter::PrintRange range = QPrinter::AllPages;
        int from = 1;
        int to = 1;
    } nativeSettings_;
    mervin::printing::DuplexMode duplexMode_ = mervin::printing::DuplexMode::LongSide;
    PaperQuery paperQuery_;
    QHash<QString, PaperCapabilities> paperCapabilities_;

    QComboBox *printerCombo_ = nullptr;
    QComboBox *paperSizeCombo_ = nullptr;
    QScrollArea *settingsScroll_ = nullptr;
    QComboBox *pageLayoutCombo_ = nullptr;
    QWidget *repeatOptions_ = nullptr;
    QSpinBox *rowsSpin_ = nullptr;
    QSpinBox *columnsSpin_ = nullptr;
    QComboBox *pageOrientationCombo_ = nullptr;
    QLabel *repeatCountLabel_ = nullptr;
    QLabel *marginNote_ = nullptr;
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
