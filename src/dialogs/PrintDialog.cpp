#include "dialogs/PrintDialog.h"

#include "dialogs/LayoutMinimum.h"
#include "print/PageRange.h"
#include "print/PrintPreviewWidget.h"
#include "render/Document.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPageSize>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>
#include <vector>

using mervin::printing::Alignment;
using mervin::printing::ColourMode;
using mervin::printing::PageLayoutMode;
using mervin::printing::PageOrientation;
using mervin::printing::ScaleMode;

namespace {
// The settings column fits English at this width. A longer translation widens it.
constexpr int kSettingsWidth = 390;
constexpr int kDefaultPaperSizeRole = Qt::UserRole + 1;
constexpr QPageSize::PageSizeId kCommonSizes[] = {
    QPageSize::A3, QPageSize::A4, QPageSize::A5, QPageSize::B4, QPageSize::B5,
    QPageSize::Letter, QPageSize::Legal, QPageSize::Tabloid, QPageSize::Executive,
};

// Converts saved semantic duplex choices without losing the native binding edge.
QPrinter::DuplexMode nativeDuplex(mervin::printing::DuplexMode mode)
{
    using mervin::printing::DuplexMode;
    switch (mode) {
    case DuplexMode::None: return QPrinter::DuplexNone;
    case DuplexMode::Auto: return QPrinter::DuplexAuto;
    case DuplexMode::LongSide: return QPrinter::DuplexLongSide;
    case DuplexMode::ShortSide: return QPrinter::DuplexShortSide;
    }
    return QPrinter::DuplexNone;
}
} // namespace

QPageSize PrintDialog::PaperCapabilities::acceptedPaper(const QPageSize &paper) const
{
    if (!paper.isValid())
        return {};
    for (const QPageSize &reported : supported) {
        if (reported.key() == paper.key() && reported.isEquivalentTo(paper))
            return reported;
    }
    if (supported.isEmpty() && !custom)
        return paper;
    if (!custom)
        return {};
    // Ordinary named sizes may also fit a variable-size device. A driver-specific
    // variant key, however, must be explicitly reported, even at the same dimensions.
    if (paper.id() != QPageSize::Custom && paper.key() != QPageSize(paper.id()).key())
        return {};
    const QSizeF size = paper.size(QPageSize::Point);
    const QSizeF low = minimum.size(QPageSize::Point);
    const QSizeF high = maximum.size(QPageSize::Point);
    if ((minimum.isValid() && (size.width() < low.width() || size.height() < low.height()))
        || (maximum.isValid() && (size.width() > high.width() || size.height() > high.height())))
        return {};
    return paper;
}

