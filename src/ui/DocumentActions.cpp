#include "ui/MainWindow.h"
#include "app/WindowManager.h"
#include "dialogs/ExportMeasureDialog.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/PrintDialog.h"
#include "dialogs/SecurityDialog.h"
#include "print/PageRange.h"
#include "print/PrintLayout.h"
#include "render/Document.h"
#include "render/MeasureContent.h"
#include "render/MeasureMath.h"
#include "render/RenderEngine.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QSet>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <unordered_map>

using mervin::TabPage;
using mervin::ViewerWidget;
using mervin::Document;
using mervin::Measurement;
using mervin::MeasureModel;
using mervin::MeasureScale;
using mervin::MeasureExport;
using mervin::ExportMeasureDialog;

namespace {

using mervin::PageOps;

// Retry once with a prompted password and remember it only on success.
bool runWriteOp(QWidget *parent, TabPage *tab,
                const std::function<PageOps::Status(const QString &, QString *)> &op)
{
    QString err;
    PageOps::Status st = op(tab->password(), &err);
    if (st == PageOps::Status::NeedsPassword) {
        bool ok = false;
        const QString pw = QInputDialog::getText(
            parent, QObject::tr("Password Required"),
            QObject::tr("Enter the document password:"), QLineEdit::Password, QString(), &ok);
        if (ok) {
            st = op(pw, &err);
            if (st == PageOps::Status::Ok)
                tab->setPassword(pw);
        }
    }
    if (st != PageOps::Status::Ok) {
        QMessageBox::warning(parent, QObject::tr("Operation failed"),
                             st == PageOps::Status::NeedsPassword
                                 ? QObject::tr("A password is required.")
                                 : QObject::tr("The operation failed.\n\n%1").arg(err));
        return false;
    }
    return true;
}


} // namespace

QList<int> MainWindow::askPageRange(const QString &title, int pageCount)
{
    bool ok = false;
    const QString spec = QInputDialog::getText(
        this, title,
        tr("Pages (e.g. \"all\", \"1-%1\", \"1,3,5-9\"):").arg(pageCount),
        QLineEdit::Normal, QStringLiteral("all"), &ok);
    if (!ok)
        return {};
    QString error;
    QList<int> pages;
    QSet<int> seen;
    for (int page : PageRange::parseAllowingAll(spec, pageCount, &error))
        if (!seen.contains(page)) {
            seen.insert(page);
            pages.append(page - 1);
        }
    if (pages.isEmpty())
        QMessageBox::warning(this, title, error);
    return pages;
}

void MainWindow::offerToOpen(const QString &path, const QString &password)
{
    const auto open = QMessageBox::information(
        this, tr("Done"), tr("Saved to:\n%1\n\nOpen it now?").arg(QDir::toNativeSeparators(path)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (open == QMessageBox::Yes)
        openFile(path, false, -1, true, password);
}

void MainWindow::addSectionHeader(QMenu *menu, const QString &text)
{
    // QWidgetAction allows theme styling that native menu section labels lack.
    auto *label = new QLabel(menu);
    label->setObjectName(QStringLiteral("menuSectionHeader"));
    // No '&' escaping: a QLabel without a buddy shows ampersands literally
    // (doubling them rendered "Select && Annotate").
    label->setText(text);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() * 0.82);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::PercentageSpacing, 102);
    label->setFont(f);
    // Align with icons; QWidgetAction does not inherit QMenu::item padding.
    label->setContentsMargins(2, 7, 10, 3);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    auto *wa = new QWidgetAction(menu);
    wa->setDefaultWidget(label);
    wa->setEnabled(false); // never selectable or hoverable
    menu->addAction(wa);
}

