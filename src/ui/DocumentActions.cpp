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
#include "recent/PathKey.h"
#include "security/DocumentOutput.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "ui/TabPage.h"
#include "ui/ViewerWidget.h"

#include <QAbstractButton>
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
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QSet>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
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

// Shows a message that blocks only `window`, so the other windows stay usable.
void tellInWindow(QWidget *window, QMessageBox::Icon icon, const QString &title,
                  const QString &text)
{
    const QPointer<QMessageBox> box(new QMessageBox(icon, title, text, QMessageBox::Ok, window));
    box->setWindowModality(Qt::WindowModal);
    box->exec();
    delete box.data(); // a Quit in another window may already have deleted its parent
}

// Comparison keys of the files open in tabs, which hold canonical paths already, so
// this reads no disk. A page tool's job compares its outputs' fileKey() with them.
QSet<QString> openFileKeys(const QStringList &paths)
{
    QSet<QString> keys;
    for (const QString &path : paths)
        keys.insert(mervin::normalizePathKey(path));
    return keys;
}

} // namespace

struct MainWindow::PageToolResult
{
    PageOps::Status status = PageOps::Status::Failed;
    QString error;
    bool destinationOpen = false; // nothing written, as an output is open in a tab
    QStringList written;          // the files a split wrote
};

struct MainWindow::PageTool
{
    QString title;       // of its messages, such as "Rotate Pages"
    QString destination; // the folder it writes to, which its tab's wait bar names
    // On the job's thread, with the password to open the source with.
    std::function<PageToolResult(const QString &password)> work;
    // In the tab's window once the tool succeeded.
    std::function<void(MainWindow &window, TabPage &tab, const PageToolResult &result)> succeeded;
};

void MainWindow::runPageTool(TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                             const QString &password, bool asked)
{
    const auto result = std::make_shared<PageToolResult>();
    TabPage::Operation operation;
    operation.destination = tool->destination;
    operation.work = [tool, password, result] { *result = tool->work(password); };
    operation.finished = [page = QPointer<TabPage>(tab), tool, password, asked, result] {
        if (page)
            deliverPageTool(page, tool, password, asked, result);
    };
    tab->startOperation(std::move(operation));
}

void MainWindow::deliverPageTool(TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                                 const QString &password, bool asked,
                                 const std::shared_ptr<PageToolResult> &result)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    // The previous owner drops its queue on destruction. A moved tab still needs
    // its result delivered through the surviving owner's queue.
    const auto discarded = connect(window, &QObject::destroyed, tab,
        [page, tool, password, asked, result] {
            QTimer::singleShot(0, page, [page, tool, password, asked, result] {
                if (page)
                    deliverPageTool(page, tool, password, asked, result);
            });
        });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window),
                              tool, password, asked, result, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current)
            return;
        if (current != owner) {
            deliverPageTool(page, tool, password, asked, result);
            return;
        }
        current->finishPageTool(page, tool, password, asked, *result);
    });
}

void MainWindow::finishPageTool(TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                                const QString &password, bool asked,
                                const PageToolResult &result)
{
    const OperationScope operation(this);
    // The boxes below run their own event loops, in which the tab can go.
    const QPointer<TabPage> page(tab);
    if (result.status == PageOps::Status::NeedsPassword && !asked && !hiddenToTray_) {
        const QPointer<QInputDialog> prompt(new QInputDialog(this));
        prompt->setWindowModality(Qt::WindowModal);
        prompt->setWindowTitle(tr("Password Required"));
        prompt->setLabelText(tr("Enter the document password:"));
        prompt->setTextEchoMode(QLineEdit::Password);
        const bool accepted = prompt->exec() == QDialog::Accepted;
        const QString supplied = prompt ? prompt->textValue() : QString();
        delete prompt.data();
        if (accepted && page && page->isUsable())
            runPageTool(page, tool, supplied, true);
        return;
    }
    QString title = tr("Operation failed");
    QString failure;
    if (result.destinationOpen) {
        title = tool->title;
        failure = tr("The destination is open in a tab.");
    } else if (result.status == PageOps::Status::NeedsPassword) {
        failure = tr("A password is required.");
    } else if (result.status != PageOps::Status::Ok) {
        failure = tr("The operation failed.\n\n%1").arg(result.error);
    }
    if (!failure.isEmpty()) {
        if (!hiddenToTray_)
            tellInWindow(this, QMessageBox::Warning, title, failure);
        else if (wm_)
            wm_->notifyInTray(title, failure);
        return;
    }
    if (asked)
        page->setPassword(password);
    if (!hiddenToTray_)
        tool->succeeded(*this, *page, result);
}