PrintDialog::PrintDialog(QPrinter *printer, mervin::RenderEngine *engine,
                         mervin::Document *document, int rotation,
                         QPageLayout::Orientation initialOrientation, int currentPage,
                         const QString &suggestedFileName, QWidget *parent, PaperQuery paperQuery)
    : QDialog(parent)
    , printer_(printer)
    , pageCount_(document ? document->pageCount() : 0)
    , currentPage_(qBound(1, currentPage, qMax(1, pageCount_)))
    , suggestedFileName_(suggestedFileName)
    , preferredMargins_(printer->pageLayout().margins(QPageLayout::Point))
    , paperQuery_(std::move(paperQuery))
{
    //: Dialog title.
    setWindowTitle(tr("Print"));
    setObjectName(QStringLiteral("printDialog"));
    nativeSettings_ = {printer_->pageLayout(), printer_->copyCount(), printer_->colorMode(),
                       printer_->duplex(), QPrinter::AllPages, 1, qMax(1, pageCount_)};
    nativeSettings_.layout.setOrientation(initialOrientation);
    if (printer_->duplex() == QPrinter::DuplexAuto)
        duplexMode_ = mervin::printing::DuplexMode::Auto;
    else if (printer_->duplex() == QPrinter::DuplexShortSide)
        duplexMode_ = mervin::printing::DuplexMode::ShortSide;

    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    layout->addLayout(body, 1);

    // Keep settings reachable on short screens while the preview fits its available area.
    auto *settingsScroll = settingsScroll_ = new QScrollArea(this);
    settingsScroll->setObjectName(QStringLiteral("printSettingsScroll"));
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setFixedWidth(kSettingsWidth); // widened below if the settings need it
    auto *settings = new QWidget(settingsScroll);
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(0, 0, 10, 0);
    settingsScroll->setWidget(settings);
    body->addWidget(settingsScroll);

    auto *printerBox = new QGroupBox(tr("Printer"), settings);
    auto *printerForm = new QFormLayout(printerBox);
    printerCombo_ = new QComboBox(printerBox);
    printerCombo_->setObjectName(QStringLiteral("printPrinter"));
    // Include full printer names when calculating the settings column's width.
    printerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    const QList<QPrinterInfo> printers = QPrinterInfo::availablePrinters();
    const QString currentName = printer_->printerName().isEmpty()
                                    ? QPrinterInfo::defaultPrinterName()
                                    : printer_->printerName();
    for (const QPrinterInfo &info : printers) {
        QString label = info.printerName();
        if (!info.description().isEmpty() && info.description() != info.printerName()) {
            //: A printer in the list: %1 is its name, %2 its description.
            label = tr("%1 (%2)").arg(info.printerName(), info.description());
        }
        printerCombo_->addItem(label, info.printerName());
        if (info.printerName() == currentName)
            printerCombo_->setCurrentIndex(printerCombo_->count() - 1);
    }
    if (printers.isEmpty())
        printerCombo_->addItem(tr("No printers available"));
    //: Label of the printer list.
    printerForm->addRow(tr("Name:"), printerCombo_);

    paperSizeCombo_ = new QComboBox(printerBox);
    paperSizeCombo_->setObjectName(QStringLiteral("printPaperSize"));
    const QPageSize currentSize = printer_->pageLayout().pageSize();
    for (QPageSize::PageSizeId id : kCommonSizes)
        paperSizeCombo_->addItem(QPageSize(id).name(), QVariant::fromValue(QPageSize(id)));
    selectPaperSize(currentSize.isValid() ? currentSize : QPageSize(QPageSize::A4));
    printerForm->addRow(tr("Paper size:"), paperSizeCombo_);

    printToFile_ = new QCheckBox(tr("Print to a PDF file instead"), printerBox);
    printToFile_->setObjectName(QStringLiteral("printToFile"));
    printToFile_->setChecked(printers.isEmpty() || printer_->outputFormat() == QPrinter::PdfFormat);
    printToFile_->setEnabled(!printers.isEmpty());
    printerCombo_->setEnabled(!printToFile_->isChecked());
    printerForm->addRow(QString(), printToFile_);
    connect(printToFile_, &QCheckBox::toggled, printerCombo_, &QWidget::setDisabled);
    settingsLayout->addWidget(printerBox);

    // Group-box parents keep orientation and page-selection radios independent.
    auto *orientationBox = new QGroupBox(tr("Orientation"), settings);
    auto *orientationRow = new QHBoxLayout(orientationBox);
    portraitRadio_ = new QRadioButton(tr("Portrait"), orientationBox);
    portraitRadio_->setObjectName(QStringLiteral("printPortrait"));
    landscapeRadio_ = new QRadioButton(tr("Landscape"), orientationBox);
    landscapeRadio_->setObjectName(QStringLiteral("printLandscape"));
    orientationRow->addWidget(portraitRadio_);
    orientationRow->addWidget(landscapeRadio_);
    orientationRow->addStretch();
    if (initialOrientation == QPageLayout::Landscape)
        landscapeRadio_->setChecked(true);
    else
        portraitRadio_->setChecked(true);
    settingsLayout->addWidget(orientationBox);

    //: Group heading over the choice of which pages to print.
    auto *pagesBox = new QGroupBox(tr("Pages"), settings);
    auto *pagesLayout = new QVBoxLayout(pagesBox);
    // A one-page document names its page rather than a range.
    QString allLabel;
    if (pageCount_ > 1) {
        //: %1 is the document's last page.
        allLabel = tr("All pages (page 1-%1)").arg(pageCount_);
    } else {
        allLabel = tr("All pages (page 1)");
    }
    allPagesRadio_ = new QRadioButton(allLabel, pagesBox);
    allPagesRadio_->setObjectName(QStringLiteral("printAllPages"));
    allPagesRadio_->setChecked(true);
    pagesLayout->addWidget(allPagesRadio_);
    //: %1 is the page number shown in the viewer.
    currentPageRadio_ = new QRadioButton(tr("Current page (page %1)").arg(currentPage_), pagesBox);
    currentPageRadio_->setObjectName(QStringLiteral("printCurrentPage"));
    pagesLayout->addWidget(currentPageRadio_);

    auto *rangeRow = new QHBoxLayout;
    // The row reads "Pages from [first] to [last]": a radio button, a spin box,
    // a label and another spin box, so the sentence comes in two pieces.
    //: Radio button followed by a page number field, then "to" and another
    //: page number field: "Pages from [1] to [12]".
    rangeRadio_ = new QRadioButton(tr("Pages from"), pagesBox);
    rangeRadio_->setObjectName(QStringLiteral("printPageRange"));
    fromSpin_ = new QSpinBox(pagesBox);
    fromSpin_->setObjectName(QStringLiteral("printFromPage"));
    fromSpin_->setRange(1, qMax(1, pageCount_));
    fromSpin_->setValue(1);
    toSpin_ = new QSpinBox(pagesBox);
    toSpin_->setObjectName(QStringLiteral("printToPage"));
    toSpin_->setRange(1, qMax(1, pageCount_));
    toSpin_->setValue(qMax(1, pageCount_));
    mervin::Theme::useSteppedSpinBox(fromSpin_);
    mervin::Theme::useSteppedSpinBox(toSpin_);
    rangeRow->addWidget(rangeRadio_);
    rangeRow->addWidget(fromSpin_);
    //: Between the two page number fields of "Pages from [1] to [12]".
    rangeRow->addWidget(new QLabel(tr("to"), pagesBox));
    rangeRow->addWidget(toSpin_);
    rangeRow->addStretch();
    pagesLayout->addLayout(rangeRow);
    fromSpin_->setEnabled(false);
    toSpin_->setEnabled(false);
    connect(rangeRadio_, &QRadioButton::toggled, fromSpin_, &QWidget::setEnabled);
    connect(rangeRadio_, &QRadioButton::toggled, toSpin_, &QWidget::setEnabled);
    connect(fromSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (value > toSpin_->value())
            toSpin_->setValue(value);
    });
    connect(toSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (value < fromSpin_->value())
            fromSpin_->setValue(value);
    });

    auto *customRow = new QHBoxLayout;
    //: Page selection option: print the pages typed in the field next to it.
    customRadio_ = new QRadioButton(tr("Custom", "page selection"), pagesBox);
    customRadio_->setObjectName(QStringLiteral("printCustomPages"));
    customEdit_ = new QLineEdit(pagesBox);
    customEdit_->setObjectName(QStringLiteral("printPages"));
    customEdit_->setPlaceholderText(tr("e.g. 1-3, 5, 8-10"));
    customRow->addWidget(customRadio_);
    customRow->addWidget(customEdit_, 1);
    pagesLayout->addLayout(customRow);
    connect(customEdit_, &QLineEdit::textEdited, this, [this] {
        customRadio_->setChecked(true);
    });
    settingsLayout->addWidget(pagesBox);

    //: Group for arranging a source PDF page on the physical printed sheet.
    auto *pageLayoutBox = new QGroupBox(tr("Page layout"), settings);
    auto *pageLayout = new QVBoxLayout(pageLayoutBox);
    pageLayoutCombo_ = new QComboBox(pageLayoutBox);
    pageLayoutCombo_->setObjectName(QStringLiteral("printPageLayout"));
    pageLayoutCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    //: Print layout option. Each selected PDF page appears once on its own sheet.
    pageLayoutCombo_->addItem(tr("One per sheet"), static_cast<int>(PageLayoutMode::Single));
    //: Print layout option. Repeat each selected PDF page in a grid on its own sheet,
    //: rather than arranging different source pages together.
    pageLayoutCombo_->addItem(tr("Repeat page"), static_cast<int>(PageLayoutMode::Repeat));
    pageLayout->addWidget(pageLayoutCombo_);
    repeatOptions_ = new QWidget(pageLayoutBox);
    auto *repeatLayout = new QVBoxLayout(repeatOptions_);
    repeatLayout->setContentsMargins(0, 0, 0, 0);
    auto *dimensions = new QWidget(repeatOptions_);
    auto *dimensionLayout = new QGridLayout(dimensions);
    dimensionLayout->setContentsMargins(0, 0, 0, 0);
    dimensionLayout->setSizeConstraint(QLayout::SetMinimumSize);
    rowsSpin_ = new QSpinBox(dimensions);
    rowsSpin_->setObjectName(QStringLiteral("printRows"));
    rowsSpin_->setRange(1, 8);
    rowsSpin_->setValue(2);
    mervin::Theme::useSteppedSpinBox(rowsSpin_);
    auto *rowsLabel = new QLabel(tr("Rows"), dimensions);
    rowsLabel->setBuddy(rowsSpin_);
    dimensionLayout->addWidget(rowsLabel, 0, 0);
    dimensionLayout->addWidget(rowsSpin_, 1, 0);
    dimensionLayout->addWidget(new QLabel(QStringLiteral("×"), dimensions), 1, 1, Qt::AlignCenter);
    columnsSpin_ = new QSpinBox(dimensions);
    columnsSpin_->setObjectName(QStringLiteral("printColumns"));
    columnsSpin_->setRange(1, 8);
    columnsSpin_->setValue(4);
    mervin::Theme::useSteppedSpinBox(columnsSpin_);
    auto *columnsLabel = new QLabel(tr("Columns"), dimensions);
    columnsLabel->setBuddy(columnsSpin_);
    dimensionLayout->addWidget(columnsLabel, 0, 2);
    dimensionLayout->addWidget(columnsSpin_, 1, 2);
    dimensionLayout->setColumnStretch(0, 1);
    dimensionLayout->setColumnStretch(2, 1);
    repeatLayout->addWidget(dimensions);
    auto *pageOrientationForm = new QFormLayout;
    pageOrientationCombo_ = new QComboBox(repeatOptions_);
    pageOrientationCombo_->setObjectName(QStringLiteral("printPageOrientation"));
    pageOrientationCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    //: Repeated-page orientation option. Keep the page's current viewer rotation.
    pageOrientationCombo_->addItem(tr("As shown"), static_cast<int>(PageOrientation::AsShown));
    pageOrientationCombo_->addItem(tr("Portrait"), static_cast<int>(PageOrientation::Portrait));
    pageOrientationCombo_->addItem(tr("Landscape"), static_cast<int>(PageOrientation::Landscape));
    //: Orientation of each repeated source page, independent of the physical paper orientation.
    pageOrientationForm->addRow(tr("Page orientation:"), pageOrientationCombo_);
    repeatLayout->addLayout(pageOrientationForm);
    repeatCountLabel_ = new QLabel(repeatOptions_);
    repeatCountLabel_->setObjectName(QStringLiteral("printRepeatCount"));
    repeatCountLabel_->setWordWrap(true);
    repeatLayout->addWidget(repeatCountLabel_);
    pageLayout->addWidget(repeatOptions_);
    settingsLayout->addWidget(pageLayoutBox);

    auto *optionsBox = new QGroupBox(tr("Options"), settings);
    auto *optionsForm = new QFormLayout(optionsBox);
    copiesSpin_ = new QSpinBox(optionsBox);
    copiesSpin_->setObjectName(QStringLiteral("printCopies"));
    copiesSpin_->setRange(1, 999);
    copiesSpin_->setValue(qMax(1, printer_->copyCount()));
    //: Tooltip for the Copies control, distinct from repetitions within one sheet.
    copiesSpin_->setToolTip(tr("Copies of the complete print job."));
    mervin::Theme::useSteppedSpinBox(copiesSpin_);
    optionsForm->addRow(tr("Copies:"), copiesSpin_);

    colorCombo_ = new QComboBox(optionsBox);
    colorCombo_->setObjectName(QStringLiteral("printColour"));
    //: Colour mode option: print in colour.
    colorCombo_->addItem(tr("Colour"), static_cast<int>(ColourMode::Colour));
    colorCombo_->addItem(tr("Grayscale"), static_cast<int>(ColourMode::Grayscale));
    colorCombo_->addItem(tr("Black and white"), static_cast<int>(ColourMode::BlackAndWhite));
    colorCombo_->setCurrentIndex(printer_->colorMode() == QPrinter::GrayScale ? 1 : 0);
    optionsForm->addRow(tr("Colour:"), colorCombo_);

    qualityCombo_ = new QComboBox(optionsBox);
    qualityCombo_->setObjectName(QStringLiteral("printQuality"));
    //: Print quality option. dpi is dots per inch.
    qualityCombo_->addItem(tr("Draft (150 dpi)"), 150);
    qualityCombo_->addItem(tr("Normal (300 dpi)"), 300);
    qualityCombo_->addItem(tr("High (600 dpi)"), 600);
    qualityCombo_->setCurrentIndex(1);
    optionsForm->addRow(tr("Quality:"), qualityCombo_);

    auto *scaleRow = new QHBoxLayout;
    scaleCombo_ = new QComboBox(optionsBox);
    scaleCombo_->setObjectName(QStringLiteral("printScale"));
    scaleCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    scaleCombo_->addItem(tr("Fit to page"), static_cast<int>(ScaleMode::FitToPage));
    scaleCombo_->addItem(tr("Actual size"), static_cast<int>(ScaleMode::ActualSize));
    //: Scale option: print at the percentage set next to it.
    scaleCombo_->addItem(tr("Custom", "scale"), static_cast<int>(ScaleMode::Custom));
    scalePercentSpin_ = new QSpinBox(optionsBox);
    scalePercentSpin_->setObjectName(QStringLiteral("printScalePercent"));
    scalePercentSpin_->setRange(10, 400);
    scalePercentSpin_->setValue(100);
    scalePercentSpin_->setSuffix(QStringLiteral(" %")); // a unit symbol, not translated
    mervin::Theme::useSteppedSpinBox(scalePercentSpin_);
    scalePercentSpin_->setEnabled(false);
    scaleRow->addWidget(scaleCombo_, 1);
    scaleRow->addWidget(scalePercentSpin_);
    optionsForm->addRow(tr("Scale:"), scaleRow);
    connect(scaleCombo_, &QComboBox::currentIndexChanged, this, [this] {
        const QSignalBlocker blocker(scalePercentSpin_);
        const auto mode = static_cast<ScaleMode>(scaleCombo_->currentData().toInt());
        scalePercentSpin_->setEnabled(mode == ScaleMode::Custom);
        scalePercentSpin_->setSpecialValueText(QString());
        if (mode == ScaleMode::Custom) {
            scalePercentSpin_->setRange(10, 400);
            scalePercentSpin_->setValue(customScalePercent_);
        } else if (mode == ScaleMode::ActualSize) {
            scalePercentSpin_->setValue(100);
        } else {
            scalePercentSpin_->clear();
        }
    });
    connect(scalePercentSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (scaleCombo_->currentData().toInt() == static_cast<int>(ScaleMode::Custom))
            customScalePercent_ = value;
    });

    alignmentCombo_ = new QComboBox(optionsBox);
    alignmentCombo_->setObjectName(QStringLiteral("printAlignment"));
    alignmentCombo_->addItem(tr("Centered"), static_cast<int>(Alignment::Centered));
    alignmentCombo_->addItem(tr("Centered vertically"),
                             static_cast<int>(Alignment::CenteredVertically));
    alignmentCombo_->addItem(tr("Centered horizontally"),
                             static_cast<int>(Alignment::CenteredHorizontally));
    alignmentCombo_->addItem(tr("Top left"), static_cast<int>(Alignment::TopLeft));
    optionsForm->addRow(tr("Alignment:"), alignmentCombo_);

    twoSidedCheck_ = new QCheckBox(tr("Print on both sides"), optionsBox);
    twoSidedCheck_->setObjectName(QStringLiteral("printTwoSided"));
    twoSidedCheck_->setChecked(printer_->duplex() != QPrinter::DuplexNone);
    twoSidedCheck_->setEnabled(!printToFile_->isChecked());
    connect(printToFile_, &QCheckBox::toggled, twoSidedCheck_, &QWidget::setDisabled);
    optionsForm->addRow(tr("Two-sided:"), twoSidedCheck_);
    settingsLayout->addWidget(optionsBox);
    settingsLayout->addStretch();
    // The column is fixed so the preview gets the rest. When a translation makes
    // the settings wider than it, widen it rather than clip them, keeping room for
    // the vertical scroll bar.
    settingsScroll->ensurePolished();
    const int scrollBarWidth = settingsScroll->verticalScrollBar()->sizeHint().width();
    settingsScroll->setFixedWidth(
        qMax(kSettingsWidth, settings->minimumSizeHint().width() + scrollBarWidth));

    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    body->addWidget(divider);
    auto *previewColumn = new QVBoxLayout;
    body->addLayout(previewColumn, 1);
    //: Heading over the print preview.
    auto *previewTitle = new QLabel(tr("Preview"), this);
    QFont titleFont = previewTitle->font();
    titleFont.setBold(true);
    previewTitle->setFont(titleFont);
    previewTitle->setContentsMargins(8, 4, 8, 4);
    previewColumn->addWidget(previewTitle);
    preview_ = new mervin::PrintPreviewWidget(engine, document, rotation, this);
    preview_->setObjectName(QStringLiteral("printPreview"));
    preview_->setMinimumSize(300, 240);
    previewColumn->addWidget(preview_, 1);

    auto *navigation = new QHBoxLayout;
    previousButton_ = new QPushButton(this);
    previousButton_->setObjectName(QStringLiteral("printPreviousPage"));
    previousButton_->setAccessibleName(tr("Previous preview page"));
    previousButton_->setFixedSize(30, 30);
    previousButton_->setAutoDefault(false);
    mervin::icons::setButtonGlyph(previousButton_, mervin::icons::Glyph::PrevPage, 16);
    navigation->addWidget(previousButton_);
    pageLabel_ = new QLabel(this);
    pageLabel_->setObjectName(QStringLiteral("printPageLabel"));
    pageLabel_->setAlignment(Qt::AlignCenter);
    // Large page numbers and translated selection summaries may need two lines.
    pageLabel_->setWordWrap(true);
    pageLabel_->setMinimumWidth(140);
    navigation->addWidget(pageLabel_, 1);
    nextButton_ = new QPushButton(this);
    nextButton_->setObjectName(QStringLiteral("printNextPage"));
    nextButton_->setAccessibleName(tr("Next preview page"));
    nextButton_->setFixedSize(30, 30);
    nextButton_->setAutoDefault(false);
    mervin::icons::setButtonGlyph(nextButton_, mervin::icons::Glyph::NextPage, 16);
    navigation->addWidget(nextButton_);
    previewColumn->addLayout(navigation);

    auto *previewFooter = new QVBoxLayout;
    paperLabel_ = new QLabel(this);
    paperLabel_->setObjectName(QStringLiteral("printPaperLabel"));
    paperLabel_->setWordWrap(true);
    previewFooter->addWidget(paperLabel_);
    marginNote_ = new QLabel(this);
    marginNote_->setWordWrap(true);
    marginNote_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    previewFooter->addWidget(marginNote_);
    previewColumn->addLayout(previewFooter);

    auto *buttons = new QHBoxLayout;
    auto *systemButton = new QPushButton(tr("Print using system dialogue…"), this);
    buttons->addWidget(systemButton);
    buttons->addStretch();
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("printCancel"));
    cancelButton->setAutoDefault(false);
    buttons->addWidget(cancelButton);
    //: Button: starts printing.
    printButton_ = new QPushButton(tr("Print"), this);
    printButton_->setObjectName(QStringLiteral("printConfirm"));
    printButton_->setDefault(true);
    printButton_->setEnabled(false);
    buttons->addWidget(printButton_);
    systemButton->setObjectName(QStringLiteral("printSystemDialog"));
    systemButton->setEnabled(!printers.isEmpty());
    systemButton->setAutoDefault(false);
    systemButton->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(systemButton, &QPushButton::clicked, this, [this] {
        // Live preview changes the supplied printer. Restore the saved baseline
        // before native handoff, or the constructor defaults for an unsaved file.
        printer_->setOutputFileName(QString());
        printer_->setOutputFormat(QPrinter::NativeFormat);
        printer_->setPrinterName(printerCombo_->currentData().toString());
        printer_->setPageLayout(nativeSettings_.layout);
        if (printerSelectionChanged_) {
            const QPageSize defaultSize = selectedPrinterDefaultPageSize();
            if (defaultSize.isValid())
                printer_->setPageSize(defaultSize);
        }
        printer_->setPageOrientation(nativeSettings_.layout.orientation());
        printer_->setCopyCount(nativeSettings_.copies);
        printer_->setColorMode(nativeSettings_.colour);
        printer_->setDuplex(nativeSettings_.duplex);
        printer_->setFromTo(nativeSettings_.from, nativeSettings_.to);
        printer_->setPrintRange(nativeSettings_.range);
        useSystemDialog_ = true;
        QDialog::accept();
    });
    connect(printButton_, &QPushButton::clicked, this, &PrintDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &PrintDialog::reject);
    layout->addLayout(buttons);

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(100);
    connect(previewTimer_, &QTimer::timeout, this, [this] { refreshPreview(); });
    connect(this, &QDialog::finished, previewTimer_, &QTimer::stop);
    connect(printerCombo_, &QComboBox::currentIndexChanged, this, [this] {
        // QPrinter can carry the previous device's paper size across setPrinterName().
        // Select the driver's default before any refresh applies the dialog's options.
        printerSelectionChanged_ = true;
        const QPageSize defaultSize = selectedPrinterDefaultPageSize();
        refreshPaperSizes(defaultSize.isValid() ? defaultSize
            : paperSizeCombo_->currentData().value<QPageSize>());
        schedulePreview();
    });
    for (QComboBox *combo : {paperSizeCombo_, colorCombo_, qualityCombo_,
                            scaleCombo_, alignmentCombo_, pageOrientationCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &PrintDialog::schedulePreview);
    for (QSpinBox *spin : {copiesSpin_, fromSpin_, toSpin_, scalePercentSpin_})
        connect(spin, &QSpinBox::valueChanged, this, &PrintDialog::schedulePreview);
    connect(pageLayoutCombo_, &QComboBox::currentIndexChanged, this, [this] {
        updatePageLayoutControls();
        schedulePreview();
    });
    for (QSpinBox *spin : {rowsSpin_, columnsSpin_}) {
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            updatePageLayoutControls();
            schedulePreview();
        });
    }
    for (QRadioButton *radio : {portraitRadio_, landscapeRadio_, allPagesRadio_,
                               currentPageRadio_, rangeRadio_, customRadio_}) {
        connect(radio, &QRadioButton::toggled, this, [this](bool checked) {
            if (checked)
                schedulePreview();
        });
    }
    connect(customEdit_, &QLineEdit::textChanged, this, &PrintDialog::schedulePreview);
    connect(printToFile_, &QCheckBox::toggled, this, [this] {
        refreshPaperSizes(paperSizeCombo_->currentData().value<QPageSize>());
        schedulePreview();
    });
    connect(twoSidedCheck_, &QCheckBox::toggled, this, &PrintDialog::schedulePreview);
    connect(previousButton_, &QPushButton::clicked, this, [this] {
        if (previewIndex_ > 0) {
            --previewIndex_;
            refreshPreview();
        }
    });
    connect(nextButton_, &QPushButton::clicked, this, [this] {
        if (previewIndex_ + 1 < pages_.size()) {
            ++previewIndex_;
            refreshPreview();
        }
    });

    const QRect screenArea = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 900);
    resize(qMin(1180, screenArea.width() - 60), qMin(800, screenArea.height() - 80));
    updatePageLayoutControls();
    refreshPaperSizes(currentSize);
    refreshPreview();
    // Wrapped selection captions must not push navigation over the preview.
    mervin::followLayoutMinimum(*this, QSize(800, 480));
}