void MainWindow::createDocumentMenu()
{
    // applyActionIcons() refreshes the menu icons when the theme changes.
    documentMenu_ = new QMenu(documentButton_);
    documentMenu_->addAction(tr("&Rotate Pages"), this, &MainWindow::rotatePagesOp);
    documentMenu_->addAction(tr("&Delete Pages"), this, &MainWindow::deletePagesOp);
    documentMenu_->addAction(tr("&Extract Pages"), this, &MainWindow::extractPagesOp);
    documentMenu_->addAction(tr("Split All Pages into One File Each"), this,
                             &MainWindow::splitDocument);
    documentMenu_->addAction(tr("&Merge PDFs"), this, &MainWindow::mergeDocuments);
    documentMenu_->addSeparator();
    documentMenu_->addAction(tr("&Security"), this, &MainWindow::openSecurity);
    // Save / Save as copy / Export with measurements live on the toolbar's
    // dropdown-only Save button (see createToolBar), not in this menu.

    documentButton_->setMenu(documentMenu_);
}

void MainWindow::openSecurity()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    if (!t)
        return;
    mervin::SecurityDialog dlg(t->path(), t->password(), this);
    connect(&dlg, &mervin::SecurityDialog::openRequested, this,
            [this](const QString &p) { openFile(p); });
    dlg.exec();
}

void MainWindow::saveAsCopy()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    const QFileInfo fi(t->path());
    const QString suggested = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName()
                              + QStringLiteral("-copy.pdf");
    const QString out = QFileDialog::getSaveFileName(this, tr("Save as Copy"), suggested,
                                                     tr("PDF documents (*.pdf)"));
    if (out.isEmpty())
        return;
    if (QFileInfo(out) == fi) {
        QMessageBox::warning(this, tr("Save as Copy"),
                             tr("Choose a different file name - use Save edits to write back to "
                                "the original."));
        return;
    }

    if (wm_ && wm_->isOpenAnywhere(QFileInfo(out).canonicalFilePath())) {
        QMessageBox::warning(this, tr("Save as Copy"), tr("The destination is open in a tab."));
        return;
    }
    v->commitActiveFormEditor();
    v->commitActiveAnnotEditor();
    QTemporaryDir stage;
    QString error;
    const QString snapshot = stage.filePath(QStringLiteral("snapshot.pdf"));
    if (!stage.isValid() || !v->document()
        || !mervin::DocumentOutput::snapshot(*v->document(), collectMeasureDoc(v),
                                             snapshot, t->password(), &error)
        || !mervin::DocumentOutput::replace(snapshot, out, &error)) {
        QMessageBox::warning(this, tr("Save as Copy"), tr("Could not save:\n%1").arg(error));
        return;
    }
    offerToOpen(out, t->password()); // the copy keeps the source's encryption
}

mervin::MeasureDoc MainWindow::collectMeasureDoc(ViewerWidget *v) const
{
    return v ? v->measurementDocument() : mervin::MeasureDoc{};
}

std::vector<mervin::RenderMeasurement> MainWindow::collectRenderMeasurements(ViewerWidget *v) const
{
    std::vector<mervin::RenderMeasurement> out;
    if (!v || !v->document())
        return out;
    Document *doc = v->document();
    const MeasureModel &ov = v->measureOverrides();
    std::unordered_map<int, std::array<double, 6>> mats;
    for (const Measurement &m : v->committedMeasurements()) {
        if (m.pts.size() < 2)
            continue;
        auto it = mats.find(m.page);
        if (it == mats.end())
            it = mats.emplace(m.page, doc->pagePointToPdfMatrix(m.page)).first;
        const mervin::PageMeasurement pm = doc->pageMeasurement(m.page);
        const MeasureScale sc = mervin::measure::resolveScale(pm, m.pts.front(), ov.override(m.page));

        mervin::RenderMeasurement rm;
        rm.page = m.page;
        rm.kind = m.kind;
        rm.pts = m.pts;
        rm.hasLabelPos = m.hasLabelPos;
        rm.labelPos = m.labelPos;
        rm.label = mervin::formatMeasurementValue(m.kind, m.pts, sc, v->measureUnit(),
                                                  v->measurePrecision());
        rm.lineWidth = v->measureLineWidth();
        const std::array<double, 6> &a = it->second;
        rm.toPdf = mervin::Mat6{a[0], a[1], a[2], a[3], a[4], a[5]};
        out.push_back(std::move(rm));
    }
    return out;
}