QList<int> MainWindow::askPageRange(const QString &title, int pageCount)
{
    bool ok = false;
    // Lower case, since it sits inside the hint. PageRange::isAll() ignores case.
    const QString all = PageRange::allKeyword().toLower();
    const QString spec = QInputDialog::getText(
        this, title,
        //: %1 is the word for every page (PageRange "All"), %2 the page count.
        tr("Pages (e.g. \"%1\", \"1-%2\", \"1,3,5-9\"):").arg(all, QString::number(pageCount)),
        QLineEdit::Normal, all, &ok);
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
    const QPointer<MainWindow> self(this);
    const QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Information,
                    //: Title of the message after a new file was written.
                    tr("Done"),
                    tr("Saved to:\n%1\n\nOpen it now?").arg(QDir::toNativeSeparators(path)),
                    QMessageBox::Yes | QMessageBox::No, this));
    box->setDefaultButton(QMessageBox::Yes);
    box->setWindowModality(Qt::WindowModal);
    const int answer = box->exec();
    delete box.data();
    if (self && answer == QMessageBox::Yes)
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
    //: Menu item that opens the document's password and permission settings.
    documentMenu_->addAction(tr("&Security"), this, &MainWindow::openSecurity);
    // Save / Save as copy / Export with measurements live on the toolbar's
    // dropdown-only Save button (see createToolBar), not in this menu.

    documentButton_->setMenu(documentMenu_);
}

void MainWindow::openSecurity()
{
    const OperationScope operation(this);
    TabPage *t = currentTab();
    if (!t)
        return;
    mervin::SecurityDialog dlg(t->path(), t->password(), this);
    connect(&dlg, &mervin::SecurityDialog::openRequested, this,
            [this](const QString &p) { openFile(p); });
    dlg.exec();
}

// Builds PDF output values while its caller protects the live document. Measurement
// preferences were captured beforehand, and no widget belongs to this work.
static std::vector<mervin::RenderMeasurement> renderMeasurements(
    const Document &document, const mervin::MeasureDoc &measurements)
{
    std::vector<mervin::RenderMeasurement> out;
    const MeasureModel overrides = measurements.overridesModel();
    std::unordered_map<int, std::array<double, 6>> matrices;
    for (const Measurement &mark : measurements.measurements) {
        if (mervin::job::stopped())
            break;
        if (mark.pts.size() < 2)
            continue;
        auto matrix = matrices.find(mark.page);
        if (matrix == matrices.end())
            matrix = matrices.emplace(mark.page, document.pagePointToPdfMatrix(mark.page)).first;
        const auto page = document.pageMeasurement(mark.page);
        const MeasureScale scale = mervin::measure::resolveScale(
            page, mark.pts.front(), overrides.override(mark.page));
        mervin::RenderMeasurement rendered;
        rendered.page = mark.page;
        rendered.kind = mark.kind;
        rendered.pts = mark.pts;
        rendered.hasLabelPos = mark.hasLabelPos;
        rendered.labelPos = mark.labelPos;
        rendered.label = mervin::formatMeasurementValue(
            mark.kind, mark.pts, scale, measurements.unit, measurements.precision);
        rendered.lineWidth = measurements.lineWidth;
        const auto &a = matrix->second;
        rendered.toPdf = mervin::Mat6{a[0], a[1], a[2], a[3], a[4], a[5]};
        out.push_back(std::move(rendered));
    }
    return out;
}