// Wait for the native frame and settings viewport before measuring their height.
void PrintDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    QTimer::singleShot(0, this, &PrintDialog::growToFitSettings);
}

// Use available desktop height before scrolling without raising the scrollable
// minimum or shrinking a window the user has already enlarged.
void PrintDialog::growToFitSettings()
{
    if (!isVisible() || isMaximized() || isFullScreen() || isMinimized() || !screen())
        return;
    layout()->activate();
    QWidget *settings = settingsScroll_->widget();
    settings->layout()->activate();
    const int contentHeight = settings->hasHeightForWidth()
        ? settings->heightForWidth(settings->width()) : settings->sizeHint().height();
    const int requiredHeight = height() + contentHeight - settingsScroll_->viewport()->height();
    const int frameHeight = frameGeometry().height() - height();
    const int availableHeight = screen()->availableGeometry().height() - frameHeight;
    const int preferredHeight = qMin(requiredHeight, availableHeight);
    if (preferredHeight > height())
        resize(width(), preferredHeight);
}

mervin::printing::Settings PrintDialog::printSettings() const
{
    mervin::printing::Settings settings;
    settings.scaleMode = static_cast<ScaleMode>(scaleCombo_->currentData().toInt());
    settings.scalePercent = customScalePercent_;
    settings.alignment = static_cast<Alignment>(alignmentCombo_->currentData().toInt());
    settings.colourMode = static_cast<ColourMode>(colorCombo_->currentData().toInt());
    settings.qualityDpi = qualityCombo_->currentData().toInt();
    settings.layoutMode = static_cast<PageLayoutMode>(pageLayoutCombo_->currentData().toInt());
    settings.rows = rowsSpin_->value();
    settings.columns = columnsSpin_->value();
    settings.pageOrientation = static_cast<PageOrientation>(pageOrientationCombo_->currentData().toInt());
    return settings;
}