void MainWindow::saveMeasurements()
{
    saveTab(currentTab());
}

bool MainWindow::saveTab(TabPage *t)
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    if (!t)
        return false;
    QString openError;
    bool needsPassword = false;
    if (!t->isLoaded() && !t->resume(&openError, &needsPassword)) {
        while (needsPassword) {
            bool accepted = false;
            const QString password = QInputDialog::getText(
                this, tr("Password required"), tr("Enter the password for %1.").arg(t->tabTitle()),
                QLineEdit::Password, {}, &accepted);
            if (!accepted)
                return false;
            t->setPassword(password);
            if (t->resume(&openError, &needsPassword))
                break;
        }
        if (!t->isLoaded()) {
            QMessageBox::warning(this, tr("Save"), openError);
            return false;
        }
    }
    ViewerWidget *v = t->viewer();
    v->commitActiveFormEditor();
    v->commitActiveAnnotEditor();
    if (!t->hasUnsavedEdits())
        return true;
    if (wm_ && wm_->openTabPaths().count(t->canonicalPath()) > 1) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Close the other view of this file before saving, or save a copy."));
        return false;
    }

    if (t->sourceChangedOnDisk()
        && QMessageBox::question(this, tr("File changed"),
            tr("%1 changed on disk since it was opened. Replace it with your edited copy?")
                .arg(t->tabTitle()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return false;

    QString snapshot;
    {
        QTemporaryFile stage(QFileInfo(t->path()).absolutePath()
                             + QStringLiteral("/.mervin-save-XXXXXX.pdf"));
        if (!stage.open()) {
            QMessageBox::warning(this, tr("Save"), stage.errorString());
            return false;
        }
        snapshot = stage.fileName();
        stage.setAutoRemove(false);
        // Destroy the handle before qpdf opens it; close() keeps it open internally.
    }
    bool retainSnapshot = false;
    const auto cleanup = qScopeGuard([&] {
        if (!retainSnapshot)
            QFile::remove(snapshot);
    });
    QString error;
    if (!mervin::DocumentOutput::snapshot(*v->document(), collectMeasureDoc(v),
                                          snapshot, t->password(), &error)) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save:\n%1").arg(error));
        return false;
    }

    const auto state = captureViewState(v);
    const bool formMode = v->formMode();
    const QString path = t->path();
    clearDocumentSidebars();
    t->detachDocument();
    const bool saved = mervin::DocumentOutput::replace(snapshot, path, &error);
    openError.clear();
    if (!saved || !t->open(path, t->password(), &openError)) {
        // Keep all edits editable and retain the staged file if recovery also fails.
        retainSnapshot = true;
        if (!t->recoverSnapshot(snapshot, &openError))
            error += tr("\nEdited document retained at %1.\n%2").arg(snapshot, openError);
        else if (saved)
            error = tr("The file was saved, but could not be reopened. The edited copy remains open.");
        applyViewStateToViewer(v, state);
        updateForCurrentTab();
        QMessageBox::warning(this, tr("Save"), error);
        return false;
    }
    applyViewStateToViewer(v, state);
    if (formMode && v->hasFormFields())
        v->setFormMode(true);
    updateForCurrentTab();
    statusInfo_->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
    return true;
}

bool MainWindow::confirmClose(TabPage *tab)
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    auto *viewer = tab->viewer();
    viewer->commitActiveFormEditor();
    viewer->commitActiveAnnotEditor();
    if (!tab->hasUnsavedEdits())
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Unsaved changes"), tr("Save changes to %1?").arg(tab->tabTitle()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Discard
        || (answer == QMessageBox::Save && saveTab(tab));
}