struct MainWindow::CopyResult
{
    enum class Conflict { None, Original, OpenTab };
    Conflict conflict = Conflict::None;
    bool success = false;
    QString error;
};

void MainWindow::startCopy(TabPage *tab, const QString &destination, bool flatten)
{
    if (!tab || !tab->isUsable())
        return;
    ViewerWidget *viewer = tab->viewer();
    viewer->commitActiveFormEditor();
    viewer->commitActiveAnnotEditor();
    const mervin::MeasureDoc measurements = collectMeasureDoc(viewer);
    const auto lifetime = viewer->document()->lifetime();
    const QString password = tab->password();
    const QString sourceKey = mervin::normalizePathKey(tab->canonicalPath());
    QSet<QString> open = openFileKeys(wm_ ? wm_->openTabPaths() : tabPaths());
    open.insert(sourceKey);
    const auto result = std::make_shared<CopyResult>();
    TabPage::Operation operation;
    operation.destination = QFileInfo(destination).absolutePath();
    operation.work = [lifetime, measurements, password, sourceKey, open, destination,
                      flatten, result] {
        const QString folder = QFileInfo(destination).absolutePath();
        if (!mervin::job::writing(folder))
            return;
        const QString outputKey = mervin::fileKey(QFileInfo(destination));
        if (!flatten && outputKey == sourceKey) {
            result->conflict = CopyResult::Conflict::Original;
            return;
        }
        if (open.contains(outputKey) || !mervin::job::reserveDestinations({outputKey})) {
            result->conflict = CopyResult::Conflict::OpenTab;
            return;
        }
        if (!mervin::job::writing(QDir::tempPath()))
            return;
        QTemporaryDir stage;
        if (!stage.isValid()) {
            result->error = QCoreApplication::translate(
                "mervin::DocumentOutput", "Could not create a temporary document.");
            return;
        }
        const QString live = stage.filePath(QStringLiteral("live.pdf"));
        const QString output = stage.filePath(QStringLiteral("output.pdf"));
        std::vector<mervin::RenderMeasurement> marks;
        {
            if (!mervin::job::beginHolding())
                return;
            // This guard runs after Hold releases its gate, including on failure.
            const auto release = qScopeGuard([] { mervin::job::endHolding(); });
            const Document::Hold hold(lifetime, mervin::job::stopped);
            Document *document = hold.document();
            if (!document)
                return;
            if (flatten)
                marks = renderMeasurements(*document, measurements);
            if (!mervin::job::writing(stage.path())
                || !document->savePdfTo(live, &result->error))
                return;
            mervin::job::progressed();
        }
        if (mervin::job::stopped())
            return;
        const auto written = flatten
            ? MeasureExport::flatten(live, output, marks, password, &result->error)
            : MeasureExport::embedMervin(live, output, measurements, password, &result->error);
        if (written == MeasureExport::Status::Ok && !mervin::job::stopped())
            result->success = mervin::DocumentOutput::replace(output, destination, &result->error);
    };
    operation.finished = [page = QPointer<TabPage>(tab), destination, password, flatten, result] {
        if (page)
            deliverCopy(page, destination, password, flatten, result);
    };
    tab->startOperation(std::move(operation));
}

void MainWindow::deliverCopy(TabPage *tab, const QString &destination, const QString &password,
                              bool flatten, const std::shared_ptr<CopyResult> &result)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), destination, password,
                               flatten, result] {
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current)
            return;
        if (current != owner) {
            deliverCopy(page, destination, password, flatten, result);
            return;
        }
        current->finishCopy(destination, password, flatten, *result);
    });
}