mervin::printing::Preferences PrintDialog::preferences() const
{
    using mervin::printing::PageSelection;
    mervin::printing::Preferences saved;
    saved.printerName = printerCombo_->currentData().toString();
    saved.printToFile = printToFile_->isChecked();
    const QPageSize paper = paperSizeCombo_->currentData().value<QPageSize>();
    saved.paperKey = paper.key();
    saved.paperPoints = paper.size(QPageSize::Point);
    saved.orientation = landscapeRadio_->isChecked() ? QPageLayout::Landscape
                                                     : QPageLayout::Portrait;
    saved.pageSelection = customRadio_->isChecked() ? PageSelection::Custom
        : rangeRadio_->isChecked() ? PageSelection::Range
        : currentPageRadio_->isChecked() ? PageSelection::Current : PageSelection::All;
    saved.fromPage = fromSpin_->value();
    saved.toPage = toSpin_->value();
    saved.customPages = customEdit_->text();
    saved.copies = copiesSpin_->value();
    saved.settings = printSettings();
    saved.duplex = twoSidedCheck_->isChecked() ? duplexMode_
                                               : mervin::printing::DuplexMode::None;
    return saved;
}

void PrintDialog::restorePreferences(const mervin::printing::Preferences &saved)
{
    using mervin::printing::PageSelection;
    // Apply a complete snapshot before refreshing. Intermediate selections must not
    // reset printer paper, change range endpoints or overwrite the custom scale.
    const QList<QObject *> controls{
        printerCombo_, printToFile_, paperSizeCombo_, portraitRadio_, landscapeRadio_,
        allPagesRadio_, currentPageRadio_, rangeRadio_, customRadio_, fromSpin_, toSpin_,
        customEdit_, copiesSpin_, colorCombo_, qualityCombo_, scaleCombo_, scalePercentSpin_,
        alignmentCombo_, twoSidedCheck_, pageLayoutCombo_, rowsSpin_, columnsSpin_,
        pageOrientationCombo_};
    std::vector<QSignalBlocker> blockers;
    blockers.reserve(controls.size());
    for (QObject *control : controls)
        blockers.emplace_back(control);

    const int printerIndex = printerCombo_->findData(saved.printerName);
    if (printerIndex >= 0 && printerIndex != printerCombo_->currentIndex()) {
        printerSelectionChanged_ = true;
        printerCombo_->setCurrentIndex(printerIndex);
    }
    const bool hasPrinter = !printerCombo_->currentData().toString().isEmpty();
    printToFile_->setChecked(saved.printToFile || !hasPrinter);
    printerCombo_->setEnabled(!printToFile_->isChecked());
    twoSidedCheck_->setEnabled(!printToFile_->isChecked());

    QPageSize paper(saved.paperPoints, QPageSize::Point, QString(), QPageSize::ExactMatch);
    if (!printToFile_->isChecked()) {
        const PaperCapabilities &capabilities = selectedPaperCapabilities();
        for (const QPageSize &reported : capabilities.supported) {
            if (reported.key() == saved.paperKey && reported.isEquivalentTo(paper)) {
                paper = reported;
                break;
            }
        }
        paper = capabilities.acceptedPaper(paper);
        // A saved printer or stock may no longer exist. Start with an available
        // sheet rather than leaving a stale choice that makes printing impossible.
        if (!paper.isValid())
            paper = capabilities.acceptedPaper(selectedPrinterDefaultPageSize());
        if (!paper.isValid() && !capabilities.supported.isEmpty())
            paper = capabilities.supported.first();
    }
    if (!paper.isValid())
        paper = printer_->pageLayout().pageSize();
    refreshPaperSizes(paper);
    (saved.orientation == QPageLayout::Landscape ? landscapeRadio_ : portraitRadio_)
        ->setChecked(true);
    copiesSpin_->setValue(saved.copies);
    duplexMode_ = saved.duplex == mervin::printing::DuplexMode::None
        ? mervin::printing::DuplexMode::LongSide : saved.duplex;
    twoSidedCheck_->setChecked(saved.duplex != mervin::printing::DuplexMode::None);

    const auto select = [](QComboBox *combo, int value) {
        const int index = combo->findData(value);
        if (index >= 0)
            combo->setCurrentIndex(index);
    };
    select(colorCombo_, static_cast<int>(saved.settings.colourMode));
    select(qualityCombo_, saved.settings.qualityDpi);
    select(alignmentCombo_, static_cast<int>(saved.settings.alignment));
    select(scaleCombo_, static_cast<int>(saved.settings.scaleMode));
    select(pageLayoutCombo_, static_cast<int>(saved.settings.layoutMode));
    rowsSpin_->setValue(saved.settings.rows);
    columnsSpin_->setValue(saved.settings.columns);
    select(pageOrientationCombo_, static_cast<int>(saved.settings.pageOrientation));
    updatePageLayoutControls();
    customScalePercent_ = qBound(10, saved.settings.scalePercent, 400);
    scalePercentSpin_->setSpecialValueText(QString());
    scalePercentSpin_->setRange(10, 400);
    scalePercentSpin_->setValue(printSettings().scaleMode == ScaleMode::ActualSize
                                   ? 100 : customScalePercent_);
    scalePercentSpin_->setEnabled(printSettings().scaleMode == ScaleMode::Custom);

    fromSpin_->setValue(saved.fromPage);
    toSpin_->setValue(qMax(fromSpin_->value(), saved.toPage));
    customEdit_->setText(saved.customPages);
    QRadioButton *selection = allPagesRadio_;
    switch (saved.pageSelection) {
    case PageSelection::All: break;
    case PageSelection::Current: selection = currentPageRadio_; break;
    case PageSelection::Range: selection = rangeRadio_; break;
    case PageSelection::Custom: selection = customRadio_; break;
    }
    selection->setChecked(true);
    fromSpin_->setEnabled(rangeRadio_->isChecked());
    toSpin_->setEnabled(rangeRadio_->isChecked());
    // Keep custom page order intact. A document that lost a selected page shows
    // the existing validation error instead of silently printing different pages.
    refreshPreview();
    nativeSettings_.layout = printer_->pageLayout();
    nativeSettings_.layout.setPageSize(paper);
    nativeSettings_.layout.setOrientation(saved.orientation);
    nativeSettings_.copies = copiesSpin_->value();
    nativeSettings_.colour = printSettings().colourMode == ColourMode::Colour
        ? QPrinter::Color : QPrinter::GrayScale;
    nativeSettings_.duplex = nativeDuplex(saved.duplex);
    nativeSettings_.range = saved.pageSelection == PageSelection::Range ? QPrinter::PageRange
        : saved.pageSelection == PageSelection::Current ? QPrinter::CurrentPage : QPrinter::AllPages;
    nativeSettings_.from = rangeRadio_->isChecked() ? fromSpin_->value() : 1;
    nativeSettings_.to = rangeRadio_->isChecked() ? toSpin_->value() : qMax(1, pageCount_);
    printerSelectionChanged_ = false;
}