void MainWindow::exportMeasuredCopy()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    if (!v->hasMeasurements()) {
        QMessageBox::information(this, tr("Export with Measurements"),
                                 tr("There are no measurements to export. Add measurements first."));
        return;
    }
    ExportMeasureDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QFileInfo fi(t->path());
    const QString suggested = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName()
                              + QStringLiteral("-measured.pdf");
    const QString out = QFileDialog::getSaveFileName(this, tr("Export with Measurements"), suggested,
                                                     tr("PDF documents (*.pdf)"));
    if (out.isEmpty())
        return;

    if (wm_ && wm_->isOpenAnywhere(QFileInfo(out).canonicalFilePath())) {
        QMessageBox::warning(this, tr("Export"), tr("The destination is open in a tab."));
        return;
    }
    v->commitActiveFormEditor();
    v->commitActiveAnnotEditor();
    QTemporaryDir stage;
    const QString live = stage.filePath(QStringLiteral("live.pdf"));
    const QString flat = stage.filePath(QStringLiteral("flat.pdf"));
    QString error;
    if (!stage.isValid()
        || !mervin::DocumentOutput::snapshot(*v->document(), collectMeasureDoc(v),
                                             live, t->password(), &error)
        || MeasureExport::flatten(live, flat, collectRenderMeasurements(v), t->password(), &error)
               != MeasureExport::Status::Ok
        || !mervin::DocumentOutput::replace(flat, out, &error)) {
        QMessageBox::warning(this, tr("Export"), tr("Could not export:\n%1").arg(error));
        return;
    }
    offerToOpen(out, t->password());
}

void MainWindow::rotatePagesOp()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    const QList<int> pages = askPageRange(tr("Rotate Pages"), v->pageCount());
    if (pages.isEmpty())
        return;
    const QStringList angles{tr("90° clockwise"), tr("180°"), tr("90° counter-clockwise")};
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Rotate Pages"), tr("Rotation:"), angles,
                                                 0, false, &ok);
    if (!ok)
        return;
    const int angle = choice == angles[1] ? 180 : (choice == angles[2] ? 270 : 90);

    const QFileInfo fi(t->path());
    const QString out = QFileDialog::getSaveFileName(
        this, tr("Save Rotated Copy"),
        fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("-rotated.pdf"),
        tr("PDF documents (*.pdf)"));
    if (out.isEmpty())
        return;
    const QString in = t->path();
    if (runWriteOp(this, t, [&](const QString &pw, QString *err) {
            return PageOps::rotatePages(in, out, pages, angle, true, pw, err);
        }))
        offerToOpen(out, t->password()); // output keeps the source's encryption
}

void MainWindow::deletePagesOp()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    const QList<int> pages = askPageRange(tr("Delete Pages"), v->pageCount());
    if (pages.isEmpty())
        return;
    if (pages.size() >= v->pageCount()) {
        QMessageBox::warning(this, tr("Delete Pages"), tr("Cannot delete every page."));
        return;
    }
    const QFileInfo fi(t->path());
    const QString out = QFileDialog::getSaveFileName(
        this, tr("Save Edited Copy"),
        fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("-edited.pdf"),
        tr("PDF documents (*.pdf)"));
    if (out.isEmpty())
        return;
    const QString in = t->path();
    if (runWriteOp(this, t, [&](const QString &pw, QString *err) {
            return PageOps::deletePages(in, out, pages, pw, err);
        }))
        offerToOpen(out, t->password()); // output keeps the source's encryption
}

void MainWindow::extractPagesOp()
{
    if (ViewerWidget *v = currentViewer())
        extractPages(v->currentPage());
}

void MainWindow::extractPages(int seedPage)
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    // Bake an edit still being typed into the live document so the unsaved-changes
    // note sees it. Nothing is written here: Extract copies from the file on disk.
    v->commitActiveFormEditor();
    v->commitActiveAnnotEditor();

    mervin::ExtractDialog::Source src;
    src.path = t->path();
    src.password = t->password();
    src.viewerPageCount = v->pageCount();
    src.currentPage = seedPage;
    src.hasUnsavedEdits = v->hasFormEdits() || v->hasAnnotEdits();
    src.openWhenDone = settings_.extractOpenWhenDone;
    src.doc = v->document();
    src.engine = engine_;
    src.openPaths = wm_ ? wm_->openTabPaths() : tabPaths();

    mervin::ExtractDialog dlg(src, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    if (!dlg.password().isEmpty())
        t->setPassword(dlg.password()); // verified by the dialog; typed there or the tab's own
    if (dlg.openWhenDone() != settings_.extractOpenWhenDone) {
        settings_.extractOpenWhenDone = dlg.openWhenDone();
        settings_.save();
    }

    // The dialog has written the file: it writes before closing, so a failed
    // write stays in the dialog with the rows. No completion modal: the status
    // bar says what was written, and Open when done decides what happens next.
    const mervin::ExtractPlan::Job job = dlg.job();
    statusBar()->showMessage(mervin::ExtractPlan::doneText(job), 5000);
    if (dlg.openWhenDone())
        openFile(job.path);
}