void MainWindow::finishCopy(const QString &destination, const QString &password, bool flatten,
                            const CopyResult &result)
{
    const OperationScope operation(this);
    //: Title of messages about exporting with measurements.
    const QString title = flatten ? tr("Export") : tr("Save as Copy");
    QString error;
    if (result.conflict == CopyResult::Conflict::Original) {
        //: "Save edits" is a command in the Save menu. Use its translation here.
        error = tr("Choose a different file name - use Save edits to write back to "
                   "the original.");
    } else if (result.conflict == CopyResult::Conflict::OpenTab) {
        error = tr("The destination is open in a tab.");
    } else if (!result.success) {
        error = flatten ? tr("Could not export:\n%1").arg(result.error)
                        : tr("Could not save:\n%1").arg(result.error);
    }
    if (!error.isEmpty()) {
        if (hiddenToTray_) {
            if (wm_)
                wm_->notifyInTray(title, error);
        } else {
            tellInWindow(this, QMessageBox::Warning, title, error);
        }
    } else if (!hiddenToTray_) {
        offerToOpen(destination, password);
    }
}

void MainWindow::saveAsCopy()
{
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> tab(currentTab());
    if (!tab || !tab->isUsable())
        return;
    const QFileInfo source(tab->path());
    const QString suggested = source.absolutePath() + QLatin1Char('/') + source.completeBaseName()
                              + QStringLiteral("-copy.pdf");
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("Save as Copy"), suggested,
        //: File type filter. Keep the pattern in parentheses.
        tr("PDF documents (*.pdf)"));
    if (!destination.isEmpty() && self && tab && tab->isUsable())
        startCopy(tab, destination, false);
}

mervin::MeasureDoc MainWindow::collectMeasureDoc(ViewerWidget *viewer) const
{
    return viewer ? viewer->measurementDocument() : mervin::MeasureDoc{};
}

std::vector<mervin::RenderMeasurement> MainWindow::collectRenderMeasurements(ViewerWidget *viewer) const
{
    return viewer && viewer->document()
        ? renderMeasurements(*viewer->document(), collectMeasureDoc(viewer))
        : std::vector<mervin::RenderMeasurement>{};
}


void MainWindow::saveMeasurements()
{
    saveTab(currentTab());
}

bool MainWindow::saveTab(TabPage *t)
{
    const OperationScope operation(this);
    // A busy tab's file may be read or replaced by its operation.
    if (!t || t->isBusy())
        return false;
    // An unloaded tab keeps its edits in a recovery copy. Without one there is
    // nothing to save, so do not load the document, which can stall on a network
    // path or fail for a file that is gone.
    if (!t->isLoaded() && !t->hasUnsavedEdits())
        return true;
    QString openError;
    bool needsPassword = false;
    if (!t->isLoaded() && !t->resume(&openError, &needsPassword)) {
        while (needsPassword) {
            bool accepted = false;
            const QString password = QInputDialog::getText(
                this, tr("Password required"),
                //: %1 is the file name.
                tr("Enter the password for %1.").arg(t->tabTitle()),
                QLineEdit::Password, {}, &accepted);
            if (!accepted)
                return false;
            t->setPassword(password);
            if (t->resume(&openError, &needsPassword))
                break;
        }
        if (!t->isLoaded()) {
            //: Title of messages about saving the document.
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
            //: %1 is the file name.
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
        if (!t->recoverSnapshot(snapshot, &openError)) {
            //: %1 is why saving failed (may be empty), %2 the path of the kept
            //: copy, %3 why that copy could not be opened.
            error = tr("%1\nEdited document retained at %2.\n%3").arg(error, snapshot, openError);
        } else if (saved) {
            error = tr("The file was saved, but could not be reopened. The edited copy remains open.");
        }
        applyViewStateToViewer(v, state);
        updateForCurrentTab();
        QMessageBox::warning(this, tr("Save"), error);
        return false;
    }
    applyViewStateToViewer(v, state);
    if (formMode && v->hasFormFields())
        v->setFormMode(true);
    updateForCurrentTab();
    //: Status bar text. %1 is the saved file's path.
    statusInfo_->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
    return true;
}

int MainWindow::askClose(TabPage *tab)
{
    const OperationScope operation(this);
    const QPointer<TabPage> page(tab);
    const QPointer<MainWindow> window(this);
    // Active editors can contain text that has not emitted an edit signal yet.
    if (tab->isUsable()) {
        auto *viewer = tab->viewer();
        viewer->commitActiveFormEditor();
        if (!page || !window)
            return QMessageBox::Discard;
        viewer->commitActiveAnnotEditor();
        if (!page || !window)
            return QMessageBox::Discard;
    }
    if (!tab->hasUnsavedEdits())
        return QMessageBox::Discard;
    const QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Question, tr("Unsaved changes"),
                    //: %1 is the file name.
                    tr("Save changes to %1?").arg(tab->tabTitle()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this));
    box->setDefaultButton(QMessageBox::Save);
    box->setWindowModality(Qt::WindowModal);
    // Qt names this button after the platform ("Discard", or "Close without
    // Saving" on GNOME). Say what it does, in the app's own catalogs.
    //: Button in the unsaved-changes question. Closes the document and loses the
    //: changes made since it was last saved. The file on disk stays as it is.
    box->button(QMessageBox::Discard)->setText(tr("Don't save"));
    const int answer = box->exec();
    delete box.data();
    return answer;
}