// Keeps repeat-only controls and scale wording consistent without changing saved values.
void PrintDialog::updatePageLayoutControls()
{
    const bool repeat = pageLayoutCombo_->currentData().toInt() == static_cast<int>(PageLayoutMode::Repeat);
    const bool expanding = repeat && repeatOptions_->isHidden();
    repeatOptions_->setVisible(repeat);
    //: %1 is rows times columns, for example 8. Each source page is repeated this many
    //: times on its own sheet. The Copies control separately repeats the whole job.
    repeatCountLabel_->setText(tr("Copies per sheet: %1")
                                  .arg(QLocale().toString(rowsSpin_->value() * columnsSpin_->value())));
    //: Scale option. Fit each source page inside one cell of the repeat grid.
    const QString fitCell = tr("Fit to cell");
    scaleCombo_->setItemText(scaleCombo_->findData(static_cast<int>(ScaleMode::FitToPage)),
                             repeat ? fitCell : tr("Fit to page"));
    // The settings column has a fixed width. Reserve the updated caption before
    // measuring it, including when a longer translation replaces Fit to page.
    scaleCombo_->setMinimumWidth(scaleCombo_->sizeHint().width());
    //: Preview-only dashed boundaries show grid cells and the printable area.
    const QString guides = tr("Dashed guides are not printed");
    marginNote_->setText(repeat ? guides : tr("Dashed line shows the printable area"));
    settingsScroll_->widget()->layout()->activate();
    const int minimum = settingsScroll_->widget()->minimumSizeHint().width()
        + settingsScroll_->verticalScrollBar()->sizeHint().width();
    settingsScroll_->setFixedWidth(qMax(settingsScroll_->width(), minimum));
    // Only adding controls requests more height. Row-count edits and ordinary
    // preview updates must preserve a user's manually reduced window size.
    if (expanding && isVisible())
        QTimer::singleShot(0, this, &PrintDialog::growToFitSettings);
}

