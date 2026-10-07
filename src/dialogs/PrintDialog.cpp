#include "dialogs/PrintDialog.h"

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
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

using mervin::printing::Alignment;
using mervin::printing::ColourMode;
using mervin::printing::ScaleMode;

namespace {
// The settings column fits English at this width. A longer translation widens it.
constexpr int kSettingsWidth = 390;
} // namespace

PrintDialog::PrintDialog(QPrinter *printer, mervin::RenderEngine *engine,
                         mervin::Document *document, int rotation,
                         QPageLayout::Orientation initialOrientation, int currentPage,
                         const QString &suggestedFileName, QWidget *parent)
    : QDialog(parent)
    , printer_(printer)
    , pageCount_(document ? document->pageCount() : 0)
    , currentPage_(qBound(1, currentPage, qMax(1, pageCount_)))
    , suggestedFileName_(suggestedFileName)
    , preferredMargins_(printer->pageLayout().margins(QPageLayout::Point))
{
    //: Dialog title.
    setWindowTitle(tr("Print"));
    setObjectName(QStringLiteral("printDialog"));
    setMinimumSize(800, 480);
    const QPageLayout initialLayout = printer_->pageLayout();
    const int initialCopies = printer_->copyCount();
    const QPrinter::ColorMode initialColour = printer_->colorMode();
    const QPrinter::DuplexMode initialDuplex = printer_->duplex();

    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    layout->addLayout(body, 1);

    // Keep settings reachable on short screens while the preview fits its available area.
    auto *settingsScroll = new QScrollArea(this);
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
    printerCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    printerCombo_->setMinimumContentsLength(14);
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
    static const QPageSize::PageSizeId kCommonSizes[] = {
        QPageSize::A3,     QPageSize::A4,     QPageSize::A5,    QPageSize::B4,
        QPageSize::B5,     QPageSize::Letter, QPageSize::Legal, QPageSize::Tabloid,
        QPageSize::Executive,
    };
    const QPageSize currentSize = printer_->pageLayout().pageSize();
    for (QPageSize::PageSizeId id : kCommonSizes)
        paperSizeCombo_->addItem(QPageSize(id).name(), QVariant::fromValue(QPageSize(id)));
    int paperIndex = -1;
    for (int index = 0; index < paperSizeCombo_->count(); ++index) {
        if (paperSizeCombo_->itemData(index).value<QPageSize>() == currentSize) {
            paperIndex = index;
            break;
        }
    }
    // Keep driver-provided custom paper dimensions instead of replacing them with A4.
    if (paperIndex < 0 && currentSize.isValid()) {
        paperSizeCombo_->insertItem(0, currentSize.name(), QVariant::fromValue(currentSize));
        paperIndex = 0;
    }
    paperSizeCombo_->setCurrentIndex(paperIndex < 0 ? 1 : paperIndex);
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

    auto *optionsBox = new QGroupBox(tr("Options"), settings);
    auto *optionsForm = new QFormLayout(optionsBox);
    copiesSpin_ = new QSpinBox(optionsBox);
    copiesSpin_->setObjectName(QStringLiteral("printCopies"));
    copiesSpin_->setRange(1, 999);
    copiesSpin_->setValue(qMax(1, printer_->copyCount()));
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
        scalePercentSpin_->setEnabled(scaleCombo_->currentData().toInt()
                                      == static_cast<int>(ScaleMode::Custom));
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
    navigation->addStretch();
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
    pageLabel_->setMinimumWidth(140);
    navigation->addWidget(pageLabel_);
    nextButton_ = new QPushButton(this);
    nextButton_->setObjectName(QStringLiteral("printNextPage"));
    nextButton_->setAccessibleName(tr("Next preview page"));
    nextButton_->setFixedSize(30, 30);
    nextButton_->setAutoDefault(false);
    mervin::icons::setButtonGlyph(nextButton_, mervin::icons::Glyph::NextPage, 16);
    navigation->addWidget(nextButton_);
    navigation->addStretch();
    previewColumn->addLayout(navigation);

    auto *previewFooter = new QHBoxLayout;
    paperLabel_ = new QLabel(this);
    paperLabel_->setObjectName(QStringLiteral("printPaperLabel"));
    paperLabel_->setWordWrap(true);
    previewFooter->addWidget(paperLabel_, 1);
    auto *marginNote = new QLabel(tr("Dashed line shows the printable area"), this);
    marginNote->setWordWrap(true);
    marginNote->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    previewFooter->addWidget(marginNote, 1);
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
    connect(systemButton, &QPushButton::clicked, this,
            [this, initialLayout, initialOrientation, initialCopies, initialColour, initialDuplex] {
        // Live preview changes the supplied printer. Restore defaults before native handoff.
        printer_->setOutputFileName(QString());
        printer_->setOutputFormat(QPrinter::NativeFormat);
        printer_->setPrinterName(printerCombo_->currentData().toString());
        printer_->setPageLayout(initialLayout);
        printer_->setPageOrientation(initialOrientation);
        printer_->setCopyCount(initialCopies);
        printer_->setColorMode(initialColour);
        printer_->setDuplex(initialDuplex);
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
    for (QComboBox *combo : {printerCombo_, paperSizeCombo_, colorCombo_, qualityCombo_,
                            scaleCombo_, alignmentCombo_})
        connect(combo, &QComboBox::currentIndexChanged, this, &PrintDialog::schedulePreview);
    for (QSpinBox *spin : {copiesSpin_, fromSpin_, toSpin_, scalePercentSpin_})
        connect(spin, &QSpinBox::valueChanged, this, &PrintDialog::schedulePreview);
    for (QRadioButton *radio : {portraitRadio_, landscapeRadio_, allPagesRadio_,
                               currentPageRadio_, rangeRadio_, customRadio_}) {
        connect(radio, &QRadioButton::toggled, this, [this](bool checked) {
            if (checked)
                schedulePreview();
        });
    }
    connect(customEdit_, &QLineEdit::textChanged, this, &PrintDialog::schedulePreview);
    connect(printToFile_, &QCheckBox::toggled, this, &PrintDialog::schedulePreview);
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
    refreshPreview();
}

mervin::printing::Settings PrintDialog::printSettings() const
{
    mervin::printing::Settings settings;
    settings.scaleMode = static_cast<ScaleMode>(scaleCombo_->currentData().toInt());
    settings.scalePercent = scalePercentSpin_->value();
    settings.alignment = static_cast<Alignment>(alignmentCombo_->currentData().toInt());
    settings.colourMode = static_cast<ColourMode>(colorCombo_->currentData().toInt());
    settings.qualityDpi = qualityCombo_->currentData().toInt();
    return settings;
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
        return;
    }
    previewTimer_->start();
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
    const QPageSize paper = paperSizeCombo_->currentData().value<QPageSize>();
    if (!printer_->setPageSize(paper)) {
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
    printer_->setDuplex(!toFile && twoSidedCheck_->isChecked() ? QPrinter::DuplexLongSide
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