void MainWindow::exportMeasuredCopy()
{
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> tab(currentTab());
    if (!tab || !tab->isUsable())
        return;
    if (!tab->viewer()->hasMeasurements()) {
        QMessageBox::information(this, tr("Export with Measurements"),
                                 tr("There are no measurements to export. Add measurements first."));
        return;
    }
    const QPointer<ExportMeasureDialog> dialog = new ExportMeasureDialog(this);
    const bool accepted = dialog->exec() == QDialog::Accepted;
    delete dialog.data();
    if (!accepted || !self || !tab || !tab->isUsable())
        return;
    const QFileInfo source(tab->path());
    const QString suggested = source.absolutePath() + QLatin1Char('/') + source.completeBaseName()
                              + QStringLiteral("-measured.pdf");
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("Export with Measurements"), suggested, tr("PDF documents (*.pdf)"));
    if (!destination.isEmpty() && self && tab && tab->isUsable())
        startCopy(tab, destination, true);
}

void MainWindow::rotatePagesOp()
{
    const OperationScope operation(this);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    const QList<int> pages = askPageRange(tr("Rotate Pages"), v->pageCount());
    if (pages.isEmpty())
        return;
    // "180°" is a number and a unit symbol, so it is not translated.
    const QStringList angles{tr("90° clockwise"), QStringLiteral("180°"),
                             tr("90° counter-clockwise")};
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
    // Only the tab waits. qpdf reads the file on disk and writes the copy in the
    // background.
    auto tool = std::make_shared<PageTool>();
    tool->title = tr("Rotate Pages");
    tool->destination = QFileInfo(out).absolutePath();
    tool->work = [in = t->path(), out, pages, angle,
                  open = openFileKeys(wm_ ? wm_->openTabPaths() : tabPaths())](
                     const QString &password) {
        PageToolResult result;
        // The viewer holds an open file, which a replace would fail on or corrupt.
        const QString key = mervin::fileKey(QFileInfo(out));
        if (open.contains(key) || !mervin::job::reserveDestinations({key}))
            result.destinationOpen = true;
        else
            result.status = PageOps::rotatePages(in, out, pages, angle, true, password,
                                                 &result.error);
        return result;
    };
    tool->succeeded = [out](MainWindow &window, TabPage &tab, const PageToolResult &) {
        window.offerToOpen(out, tab.password()); // output keeps the source's encryption
    };
    runPageTool(t, tool, t->password(), false);
}