QList<int> PrintDialog::resolvePages(QString *error) const
{
    error->clear();
    if (pageCount_ < 1) {
        *error = tr("This document has no pages to print.");
        return {};
    }
    if (customRadio_->isChecked())
        return PageRange::parse(customEdit_->text(), pageCount_, error);
    if (currentPageRadio_->isChecked())
        return {currentPage_};
    const int first = rangeRadio_->isChecked() ? fromSpin_->value() : 1;
    const int last = rangeRadio_->isChecked() ? toSpin_->value() : pageCount_;
    QList<int> pages;
    for (int page = first; page <= last; ++page)
        pages.append(page);
    return pages;
}

void PrintDialog::schedulePreview()
{
    // Validate page edits immediately so the previous selection cannot stay printable.
    QString error;
    const QList<int> pages = resolvePages(&error);
    if (pages != pages_) {
        pages_ = pages;
        previewIndex_ = 0;
    }
    updateNavigation();
    printButton_->setEnabled(false);
    if (pages_.isEmpty()) {
        previewTimer_->stop();
        preview_->clear(error);
        if (printSettings().scaleMode == ScaleMode::FitToPage)
            scalePercentSpin_->clear();
        return;
    }
    previewTimer_->start();
}

// Query only the selected device and cache even an unavailable default for this dialog.
QPageSize PrintDialog::selectedPrinterDefaultPageSize()
{
    QVariant cached = printerCombo_->currentData(kDefaultPaperSizeRole);
    if (!cached.isValid()) {
        const QString name = printerCombo_->currentData().toString();
        const QPageSize size = name.isEmpty() ? QPageSize()
            : QPrinterInfo::printerInfo(name).defaultPageSize();
        cached = QVariant::fromValue(size);
        printerCombo_->setItemData(printerCombo_->currentIndex(), cached, kDefaultPaperSizeRole);
    }
    return cached.value<QPageSize>();
}