void MainWindow::splitDocument()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    if (!t)
        return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose Output Folder"),
                                                          QFileInfo(t->path()).absolutePath());
    if (dir.isEmpty())
        return;
    const QString base = QFileInfo(t->path()).completeBaseName();
    const QString in = t->path();
    QStringList written;
    if (runWriteOp(this, t, [&](const QString &pw, QString *err) {
            return PageOps::split(in, dir, base, pw, &written, err);
        })) {
        // Not tr("%n file(s)", ..., n): with no translator loaded Qt substitutes
        // the number but leaves the "(s)", so this used to read "Wrote 3 file(s)".
        const QString what = written.size() == 1 ? tr("1 file") : tr("%1 files").arg(written.size());
        QMessageBox::information(this, tr("Split Pages"),
                                 tr("Wrote %1 to:\n%2")
                                     .arg(what, QDir::toNativeSeparators(dir)));
    }
}

void MainWindow::mergeDocuments()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    // The open document seeds the list as an ordinary first row. The viewer's
    // page count goes along only as a fallback - the dialog probes the file with
    // qpdf itself, because MuPDF opens documents qpdf will not (see MergeDialog).
    // The tab's password goes too, so an encrypted open document is not Locked.
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    mervin::MergeDialog dlg(t ? t->path() : QString(), v ? v->pageCount() : 0,
                            t ? t->password() : QString(), this);
    dlg.setOpenFiles(wm_ ? wm_->openTabPaths() : tabPaths());
    if (dlg.exec() != QDialog::Accepted)
        return;
    // The dialog has written the file: it writes before closing, so a failure
    // (naming the input that stopped it) stays in the dialog with the plan.
    offerToOpen(dlg.outputPath());
}

