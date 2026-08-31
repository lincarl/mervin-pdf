#include "dialogs/PrintDialog.h"

#include "print/PageRange.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPageSize>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

PrintDialog::PrintDialog(QPrinter *printer, QPageLayout::Orientation initialOrientation,
                         int pageCount, int currentPage, const QString &suggestedFileName,
                         QWidget *parent)
    : QDialog(parent)
    , printer_(printer)
    , pageCount_(qMax(1, pageCount))
    , currentPage_(qBound(1, currentPage, qMax(1, pageCount)))
    , suggestedFileName_(suggestedFileName)
{
    setWindowTitle(tr("Print"));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);

    // ── Printer ───────────────────────────────────────────────────────────────
    auto *printerBox = new QGroupBox(tr("Printer"), this);
    auto *printerForm = new QFormLayout(printerBox);

    printerCombo_ = new QComboBox(printerBox);
    const QList<QPrinterInfo> printers = QPrinterInfo::availablePrinters();
    const QString currentName =
        printer_->printerName().isEmpty() ? QPrinterInfo::defaultPrinterName() : printer_->printerName();
    for (const QPrinterInfo &info : printers) {
        // description() is often identical to printerName() - only append it when
        // it adds something, so the entry isn't "Name (Name)".
        const QString label =
            (info.description().isEmpty() || info.description() == info.printerName())
                ? info.printerName()
                : tr("%1 (%2)").arg(info.printerName(), info.description());
        printerCombo_->addItem(label, info.printerName());
        if (info.printerName() == currentName)
            printerCombo_->setCurrentIndex(printerCombo_->count() - 1);
    }
    printerForm->addRow(tr("Name:"), printerCombo_);

    // Paper size: a curated list of common sizes. The printer's current size is
    // pinned to the top if it isn't already one of them, so nothing the driver
    // defaulted to is silently dropped.
    paperSizeCombo_ = new QComboBox(printerBox);
    static const QPageSize::PageSizeId kCommonSizes[] = {
        QPageSize::A3,     QPageSize::A4,     QPageSize::A5,        QPageSize::B4,
        QPageSize::B5,     QPageSize::Letter, QPageSize::Legal,     QPageSize::Tabloid,
        QPageSize::Executive,
    };
    const QPageSize::PageSizeId currentId = printer_->pageLayout().pageSize().id();
    bool currentListed = false;
    for (QPageSize::PageSizeId id : kCommonSizes) {
        paperSizeCombo_->addItem(QPageSize(id).name(), static_cast<int>(id));
        if (id == currentId)
            currentListed = true;
    }
    if (!currentListed && currentId != QPageSize::Custom)
        paperSizeCombo_->insertItem(0, QPageSize(currentId).name(), static_cast<int>(currentId));
    int paperIdx = paperSizeCombo_->findData(static_cast<int>(currentId));
    if (paperIdx < 0)
        paperIdx = paperSizeCombo_->findData(static_cast<int>(QPageSize::A4));
    paperSizeCombo_->setCurrentIndex(qMax(0, paperIdx));
    printerForm->addRow(tr("Paper size:"), paperSizeCombo_);

    printToFile_ = new QCheckBox(tr("Print to a PDF file instead"), printerBox);
    printerForm->addRow(QString(), printToFile_);
    // Choosing the PDF writer makes the physical-printer pick irrelevant.
    connect(printToFile_, &QCheckBox::toggled, printerCombo_, &QWidget::setDisabled);

    layout->addWidget(printerBox);

    // ── Orientation ─────────────────────────────────────────────────────────────
    // Parenting each radio pair to its own group box scopes auto-exclusivity, so
    // the orientation radios don't interfere with the page-range radios below.
    auto *orientationBox = new QGroupBox(tr("Orientation"), this);
    auto *orientationRow = new QHBoxLayout(orientationBox);
    portraitRadio_ = new QRadioButton(tr("Portrait"), orientationBox);
    landscapeRadio_ = new QRadioButton(tr("Landscape"), orientationBox);
    orientationRow->addWidget(portraitRadio_);
    orientationRow->addWidget(landscapeRadio_);
    orientationRow->addStretch();
    if (initialOrientation == QPageLayout::Landscape)
        landscapeRadio_->setChecked(true);
    else
        portraitRadio_->setChecked(true);
    layout->addWidget(orientationBox);

    // ── Pages ───────────────────────────────────────────────────────────────────
    auto *pagesBox = new QGroupBox(tr("Pages"), this);
    auto *pagesLayout = new QVBoxLayout(pagesBox);

    // Show the actual page numbers, not just the count: "All pages (page 1-6)".
    const QString allLabel = pageCount_ > 1 ? tr("All pages (page 1-%1)").arg(pageCount_)
                                            : tr("All pages (page 1)");
    allPagesRadio_ = new QRadioButton(allLabel, pagesBox);
    allPagesRadio_->setChecked(true);
    pagesLayout->addWidget(allPagesRadio_);

    currentPageRadio_ = new QRadioButton(tr("Current page (page %1)").arg(currentPage_), pagesBox);
    pagesLayout->addWidget(currentPageRadio_);

    auto *rangeRow = new QHBoxLayout;
    rangeRadio_ = new QRadioButton(tr("Pages from"), pagesBox);
    fromSpin_ = new QSpinBox(pagesBox);
    fromSpin_->setRange(1, pageCount_);
    fromSpin_->setValue(1);
    toSpin_ = new QSpinBox(pagesBox);
    toSpin_->setRange(1, pageCount_);
    toSpin_->setValue(pageCount_);
    rangeRow->addWidget(rangeRadio_);
    rangeRow->addWidget(fromSpin_);
    rangeRow->addWidget(new QLabel(tr("to"), pagesBox));
    rangeRow->addWidget(toSpin_);
    rangeRow->addStretch();
    pagesLayout->addLayout(rangeRow);

    // The range spin boxes only matter when "Pages from" is selected.
    fromSpin_->setEnabled(false);
    toSpin_->setEnabled(false);
    connect(rangeRadio_, &QRadioButton::toggled, fromSpin_, &QWidget::setEnabled);
    connect(rangeRadio_, &QRadioButton::toggled, toSpin_, &QWidget::setEnabled);
    // Keep from <= to as the user edits either end.
    connect(fromSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        if (v > toSpin_->value())
            toSpin_->setValue(v);
    });
    connect(toSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        if (v < fromSpin_->value())
            fromSpin_->setValue(v);
    });

    // Custom: a free-form spec like "1-3, 5, 8-10". The field stays enabled so the
    // user can click straight into it; typing selects this option automatically.
    auto *customRow = new QHBoxLayout;
    customRadio_ = new QRadioButton(tr("Custom"), pagesBox);
    customEdit_ = new QLineEdit(pagesBox);
    customEdit_->setPlaceholderText(tr("e.g. 1-3, 5, 8-10"));
    customRow->addWidget(customRadio_);
    customRow->addWidget(customEdit_, 1);
    pagesLayout->addLayout(customRow);
    connect(customEdit_, &QLineEdit::textEdited, this, [this](const QString &) {
        if (!customRadio_->isChecked())
            customRadio_->setChecked(true);
    });
    layout->addWidget(pagesBox);

    // ── Options ─────────────────────────────────────────────────────────────────
    auto *optionsBox = new QGroupBox(tr("Options"), this);
    auto *optionsForm = new QFormLayout(optionsBox);

    copiesSpin_ = new QSpinBox(optionsBox);
    copiesSpin_->setRange(1, 999);
    copiesSpin_->setValue(qMax(1, printer_->copyCount()));
    optionsForm->addRow(tr("Copies:"), copiesSpin_);

    colorCombo_ = new QComboBox(optionsBox);
    colorCombo_->addItem(tr("Colour"), static_cast<int>(QPrinter::Color));
    colorCombo_->addItem(tr("Grayscale"), static_cast<int>(QPrinter::GrayScale));
    colorCombo_->setCurrentIndex(printer_->colorMode() == QPrinter::GrayScale ? 1 : 0);
    optionsForm->addRow(tr("Colour:"), colorCombo_);

    qualityCombo_ = new QComboBox(optionsBox);
    qualityCombo_->addItem(tr("Draft (150 dpi)"), 150);
    qualityCombo_->addItem(tr("Normal (300 dpi)"), 300);
    qualityCombo_->addItem(tr("High (600 dpi)"), 600);
    qualityCombo_->setCurrentIndex(1); // Normal
    optionsForm->addRow(tr("Quality:"), qualityCombo_);

    auto *scaleRow = new QHBoxLayout;
    scaleCombo_ = new QComboBox(optionsBox);
    scaleCombo_->addItem(tr("Fit to page"), static_cast<int>(ScaleMode::FitToPage));
    scaleCombo_->addItem(tr("Actual size"), static_cast<int>(ScaleMode::ActualSize));
    scaleCombo_->addItem(tr("Custom"), static_cast<int>(ScaleMode::Custom));
    scalePercentSpin_ = new QSpinBox(optionsBox);
    scalePercentSpin_->setRange(10, 400);
    scalePercentSpin_->setValue(100);
    scalePercentSpin_->setSuffix(tr(" %"));
    scalePercentSpin_->setEnabled(false);
    scaleRow->addWidget(scaleCombo_);
    scaleRow->addWidget(scalePercentSpin_);
    scaleRow->addStretch();
    optionsForm->addRow(tr("Scale:"), scaleRow);
    // The percentage only applies to Custom.
    connect(scaleCombo_, &QComboBox::currentIndexChanged, this, [this] {
        scalePercentSpin_->setEnabled(scaleCombo_->currentData().toInt()
                                      == static_cast<int>(ScaleMode::Custom));
    });

    twoSidedCheck_ = new QCheckBox(tr("Print on both sides"), optionsBox);
    optionsForm->addRow(tr("Two-sided:"), twoSidedCheck_);

    layout->addWidget(optionsBox);

    // ── Buttons ─────────────────────────────────────────────────────────────────
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Print"));
    // Escape hatch to the OS print dialog for anything our dialog doesn't expose.
    auto *systemBtn = buttons->addButton(tr("Print using system dialogue…"),
                                         QDialogButtonBox::ActionRole);
    systemBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(systemBtn, &QPushButton::clicked, this, [this] {
        useSystemDialog_ = true;
        QDialog::accept(); // skip our validation/apply - the native dialog takes over
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &PrintDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PrintDialog::reject);
    layout->addWidget(buttons);
}