// Reuse equivalent paper keys while retaining the selected driver's exact definition.
void PrintDialog::selectPaperSize(const QPageSize &size)
{
    if (!size.isValid())
        return;
    for (int index = 0; index < paperSizeCombo_->count(); ++index) {
        const QPageSize existing = paperSizeCombo_->itemData(index).value<QPageSize>();
        if (existing.key() == size.key() && existing.isEquivalentTo(size)) {
            paperSizeCombo_->setItemData(index, QVariant::fromValue(size));
            paperSizeCombo_->setItemText(index, size.name());
            paperSizeCombo_->setCurrentIndex(index);
            return;
        }
    }
    paperSizeCombo_->insertItem(0, size.name(), QVariant::fromValue(size));
    paperSizeCombo_->setCurrentIndex(0);
}

// Queries only selected native devices and keeps even absent metadata for this dialog.
const PrintDialog::PaperCapabilities &PrintDialog::selectedPaperCapabilities()
{
    const QString name = printerCombo_->currentData().toString();
    auto found = paperCapabilities_.constFind(name);
    if (found == paperCapabilities_.cend()) {
        PaperCapabilities capabilities;
        if (paperQuery_) {
            capabilities = paperQuery_(name);
        } else if (!name.isEmpty()) {
            const QPrinterInfo info = QPrinterInfo::printerInfo(name);
            capabilities = {info.supportedPageSizes(), info.supportsCustomPageSizes(),
                            info.minimumPhysicalPageSize(), info.maximumPhysicalPageSize()};
        }
        found = paperCapabilities_.insert(name, std::move(capabilities));
    }
    return found.value();
}

// Offers native driver media or the common PDF choices, retaining an explicit choice
// for validation instead of silently substituting a different sheet.
void PrintDialog::refreshPaperSizes(const QPageSize &preferred)
{
    if (printToFile_->isChecked()) {
        // Keep aliases reused by printer-default switching while PDF stays unrestricted.
        for (QPageSize::PageSizeId id : kCommonSizes) {
            if (paperSizeCombo_->findData(QVariant::fromValue(QPageSize(id))) < 0)
                selectPaperSize(QPageSize(id));
        }
        selectPaperSize(preferred);
        return;
    }
    const QSignalBlocker blocked(paperSizeCombo_);
    const PaperCapabilities &capabilities = selectedPaperCapabilities();
    paperSizeCombo_->clear();
    if (capabilities.supported.isEmpty() && !capabilities.custom) {
        for (QPageSize::PageSizeId id : kCommonSizes)
            paperSizeCombo_->addItem(QPageSize(id).name(), QVariant::fromValue(QPageSize(id)));
    } else {
        for (const QPageSize &paper : capabilities.supported)
            selectPaperSize(paper);
        if (capabilities.custom) {
            for (QPageSize::PageSizeId id : kCommonSizes) {
                const QPageSize paper = capabilities.acceptedPaper(QPageSize(id));
                if (paper.isValid())
                    selectPaperSize(paper);
            }
        }
    }
    const QPageSize accepted = capabilities.acceptedPaper(preferred);
    selectPaperSize(accepted.isValid() ? accepted : preferred);
}