void MainWindow::printDocument()
{
    QScopedValueRollback<int> operation(documentOperationDepth_, documentOperationDepth_ + 1);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v || !v->document())
        return;
    v->commitActiveFormEditor();  // bake the field still being typed into the live doc
    v->commitActiveAnnotEditor(); // bake a comment still being typed into the live doc
    const int pageCount = v->pageCount();
    if (pageCount <= 0)
        return;

    // Preview and output share the live document, including form/annotation
    // edits, or the same flattened snapshot when measurements are present.
    QTemporaryDir measureTmp;
    std::unique_ptr<Document> flatDoc;
    Document *printDoc = v->document();
    if (v->hasMeasurements()) {
        const QString live = measureTmp.filePath(QStringLiteral("live.pdf"));
        const QString flat = measureTmp.filePath(QStringLiteral("measured.pdf"));
        QString error;
        if (!measureTmp.isValid()
            || !mervin::DocumentOutput::snapshot(*v->document(), collectMeasureDoc(v),
                                                 live, t->password(), &error)
            || MeasureExport::flatten(live, flat, collectRenderMeasurements(v),
                                      t->password(), &error) != MeasureExport::Status::Ok
            || !(flatDoc = engine_->openDocument(flat, t->password(), &error))) {
            QMessageBox::warning(this, tr("Print"), tr("Could not prepare the document:\n%1").arg(error));
            return;
        }
        printDoc = flatDoc.get();
    }
    const int rotation = v->rotation();

    // Pre-select the paper orientation matching the page the user is looking at.
    // pageSize() is the page's unrotated size; fold in the viewer's rotation
    // (90/270 swap the displayed aspect - same rule as ViewLayout::displaySize) so
    // a landscape page rotated to display portrait pre-selects Portrait. Square
    // pages fall through to Portrait.
    QSizeF pageSz = v->document()->pageSize(v->currentPage());
    if (v->rotation() == 90 || v->rotation() == 270)
        pageSz.transpose();
    const QPageLayout::Orientation initialOrientation =
        pageSz.width() > pageSz.height() ? QPageLayout::Landscape : QPageLayout::Portrait;

    QPrinter printer(QPrinter::HighResolution);
    printer.setPageOrientation(initialOrientation);

    // Our own dialog, not QPrintDialog: on Windows the native dialog ignores the
    // orientation we set and renders the driver's mangled option labels. PrintDialog
    // pre-selects orientation reliably and applies the user's choices to `printer`.
    PrintDialog dialog(&printer, engine_, printDoc, rotation, initialOrientation, v->currentPage() + 1,
                       QFileInfo(t->path()).completeBaseName(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // Resolve which pages to print and how to scale/rasterize them. The in-app
    // dialog supplies an explicit page list plus scale + quality; "Print using
    // system dialogue…" hands off to the native QPrintDialog, from which we
    // derive the same values (Fit/Normal, since it has no equivalents).
    QList<int> pages;
    mervin::printing::Settings printSettings;
    if (dialog.useSystemDialog()) {
        printer.setFromTo(1, pageCount);
        QPrintDialog native(&printer, this);
        native.setWindowTitle(tr("Print"));
        native.setOption(QAbstractPrintDialog::PrintPageRange, true);
        native.setOption(QAbstractPrintDialog::PrintCurrentPage, true);
        if (native.exec() != QDialog::Accepted)
            return;
        printSettings.colourMode = printer.colorMode() == QPrinter::GrayScale
                                       ? mervin::printing::ColourMode::Grayscale
                                       : mervin::printing::ColourMode::Colour;
        switch (printer.printRange()) {
        case QPrinter::PageRange: {
            const int f = qBound(1, printer.fromPage(), pageCount);
            const int tt = qBound(f, printer.toPage(), pageCount);
            for (int p = f; p <= tt; ++p)
                pages << p;
            break;
        }
        case QPrinter::CurrentPage:
            pages << (v->currentPage() + 1);
            break;
        default: // AllPages / Selection
            for (int p = 1; p <= pageCount; ++p)
                pages << p;
            break;
        }
    } else {
        pages = dialog.selectedPages();
        printSettings = dialog.printSettings();
    }
    if (pages.isEmpty())
        return;

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::warning(this, tr("Print"), tr("Could not start the print job."));
        return;
    }

    // Rasterize at the chosen quality, capped at the printer's own resolution
    // (asking for more than the device offers just wastes memory). The page image
    // is then scaled to the device per the scale mode below.
    const int printerRes = printer.resolution();
    const int renderDpi = qMin(printerRes, qMax(72, printSettings.qualityDpi));
    const double scale = renderDpi / 72.0;
    const QRectF printable = printer.pageLayout().paintRect(QPageLayout::Point);

    bool first = true;
    for (int p : pages) {
        if (p < 1 || p > pageCount)
            continue;
        if (!first && !printer.newPage()) {
            QMessageBox::warning(this, tr("Print"), tr("Could not start the next printed page."));
            return;
        }
        first = false;
        const QImage img = engine_->renderPageImage(printDoc, p - 1, scale, rotation);
        if (img.isNull()) {
            QMessageBox::warning(this, tr("Print"), tr("Could not render page %1.").arg(p));
            return;
        }

        QSizeF pagePoints = printDoc->pageSize(p - 1);
        if (rotation == 90 || rotation == 270)
            pagePoints.transpose();
        painter.save();
        // Normal QPrinter coordinates already start at the printable corner.
        // Map full-sheet points to that origin once so printer margins are not
        // added twice. The shared painter then matches the preview's geometry.
        painter.scale(printerRes / 72.0, printerRes / 72.0);
        if (!printer.fullPage())
            painter.translate(-printable.topLeft());
        mervin::printing::paintPage(painter, img, pagePoints, printable, printSettings);
        painter.restore();
    }
    painter.end();
}

// ---- Detachable tabs (M9) --------------------------------------------------