void PrintDialog::accept()
{
    // Resolve the page selection first: a bad custom range must keep the dialog
    // open before we mutate the printer.
    QList<int> pages;
    if (customRadio_->isChecked()) {
        QString err;
        pages = PageRange::parse(customEdit_->text(), pageCount_, &err);
        if (pages.isEmpty()) {
            QMessageBox::warning(this, tr("Print"), err);
            customEdit_->setFocus();
            customEdit_->selectAll();
            return;
        }
    } else if (rangeRadio_->isChecked()) {
        for (int p = fromSpin_->value(); p <= toSpin_->value(); ++p)
            pages << p;
    } else if (currentPageRadio_->isChecked()) {
        pages << currentPage_;
    } else {
        for (int p = 1; p <= pageCount_; ++p)
            pages << p;
    }
    pages_ = pages;

    // Target device first: changing the printer can reset other properties, so
    // everything else is applied afterwards.
    if (printToFile_->isChecked()) {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (dir.isEmpty())
            dir = QDir::homePath();
        QString base = suggestedFileName_;
        if (base.isEmpty())
            base = tr("document");
        const QString suggested = QDir(dir).filePath(base + QStringLiteral(".pdf"));
        const QString path =
            QFileDialog::getSaveFileName(this, tr("Print to file"), suggested,
                                         tr("PDF files (*.pdf)"));
        if (path.isEmpty())
            return; // user cancelled the save picker - stay open
        printer_->setOutputFormat(QPrinter::PdfFormat);
        printer_->setOutputFileName(path);
    } else {
        printer_->setOutputFormat(QPrinter::NativeFormat);
        printer_->setOutputFileName(QString());
        const QString name = printerCombo_->currentData().toString();
        if (!name.isEmpty())
            printer_->setPrinterName(name);
    }

    printer_->setPageSize(
        QPageSize(static_cast<QPageSize::PageSizeId>(paperSizeCombo_->currentData().toInt())));
    printer_->setPageOrientation(landscapeRadio_->isChecked() ? QPageLayout::Landscape
                                                              : QPageLayout::Portrait);
    printer_->setCopyCount(copiesSpin_->value());
    printer_->setColorMode(static_cast<QPrinter::ColorMode>(colorCombo_->currentData().toInt()));
    printer_->setDuplex(twoSidedCheck_->isChecked() ? QPrinter::DuplexLongSide
                                                     : QPrinter::DuplexNone);

    scaleMode_ = static_cast<ScaleMode>(scaleCombo_->currentData().toInt());
    scalePercent_ = scalePercentSpin_->value();
    qualityDpi_ = qualityCombo_->currentData().toInt();

    QDialog::accept();
}