bool PrintDialog::applyPrinterSettings(QString *error)
{
    // Device changes reset driver properties, so apply every explicit option afterwards.
    const bool toFile = printToFile_->isChecked();
    const QPrinter::OutputFormat format = toFile ? QPrinter::PdfFormat : QPrinter::NativeFormat;
    if (printer_->outputFormat() != format) {
        printer_->setOutputFileName(QString());
        printer_->setOutputFormat(format);
    }
    if (!toFile) {
        const QString name = printerCombo_->currentData().toString();
        if (name.isEmpty()) {
            *error = tr("Choose a printer or print to a PDF file.");
            return false;
        }
        if (printer_->printerName() != name)
            printer_->setPrinterName(name);
    }
    printer_->setFullPage(false);
    QPageSize paper = paperSizeCombo_->currentData().value<QPageSize>();
    if (!toFile)
        paper = selectedPaperCapabilities().acceptedPaper(paper);
    if (!paper.isValid() || !printer_->setPageSize(paper)) {
        *error = tr("This printer does not support the selected paper size.");
        return false;
    }
    printer_->setPageOrientation(landscapeRadio_->isChecked() ? QPageLayout::Landscape
                                                             : QPageLayout::Portrait);

    QPageLayout pageLayout = printer_->pageLayout();
    pageLayout.setUnits(QPageLayout::Point);
    const QMarginsF minimum = pageLayout.minimumMargins();
    const QMarginsF maximum = pageLayout.maximumMargins();
    const QMarginsF margins(qBound(minimum.left(), preferredMargins_.left(), maximum.left()),
                            qBound(minimum.top(), preferredMargins_.top(), maximum.top()),
                            qBound(minimum.right(), preferredMargins_.right(), maximum.right()),
                            qBound(minimum.bottom(), preferredMargins_.bottom(), maximum.bottom()));
    if (!printer_->setPageMargins(margins, QPageLayout::Point)) {
        *error = tr("The selected paper size cannot use these printer margins.");
        return false;
    }
    printer_->setCopyCount(copiesSpin_->value());
    printer_->setColorMode(printSettings().colourMode == ColourMode::Colour ? QPrinter::Color
                                                                          : QPrinter::GrayScale);
    printer_->setDuplex(!toFile && twoSidedCheck_->isChecked() ? nativeDuplex(duplexMode_)
                                                             : QPrinter::DuplexNone);
    return true;
}

bool PrintDialog::refreshPreview()
{
    previewTimer_->stop();
    QString error;
    const QList<int> pages = resolvePages(&error);
    if (pages != pages_) {
        pages_ = pages;
        previewIndex_ = 0;
    }
    updateNavigation();
    if (pages_.isEmpty() || !applyPrinterSettings(&error)) {
        preview_->clear(error);
        if (printSettings().scaleMode == ScaleMode::FitToPage)
            scalePercentSpin_->clear();
        printButton_->setEnabled(false);
        return false;
    }

    const QPageLayout pageLayout = printer_->pageLayout();
    const QSizeF size = pageLayout.fullRect(QPageLayout::Millimeter).size();
    const QLocale locale;
    const auto dimension = [&locale](double value) {
        return locale.toString(value, 'f', qAbs(value - qRound(value)) < 0.05 ? 0 : 1);
    };
    // "A4 · Portrait · 210 × 297 mm". Only the paper name and orientation are
    // words; the unit symbol and the separators are not translated.
    const QString paperSize =
        QStringLiteral("%1 × %2 mm").arg(dimension(size.width()), dimension(size.height()));
    paperLabel_->setText(QStringList{pageLayout.pageSize().name(),
                                     pageLayout.orientation() == QPageLayout::Landscape
                                         ? tr("Landscape")
                                         : tr("Portrait"),
                                     paperSize}
                             .join(QStringLiteral(" · ")));
    auto previewSettings = printSettings();
    if (printer_->resolution() > 0)
        previewSettings.qualityDpi = qMin(previewSettings.qualityDpi, printer_->resolution());
    preview_->setPage(pages_.at(previewIndex_) - 1, pageLayout, previewSettings);
    if (previewSettings.scaleMode != ScaleMode::Custom) {
        const QSignalBlocker blocker(scalePercentSpin_);
        const double percent = preview_->scale() * 100;
        if (percent > 0) {
            // Automatic placement is not limited by the custom input's 10–400% range.
            const int rounded = qRound(qMin(percent, double(std::numeric_limits<int>::max())));
            scalePercentSpin_->setRange(0, qMax(400, rounded));
            // A nonzero fit must not appear as 0%, even for unusually large pages.
            scalePercentSpin_->setSpecialValueText(
                rounded == 0 ? locale.toString(percent, 'g', 3) + QStringLiteral(" %") : QString());
            scalePercentSpin_->setValue(rounded);
        } else {
            scalePercentSpin_->clear();
        }
    }
    printButton_->setEnabled(true);
    return true;
}

void PrintDialog::updateNavigation()
{
    previewIndex_ = qBound(0, previewIndex_, qMax(0, static_cast<int>(pages_.size()) - 1));
    previousButton_->setEnabled(!pages_.isEmpty() && previewIndex_ > 0);
    nextButton_->setEnabled(!pages_.isEmpty() && previewIndex_ + 1 < pages_.size());
    if (pages_.isEmpty()) {
        pageLabel_->setText(tr("No pages selected"));
    } else if (allPagesRadio_->isChecked()) {
        //: Under the preview: %1 is the page shown, %2 the document's page count.
        pageLabel_->setText(tr("Page %1 of %2").arg(pages_.at(previewIndex_)).arg(pageCount_));
    } else {
        //: Under the preview: %1 is the page shown, which is number %2 of the
        //: %3 pages selected for printing.
        pageLabel_->setText(tr("Page %1 · %2 of %3 selected")
                                .arg(pages_.at(previewIndex_)).arg(previewIndex_ + 1)
                                .arg(pages_.size()));
    }
}

void PrintDialog::accept()
{
    // Flush pending edits before opening the file picker or handing off to the printer.
    if (!refreshPreview()) {
        if (customRadio_->isChecked())
            customEdit_->setFocus();
        return;
    }
    if (printToFile_->isChecked()) {
        QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (directory.isEmpty())
            directory = QDir::homePath();
        //: Default file name, without ".pdf", when the document has none.
        const QString base = suggestedFileName_.isEmpty() ? tr("document") : suggestedFileName_;
        const QString suggested = QDir(directory).filePath(base + QStringLiteral(".pdf"));
        const QString path = QFileDialog::getSaveFileName(this, tr("Print to file"), suggested,
                                                         //: File type filter. Keep "(*.pdf)".
                                                         tr("PDF files (*.pdf)"));
        if (path.isEmpty())
            return;
        printer_->setOutputFileName(path);
    }
    QDialog::accept();
}