void MainWindow::deletePagesOp()
{
    // The page being read is the one most often deleted, so it is the first row.
    if (ViewerWidget *v = currentViewer())
        pagesDialog(mervin::ExtractPlan::Mode::Delete, v->currentPage());
}

void MainWindow::extractPagesOp()
{
    pagesDialog(mervin::ExtractPlan::Mode::Extract, std::nullopt);
}

void MainWindow::pagesDialog(mervin::ExtractPlan::Mode mode, std::optional<int> seedPage)
{
    const OperationScope operation(this);
    TabPage *t = currentTab();
    ViewerWidget *v = currentViewer();
    if (!t || !v)
        return;
    // Bake an edit still being typed into the live document so the unsaved-changes
    // note sees it. Nothing is written here, since both modes copy from the file on disk.
    v->commitActiveFormEditor();
    v->commitActiveAnnotEditor();

    mervin::ExtractDialog::Source src;
    src.mode = mode;
    src.path = t->path();
    src.canonicalPath = t->canonicalPath();
    src.password = t->password();
    src.viewerPageCount = v->pageCount();
    src.seedPage = seedPage;
    // The tab's own state, since a document reloaded from its recovery copy shows
    // edits the viewer does not count as unsaved.
    src.hasUnsavedEdits = t->hasUnsavedEdits();
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
    // Delete's copy keeps the source's encryption, so it opens with its password.
    const mervin::ExtractPlan::Job job = dlg.job();
    statusBar()->showMessage(mervin::ExtractPlan::doneText(job), 5000);
    if (dlg.openWhenDone())
        openFile(job.path, false, -1, true,
                 job.mode == mervin::ExtractPlan::Mode::Delete ? dlg.password() : QString());
}

void MainWindow::splitDocument()
{
    const OperationScope operation(this);
    TabPage *t = currentTab();
    if (!t || !t->isUsable())
        return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose Output Folder"),
                                                          QFileInfo(t->path()).absolutePath());
    if (dir.isEmpty())
        return;
    // Only the tab waits. qpdf reads the file on disk and writes the pages in the
    // background.
    auto tool = std::make_shared<PageTool>();
    tool->title = tr("Split Pages");
    tool->destination = dir;
    tool->work = [in = t->path(), dir, base = QFileInfo(t->path()).completeBaseName(),
                  open = openFileKeys(wm_ ? wm_->openTabPaths() : tabPaths())](
                     const QString &password) {
        PageToolResult result;
        // The viewer holds an open file, which a replace would fail on or corrupt.
        const auto mayWrite = [&result, &open, &dir](const QStringList &outputs) {
            // Resolving an output's canonical path can wait on its drive too.
            if (!mervin::job::writing(dir)) {
                result.error = mervin::job::notRunError();
                return false;
            }
            QSet<QString> keys;
            for (const QString &output : outputs) {
                const QString key = mervin::fileKey(QFileInfo(output));
                keys.insert(key);
                result.destinationOpen = result.destinationOpen || open.contains(key);
            }
            if (!result.destinationOpen && !mervin::job::stopped())
                result.destinationOpen = !mervin::job::reserveDestinations(keys);
            return !result.destinationOpen;
        };
        result.status = PageOps::split(in, dir, base, password, &result.written, &result.error,
                                       mayWrite);
        return result;
    };
    tool->succeeded = [dir](MainWindow &window, TabPage &, const PageToolResult &result) {
        // The English catalog supplies "file" and "files" for "%n file(s)".
        tellInWindow(&window, QMessageBox::Information, tr("Split Pages"),
                     //: %1 is the folder the files were written to.
                     tr("Wrote %n file(s) to:\n%1", nullptr, int(result.written.size()))
                         .arg(QDir::toNativeSeparators(dir)));
    };
    runPageTool(t, tool, t->password(), false);
}

void MainWindow::mergeDocuments()
{
    const OperationScope operation(this);
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
    const OperationScope operation(this);
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
            //: Title of the print dialog and of messages about printing.
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
