#include "ui/MainWindow.h"
#include "app/WindowManager.h"
#include "dialogs/ExportMeasureDialog.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/PrintDialog.h"
#include "dialogs/SecurityDialog.h"
#include "io/DestinationConsent.h"
#include "io/JobContext.h"
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
#include "ui/TextLayout.h"
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
#include <QScreen>
#include <QSet>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>

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
using mervin::QpdfService;
using mervin::SecurityDialog;

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

// The measured preview's files must outlive its Document, including pending preview
// renders. Neither document closure nor temporary-folder cleanup runs on the GUI.
struct PreparedPrintDocument
{
    std::unique_ptr<QTemporaryDir> stage;
    mervin::OwnedDocument document;

    // Defers private-folder removal until the prepared document and its readers close.
    ~PreparedPrintDocument()
    {
        if (stage) {
            auto files = std::shared_ptr<QTemporaryDir>(std::move(stage));
            Document::closeInBackground(std::unique_ptr<Document>(document.release()),
                                       [files = std::move(files)] {});
        }
    }
};

// Carries printer start, page and final submission failures to GUI reporting.
struct PrintResult
{
    enum class Failure { None, DestinationOpen, TemporaryFile, Start, NextPage, Render, End, Write };
    Failure failure = Failure::None;
    QString error;
    int page = 0;
};

// Paints on the worker that owns `printer`. Each page releases live document access
// before painting or entering the printer system.
void printPages(QPrinter &printer, const std::shared_ptr<mervin::DocumentLifetime> &lifetime,
                const QList<int> &pages, int rotation, const mervin::printing::Settings &settings,
                const QString &printerName, bool liveDocument, PrintResult &result)
{
    if (!mervin::job::printing(printerName)) {
        result.failure = PrintResult::Failure::Start;
        return;
    }
    QPainter painter;
    if (!painter.begin(&printer)) {
        result.failure = PrintResult::Failure::Start;
        return;
    }
    // Error paths and Stop abort instead of submitting a partial print job. The
    // painter still closes its engine while the QPrinter remains alive here.
    const auto abort = qScopeGuard([&] {
        if (painter.isActive())
            printer.abort();
    });
    const int resolution = printer.resolution();
    const int renderDpi = qMin(resolution, qMax(72, settings.qualityDpi));
    const QRectF printable = printer.pageLayout().paintRect(QPageLayout::Point);
    bool first = true;
    for (int page : pages) {
        if (mervin::job::stopped())
            return;
        QImage image;
        QSizeF pagePoints;
        {
            if (liveDocument && !mervin::job::beginHolding())
                return;
            const auto release = qScopeGuard([liveDocument] {
                if (liveDocument)
                    mervin::job::endHolding();
            });
            const Document::Hold hold(lifetime, mervin::job::stopped);
            Document *document = hold.document();
            if (!document) {
                result.failure = PrintResult::Failure::Render;
                result.page = page;
                return;
            }
            image = document->renderPage(page - 1, renderDpi / 72.0, rotation);
            pagePoints = document->pageSize(page - 1);
        }
        if (mervin::job::stopped())
            return;
        if (image.isNull()) {
            result.failure = PrintResult::Failure::Render;
            result.page = page;
            return;
        }
        if (!mervin::job::printing(printerName)) {
            result.failure = PrintResult::Failure::NextPage;
            return;
        }
        if (!first && !printer.newPage()) {
            result.failure = PrintResult::Failure::NextPage;
            return;
        }
        if (mervin::job::stopped())
            return;
        first = false;
        if (rotation == 90 || rotation == 270)
            pagePoints.transpose();
        painter.save();
        painter.scale(resolution / 72.0, resolution / 72.0);
        if (!printer.fullPage())
            painter.translate(-printable.topLeft());
        mervin::printing::paintPage(painter, image, pagePoints, printable, settings);
        painter.restore();
        mervin::job::progressed();
    }
    if (!mervin::job::beginPrintingCommit(printerName)) {
        result.failure = PrintResult::Failure::End;
        return;
    }
    const bool ended = painter.end();
    mervin::job::endCommit();
    if (!ended)
        result.failure = PrintResult::Failure::End;
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

/// Freezes source credentials, write choices and approved destination contents for each attempt.
struct MainWindow::SecurityRequest
{
    QString source;
    QString password;
    QpdfService::Info info;
    QString infoError;
    std::optional<SecurityDialog::Request> action;
    std::optional<mervin::DestinationConsent> consent;
};

/// Returns metadata read by the worker or the write outcome and destination conflict.
struct MainWindow::SecurityResult
{
    enum class Destination { None, OpenTab, Confirm, Changed, CheckFailed };
    QpdfService::Status status = QpdfService::Status::Failed;
    QpdfService::Info info;
    QString error;
    Destination destination = Destination::None;
    std::optional<mervin::DestinationConsent> consent;
};

/// Starts a background security-information read for the current usable tab.
void MainWindow::openSecurity()
{
    const OperationScope operation(this);
    TabPage *tab = currentTab();
    if (!tab || !tab->isUsable())
        return;
    const auto request = std::make_shared<SecurityRequest>();
    request->source = tab->path();
    request->password = tab->password();
    runSecurity(tab, request, false);
}

/// Captures consent before source reads and validates it under the writer's publication lock.
void MainWindow::runSecurity(TabPage *tab, const std::shared_ptr<SecurityRequest> &request,
                              bool asked)
{
    auto *owner = qobject_cast<MainWindow *>(tab->window());
    if (!owner || !tab->isUsable() || owner->closeConfirmed_
        || (owner->wm_ && owner->wm_->isClosing()))
        return;
    const QSet<QString> open = openFileKeys(owner->wm_ ? owner->wm_->openTabPaths() : owner->tabPaths());
    const auto result = std::make_shared<SecurityResult>();
    TabPage::Operation operation;
    if (request->action)
        operation.destination = QFileInfo(request->action->output).absolutePath();
    operation.work = [request, result, open] {
        QpdfService service;
        if (!request->action) {
            result->status = service.readInfo(request->source, request->password,
                                              result->info, &result->error);
            return;
        }
        const auto &action = *request->action;
        result->consent = request->consent;
        if (!result->consent) {
            result->consent = mervin::DestinationConsent::read(action.output, &result->error);
            if (!result->consent) {
                result->destination = SecurityResult::Destination::CheckFailed;
                return;
            }
            // Open-tab keys are already in memory. Reject them before asking to
            // replace an existing file, without touching the source filesystem.
            if (open.contains(result->consent->key)) {
                result->destination = SecurityResult::Destination::OpenTab;
                return;
            }
            if (result->consent->exists) {
                result->destination = SecurityResult::Destination::Confirm;
                return;
            }
        }
        if (!mervin::job::reading(request->source)) {
            result->error = mervin::job::notRunError();
            return;
        }
        const QString source = mervin::fileKey(QFileInfo(request->source));
        if (!mervin::job::writing(QFileInfo(action.output).absolutePath())) {
            result->error = mervin::job::notRunError();
            return;
        }
        const QString destination = mervin::fileKey(QFileInfo(action.output));
        if (destination == source || open.contains(destination)
            || !mervin::job::reserveDestinations({destination})) {
            result->destination = SecurityResult::Destination::OpenTab;
            return;
        }
        const mervin::DestinationConsent consent = *result->consent;
        QString consentError;
        const auto mayCommit = [&] {
            const auto current = mervin::DestinationConsent::read(action.output, &consentError);
            if (!current) {
                result->destination = SecurityResult::Destination::CheckFailed;
                return false;
            }
            if (*current != consent) {
                result->destination = SecurityResult::Destination::Changed;
                return false;
            }
            return true;
        };
        if (action.action == SecurityDialog::Action::Encrypt) {
            result->status = service.encrypt(request->source, action.output, request->password,
                action.userPassword, action.ownerPassword, action.algorithm, action.permissions,
                &result->error, mayCommit);
        } else {
            result->status = service.decrypt(request->source, action.output, request->password,
                                             &result->error, mayCommit);
        }
        if (result->destination == SecurityResult::Destination::Changed)
            result->error.clear();
        else if (!consentError.isEmpty())
            result->error = consentError;
    };
    operation.finished = [page = QPointer<TabPage>(tab), request, result, asked] {
        if (page)
            deliverSecurity(page, request, result, asked);
    };
    tab->startOperation(std::move(operation));
}

/// Queues the worker result for its tab owner unless shutdown was confirmed.
void MainWindow::deliverSecurity(TabPage *tab, const std::shared_ptr<SecurityRequest> &request,
                                  const std::shared_ptr<SecurityResult> &result, bool asked)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window || window->closeConfirmed_ || (window->wm_ && window->wm_->isClosing()))
        return;
    const auto discarded = connect(window, &QObject::destroyed, tab, [page, request, result, asked] {
        QTimer::singleShot(0, page, [page, request, result, asked] {
            if (page)
                deliverSecurity(page, request, result, asked);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window),
                              request, result, asked, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || current->closeConfirmed_ || (current->wm_ && current->wm_->isClosing()))
            return;
        if (current != owner) {
            deliverSecurity(page, request, result, asked);
            return;
        }
        current->finishSecurity(page, request, result, asked);
    });
}

/// Selects the launching tab for questions and handles password retry or output opening.
void MainWindow::finishSecurity(TabPage *tab, const std::shared_ptr<SecurityRequest> &request,
                                 const std::shared_ptr<SecurityResult> &result, bool asked)
{
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    if (!tab->isUsable() || closeConfirmed_ || (wm_ && wm_->isClosing()))
        return;
    // A requested information dialog belongs to its tab, even after the user
    // selected another one while the read waited. Do not raise its window.
    if (!hiddenToTray_ && (!request->action || result->status != QpdfService::Status::Ok)) {
        recentActive_ = false;
        tabs_->setCurrentWidget(tab);
        if (!self || !page || !page->isUsable() || page->window() != self
            || closeConfirmed_ || (wm_ && wm_->isClosing()))
            return;
        updateForCurrentTab();
        if (!self || !page || !page->isUsable() || page->window() != self
            || closeConfirmed_ || (wm_ && wm_->isClosing()))
            return;
    }
    if (result->destination == SecurityResult::Destination::Confirm) {
        if (!request->action || !result->consent || hiddenToTray_ || !isVisible()
            || isMinimized() || currentTab() != page)
            return;
        // The question approves this exact background snapshot. Nested events
        // cannot refresh it while the user is deciding whether to replace it.
        const auto confirmed = std::make_shared<SecurityRequest>(*request);
        confirmed->consent = result->consent;
        const QPointer<QMessageBox> question(new QMessageBox(QMessageBox::Question,
            SecurityDialog::tr("Document Security"), QString(),
            QMessageBox::Yes | QMessageBox::No, this));
        question->setObjectName(QStringLiteral("securityReplace"));
        question->setTextFormat(Qt::PlainText);
        question->setDefaultButton(QMessageBox::No);
        question->setWindowModality(Qt::WindowModal);
        question->ensurePolished();
        // QMessageBox wraps the sentence but can clip an unbroken filename.
        // Keep both ends of the name and leave room for the icon and translated text.
        const QString name = QChar(0x2068) + mervin::elidedLabelText(question->fontMetrics(),
            QFileInfo(request->action->output).fileName(), Qt::ElideMiddle,
            qMax(1, question->screen()->availableGeometry().width() / 2)) + QChar(0x2069);
        //: %1 is the destination filename. The background check captured its contents
        //: before this question. Yes approves replacing that version; nothing has
        //: been replaced yet. A changed file will require another decision.
        question->setText(SecurityDialog::tr("\"%1\" already exists. Replace it?").arg(name));
        const int answer = question->exec();
        delete question.data();
        if (!self || !page || !page->isUsable() || page->window() != self
            || closeConfirmed_ || (wm_ && wm_->isClosing()) || hiddenToTray_
            || !isVisible() || isMinimized() || currentTab() != page)
            return;
        if (answer == QMessageBox::Yes)
            runSecurity(page, confirmed, asked);
        else
            queueSecurityDialog(page, request);
        return;
    }
    if (result->status == QpdfService::Status::NeedsPassword && !asked
        && !hiddenToTray_ && isVisible() && !isMinimized() && currentTab() == page) {
        const QPointer<QInputDialog> prompt(new QInputDialog(this));
        prompt->setWindowModality(Qt::WindowModal);
        prompt->setWindowTitle(tr("Password required"));
        //: %1 is the document's name. This asks for the password needed to open
        //: its contents, not the owner password that guards permission flags.
        prompt->setLabelText(tr("Enter the password for %1.").arg(page->tabTitle()));
        prompt->setTextEchoMode(QLineEdit::Password);
        const bool accepted = prompt->exec() == QDialog::Accepted;
        const QString password = prompt && accepted ? prompt->textValue() : QString();
        delete prompt.data();
        if (!self || !page || !page->isUsable() || page->window() != self
            || closeConfirmed_ || (wm_ && wm_->isClosing()) || !accepted)
            return;
        const auto retry = std::make_shared<SecurityRequest>(*request);
        retry->password = password;
        // An absent output can reach a password prompt on its first attempt.
        // Preserve that attempt's consent instead of checking a newer file here.
        retry->consent = result->consent;
        runSecurity(page, retry, true);
        return;
    }
    if (result->status == QpdfService::Status::Ok && asked)
        page->setPassword(request->password);
    if (!request->action) {
        QString error;
        if (result->status == QpdfService::Status::NeedsPassword)
            error = SecurityDialog::tr("A password is required.");
        else if (result->status != QpdfService::Status::Ok)
            error = result->error;
        if (hiddenToTray_) {
            if (!error.isEmpty() && wm_)
                wm_->notifyInTray(SecurityDialog::tr("Document Security"), error);
            return;
        }
        const auto ready = std::make_shared<SecurityRequest>(*request);
        ready->info = result->info;
        ready->infoError = error;
        showSecurityDialog(page, ready);
        return;
    }
    if (result->status == QpdfService::Status::Ok) {
        if (!hiddenToTray_)
            offerToOpen(request->action->output,
                request->action->action == SecurityDialog::Action::Encrypt
                    ? request->action->userPassword : QString());
        return;
    }
    QString failure;
    if (result->destination == SecurityResult::Destination::OpenTab)
        failure = tr("The destination is open in a tab.");
    else if (result->destination == SecurityResult::Destination::Changed) {
        //: The output contents or target changed after this Security action was approved.
        //: Nothing was replaced. Security reopens with the same choices. Try again means
        //: choosing Encrypt, Remove Password or Strip Owner Restrictions again to check
        //: the destination and confirm any replacement of an existing file.
        failure = SecurityDialog::tr("The destination changed. Try again to review it.");
    } else if (result->destination == SecurityResult::Destination::CheckFailed) {
        //: The destination could not be read to verify replacement consent. Nothing was
        //: replaced. Security reopens with the same choices so the user can retry the
        //: action or choose another output filename in its Save As picker.
        failure = SecurityDialog::tr("Could not check the destination. Try again or choose another name.");
    } else if (result->status == QpdfService::Status::NeedsPassword)
        failure = SecurityDialog::tr("The correct password is required.");
    else {
        //: %1 is the reason the security copy could not be written.
        failure = SecurityDialog::tr("The operation failed.\n\n%1").arg(result->error);
    }
    const QString title = SecurityDialog::tr("Operation failed");
    if (hiddenToTray_) {
        if (wm_)
            wm_->notifyInTray(title, failure);
        return;
    }
    tellInWindow(this, QMessageBox::Warning, title, failure);
    if (self && page && page->isUsable())
        queueSecurityDialog(page, request);
}

/// Queues a retained information dialog or failed request in the surviving tab's window.
void MainWindow::queueSecurityDialog(TabPage *tab, const std::shared_ptr<SecurityRequest> &request)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window || window->closeConfirmed_ || (window->wm_ && window->wm_->isClosing()))
        return;
    const auto discarded = connect(window, &QObject::destroyed, tab, [page, request] {
        QTimer::singleShot(0, page, [page, request] {
            if (page)
                queueSecurityDialog(page, request);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window),
                              request, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || current->closeConfirmed_ || (current->wm_ && current->wm_->isClosing()))
            return;
        if (current != owner) {
            queueSecurityDialog(page, request);
            return;
        }
        current->showSecurityDialog(page, request);
    });
}

/// Edits retained choices in the current owner and starts the accepted worker write.
void MainWindow::showSecurityDialog(TabPage *tab, const std::shared_ptr<SecurityRequest> &request)
{
    auto *owner = qobject_cast<MainWindow *>(tab->window());
    if (!owner || !tab->isUsable() || owner->closeConfirmed_
        || (owner->wm_ && owner->wm_->isClosing()))
        return;
    if (owner != this) {
        queueSecurityDialog(tab, request);
        return;
    }
    if (hiddenToTray_)
        return;
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    recentActive_ = false;
    tabs_->setCurrentWidget(tab);
    if (!self || !page || !page->isUsable() || page->window() != self)
        return;
    updateForCurrentTab();
    if (!self || !page || !page->isUsable() || page->window() != self)
        return;
    const QPointer<SecurityDialog> dialog(new SecurityDialog(request->source, request->info,
                                                request->infoError, request->action, this));
    dialog->setWindowModality(Qt::WindowModal);
    const bool accepted = dialog->exec() == QDialog::Accepted;
    const auto action = dialog && accepted ? dialog->request() : std::nullopt;
    delete dialog.data();
    if (!self || !page || !page->isUsable() || !action)
        return;
    const auto write = std::make_shared<SecurityRequest>(*request);
    write->action = action;
    write->consent.reset();
    runSecurity(page, write, false);
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

// Carries a copy worker outcome and destination conflicts to the owning window.
struct MainWindow::CopyResult
{
    enum class Conflict { None, Original, OpenTab };
    Conflict conflict = Conflict::None;
    bool success = false;
    QString error;
};

// Copies the tab's edits on a worker, optionally flattening its measurements.
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
        if (!mervin::job::writing(folder)) {
            result->error = mervin::job::notRunError();
            return;
        }
        const QString outputKey = mervin::fileKey(QFileInfo(destination));
        if (!flatten && outputKey == sourceKey) {
            result->conflict = CopyResult::Conflict::Original;
            return;
        }
        if (open.contains(outputKey) || !mervin::job::reserveDestinations({outputKey})) {
            result->conflict = CopyResult::Conflict::OpenTab;
            return;
        }
        if (!mervin::job::writing(QDir::tempPath())) {
            result->error = mervin::job::notRunError();
            return;
        }
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
            if (!document) {
                result->error = mervin::job::notRunError();
                return;
            }
            if (flatten)
                marks = renderMeasurements(*document, measurements);
            if (!mervin::job::writing(stage.path())) {
                result->error = mervin::job::notRunError();
                return;
            }
            if (!document->savePdfTo(live, &result->error))
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

// Queues copy completion in the surviving owner of its source tab.
void MainWindow::deliverCopy(TabPage *tab, const QString &destination, const QString &password,
                              bool flatten, const std::shared_ptr<CopyResult> &result)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    const auto discarded = connect(window, &QObject::destroyed, page,
        [page, destination, password, flatten, result] {
            if (page)
                QTimer::singleShot(0, page, [page, destination, password, flatten, result] {
                    if (page)
                        deliverCopy(page, destination, password, flatten, result);
                });
        });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), destination, password,
                               flatten, result, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || current->closeConfirmed_ || (current->wm_ && current->wm_->isClosing()))
            return;
        if (current != owner) {
            deliverCopy(page, destination, password, flatten, result);
            return;
        }
        current->finishCopy(destination, password, flatten, *result);
    });
}

// Reports copy failure or offers the completed file from the current window.
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

// Captures the source tab across the destination picker and starts its copy job.
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


// Dispatches the current tab Save through the common background-save entry point.
void MainWindow::saveMeasurements()
{
    saveTab(currentTab(), {});
}

// Starts Save for a usable loaded tab or retained recovery and queues owner delivery.
void MainWindow::saveTab(TabPage *tab, std::function<void(TabPage::SaveResult)> completion)
{
    if (!tab || tab->isBusy()) {
        if (completion)
            completion(TabPage::SaveResult::Failed);
        return;
    }
    const QPointer<TabPage> page(tab);
    TabPage::SaveCallbacks callbacks;
    callbacks.changed = [page](auto valid, auto answer) {
        askSaveReplacement(page, std::move(valid), std::move(answer));
    };
    callbacks.finished = [page, completion = std::move(completion)](const auto &result) {
        if (completion)
            completion(result.result);
        if (page)
            deliverSave(page, result);
    };
    tab->startSave(std::move(callbacks));
}

// Asks the current owner to approve a changed source only while its checkpoint is valid.
void MainWindow::askSaveReplacement(QPointer<TabPage> page, std::function<bool()> valid,
                                     std::function<void(bool)> answer)
{
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window || !valid()) {
        answer(false);
        return;
    }
    // A moved tab survives its old window, whose pending queue is discarded.
    const auto discarded = connect(window, &QObject::destroyed, page, [page, valid, answer] {
        QTimer::singleShot(0, page, [page, valid, answer] {
            if (page && valid())
                askSaveReplacement(page, valid, answer);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window),
                               valid = std::move(valid), answer = std::move(answer), discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || !valid()) {
            answer(false);
            return;
        }
        if (current != owner) {
            askSaveReplacement(page, valid, answer);
            return;
        }
        if (current->hiddenToTray_) {
            answer(false);
            return;
        }
        const OperationScope operation(current);
        const QPointer<QMessageBox> prompt(new QMessageBox(
            QMessageBox::Question, tr("File changed"),
            //: %1 is the file name.
            tr("%1 changed on disk since it was opened. Replace it with your edited copy?")
                .arg(page->tabTitle()),
            QMessageBox::Yes | QMessageBox::Cancel, current));
        prompt->setWindowModality(Qt::WindowModal);
        prompt->setDefaultButton(QMessageBox::Cancel);
        const bool replace = prompt->exec() == QMessageBox::Yes;
        delete prompt.data();
        answer(replace && valid() && page && page->window() == owner);
    });
}

// Delivers terminal Save status in the surviving owner of the source tab.
void MainWindow::deliverSave(QPointer<TabPage> page, const TabPage::SaveCompletion &result)
{
    if (!page || page->saveGeneration() != result.generation)
        return;
    if (result.result == TabPage::SaveResult::Stopped
        || result.result == TabPage::SaveResult::Canceled)
        return;
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    const auto discarded = connect(window, &QObject::destroyed, page, [page, result] {
        QTimer::singleShot(0, page, [page, result] {
            if (page)
                deliverSave(page, result);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), result, discarded] {
        disconnect(discarded);
        if (!page || page->saveGeneration() != result.generation)
            return;
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current)
            return;
        if (current != owner) {
            deliverSave(page, result);
            return;
        }
        if (result.result == TabPage::SaveResult::Saved) {
            if (!current->hiddenToTray_ && current->currentTab() == page) {
                current->updateForCurrentTab();
                //: Status bar text. %1 is the saved file's path.
                if (owner && page && owner->currentTab() == page)
                    owner->statusInfo_->setText(tr("Saved %1").arg(QDir::toNativeSeparators(page->path())));
            }
            return;
        }
        if (result.error.isEmpty())
            return;
        QString error;
        if (result.originalSaved) {
            //: The original file was replaced successfully, but reading it again
            //: failed. The retained edited copy may still be loading for recovery.
            error = tr("The file was saved, but could not be reopened.");
        } else {
            error = tr("Could not save:\n%1").arg(result.error);
        }
        if (result.originalSaved && page->isLoaded()) {
            error = tr("The file was saved, but could not be reopened. The edited copy remains open.");
        } else if (page->hasRecoverySnapshot()) {
            //: %1 describes the Save or reopening result, %2 is the retained copy,
            //: and %3 is a recovery error.
            error = tr("%1\nEdited document retained at %2.\n%3")
                .arg(error, QDir::toNativeSeparators(page->recoveryPath()), QString());
        }
        const OperationScope operation(current);
        if (current->hiddenToTray_) {
            if (current->wm_)
                current->wm_->notifyInTray(tr("Save"), error);
        } else {
            tellInWindow(current, QMessageBox::Warning, tr("Save"), error);
        }
    });
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

// Explains flattened output, then starts export for the guarded source tab.
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

// Captures the selected page tool choices and starts a protected tab operation.
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
        // Copy tools must not replace a file open in a tab.
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

// The probe and failed-attempt choices travel with their tab. Only the worker
// mutates a running result, published through TabPage's job mutex before delivery.
struct MainWindow::PagesJob
{
    mervin::ExtractDialog::Source source;
    std::optional<mervin::ExtractDialog::Choices> choices;
    mervin::ExtractPlan::Job output;
    bool writing = false;
    bool succeeded = false;
};

/// Captures the launching tab and starts its source probe before opening the editor.
void MainWindow::pagesDialog(mervin::ExtractPlan::Mode mode, std::optional<int> seedPage)
{
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> tab(currentTab());
    if (!tab || !tab->isUsable())
        return;
    tab->viewer()->commitActiveFormEditor();
    if (!self || !tab || !tab->isUsable())
        return;
    tab->viewer()->commitActiveAnnotEditor();
    if (!self || !tab || !tab->isUsable())
        return;
    auto job = std::make_shared<PagesJob>();
    auto &source = job->source;
    source.mode = mode;
    source.path = tab->path();
    source.canonicalPath = tab->canonicalPath();
    source.password = tab->password();
    source.viewerPageCount = tab->viewer()->pageCount();
    source.seedPage = seedPage;
    source.hasUnsavedEdits = tab->hasUnsavedEdits();
    source.openWhenDone = settings_.extractOpenWhenDone;
    runPagesJob(tab, job, false);
}

/// Runs a source probe or accepted page plan as a tab-owned worker operation.
void MainWindow::runPagesJob(TabPage *tab, const std::shared_ptr<PagesJob> &result, bool write)
{
    if (!tab || !tab->isUsable())
        return;
    auto *owner = qobject_cast<MainWindow *>(tab->window());
    if (!owner)
        return;
    const QSet<QString> open = openFileKeys(owner->wm_ ? owner->wm_->openTabPaths()
                                                     : owner->tabPaths());
    result->writing = write;
    result->succeeded = false;
    TabPage::Operation operation;
    operation.destination = QFileInfo(write ? result->output.path : result->source.path).absolutePath();
    operation.work = [result, open, write] {
        using Dialog = mervin::ExtractDialog;
        auto &source = result->source;
        if (!write) {
            source.probe = Dialog::probeSource(source.path, source.password);
            return;
        }
        auto &choices = *result->choices;
        choices.failure = Dialog::Choices::Failure::Write;
        choices.error = mervin::job::notRunError();
        const QString folder = QFileInfo(result->output.path).absolutePath();
        if (!mervin::job::writing(folder))
            return;
        const QString key = mervin::fileKey(QFileInfo(result->output.path));
        // The source may have been redirected while the dialog was open. Its
        // current target must remain protected even when the tab owns the old key.
        if (!mervin::job::reading(source.path))
            return;
        const QString sourceKey = mervin::fileKey(QFileInfo(source.path));
        if (mervin::job::stopped())
            return;
        if (key == sourceKey) {
            choices.failure = Dialog::Choices::Failure::Source;
            choices.error.clear();
            return;
        }
        if (open.contains(key) || !mervin::job::reserveDestinations({key})) {
            choices.failure = Dialog::Choices::Failure::Destination;
            choices.error.clear();
            return;
        }
        const int oldCount = source.probe.status == PageOps::Status::Ok
            ? source.probe.pageCount : source.viewerPageCount;
        // Verification belongs here even if the row was absent. The source may
        // have changed since the first probe while the user chose its pages.
        Dialog::Probe probe;
        probe.status = PageOps::probe(source.path, &probe.pageCount, choices.password, &probe.error);
        if (probe.status == PageOps::Status::Ok)
            probe.password = choices.password;
        source.probe = probe;
        if (probe.status != PageOps::Status::Ok) {
            choices.failure = probe.status == PageOps::Status::NeedsPassword
                ? Dialog::Choices::Failure::Password : Dialog::Choices::Failure::Read;
            choices.error = probe.error;
            return;
        }
        if (probe.pageCount != oldCount) {
            choices.failure = Dialog::Choices::Failure::ChangedCount;
            choices.error.clear();
            return;
        }
        // Use page indices captured on the GUI thread. A language change cannot
        // change the meaning of a localized range while this worker runs.
        const auto job = result->output;
        if (mervin::job::stopped())
            return;
        const PageOps::Status status = job.mode == mervin::ExtractPlan::Mode::Delete
            ? PageOps::deletePages(source.path, job.path, job.pages, choices.password, &choices.error)
            : PageOps::merge({PageOps::MergeInput{source.path, job.pages, choices.password}},
                             job.path, &choices.error);
        result->succeeded = status == PageOps::Status::Ok;
        if (!result->succeeded)
            choices.failure = status == PageOps::Status::NeedsPassword
                ? Dialog::Choices::Failure::Password : Dialog::Choices::Failure::Write;
    };
    operation.finished = [page = QPointer<TabPage>(tab), result] {
        if (page)
            queuePagesResult(page, result);
    };
    tab->startOperation(std::move(operation));
}

/// Routes completed page work through the surviving tab owner and close guards.
void MainWindow::queuePagesResult(TabPage *tab, const std::shared_ptr<PagesJob> &result)
{
    const QPointer<TabPage> page(tab);
    const QPointer<MainWindow> owner(qobject_cast<MainWindow *>(tab->window()));
    if (!owner)
        return;
    const auto discarded = connect(owner, &QObject::destroyed, page, [page, result] {
        if (page)
            QTimer::singleShot(0, page, [page, result] {
                if (page)
                    queuePagesResult(page, result);
            });
    });
    owner->whenTabsCanChange([page, owner, result, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || current->closeConfirmed_ || (current->wm_ && current->wm_->isClosing()))
            return;
        if (current != owner) {
            queuePagesResult(page, result);
            return;
        }
        if (page->isUsable())
            current->finishPagesJob(page, result);
    });
}

/// Opens accepted output or restores failed choices in the launching tab context.
void MainWindow::finishPagesJob(TabPage *tab, const std::shared_ptr<PagesJob> &result)
{
    const OperationScope operation(this);
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    if (result->writing && result->succeeded) {
        if (!result->source.probe.password.isEmpty())
            page->setPassword(result->source.probe.password);
        statusBar()->showMessage(mervin::ExtractPlan::doneText(result->output), 5000);
        if (result->choices->openWhenDone && !hiddenToTray_)
            openFile(result->output.path, false, -1, true,
                result->output.mode == mervin::ExtractPlan::Mode::Delete
                    ? result->source.probe.password : QString());
        return;
    }
    if (hiddenToTray_) {
        if (result->writing && wm_)
            wm_->notifyInTray(tr("Operation failed"),
                mervin::ExtractDialog::failureText(*result->choices, result->source.mode));
        return;
    }
    auto source = result->source;
    source.doc = page->viewer()->document();
    source.engine = engine_;
    source.hasUnsavedEdits = page->hasUnsavedEdits();
    source.openPaths = wm_ ? wm_->openTabPaths() : tabPaths();
    const QPointer<mervin::ExtractDialog> dialog =
        new mervin::ExtractDialog(source, this, result->choices);
    dialog->setWindowModality(Qt::WindowModal);
    const bool accepted = dialog->exec() == QDialog::Accepted;
    if (!self || !page || !dialog) {
        delete dialog.data();
        return;
    }
    if (!accepted) {
        delete dialog.data();
        return;
    }
    result->choices = dialog->choices();
    result->output = dialog->job();
    delete dialog.data();
    if (result->choices->openWhenDone != settings_.extractOpenWhenDone) {
        settings_.extractOpenWhenDone = result->choices->openWhenDone;
        settings_.save();
    }
    runPagesJob(page, result, true);
}

// Captures split choices and reserves every output before writing any file.
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
        // Copy tools must not replace a file open in a tab.
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

/// Captures the probed plan, output path and replacement consent independently of its dialog.
struct MainWindow::MergeRequest
{
    mervin::MergePlan plan;
    QString output;
    mervin::DestinationConsent consent;
};

/// Returns the worker outcome and the input or destination that blocked it.
struct MainWindow::MergeResult
{
    PageOps::Status status = PageOps::Status::Failed;
    QString error;
    int failedIndex = -1;
    mervin::MergeDialog::Conflict conflict = mervin::MergeDialog::Conflict::None;
};

/// Opens Merge for the current usable tab.
void MainWindow::mergeDocuments()
{
    TabPage *tab = currentTab();
    if (tab && tab->isUsable())
        showMergeDialog(tab);
}

/// Edits a fresh or failed plan in its launching tab and current owner.
void MainWindow::showMergeDialog(TabPage *tab, const std::shared_ptr<MergeRequest> &request,
                                 const std::shared_ptr<MergeResult> &result)
{
    const OperationScope operation(this);
    const QPointer<MainWindow> owner(this);
    const QPointer<TabPage> page(tab);
    if (!tab->isUsable() || closeConfirmed_ || (wm_ && wm_->isClosing()))
        return;
    if (hiddenToTray_) {
        if (result && wm_)
            wm_->notifyInTray(mervin::MergeDialog::tr("Merge PDFs"), mervin::MergeDialog::failureSummary(
                request->plan, request->output, result->failedIndex, result->conflict));
        return;
    }
    // Reopening the failed plan belongs to its launching tab, even if the user
    // selected another one while the write waited.
    recentActive_ = false;
    tabs_->setCurrentWidget(tab);
    if (!owner || !page || !page->isUsable() || page->window() != owner
        || closeConfirmed_ || (wm_ && wm_->isClosing()))
        return;
    updateForCurrentTab();
    if (!owner || !page || !page->isUsable() || page->window() != owner
        || closeConfirmed_ || (wm_ && wm_->isClosing()))
        return;
    const QPointer<mervin::MergeDialog> dialog(request
        ? new mervin::MergeDialog(request->plan, request->output, result->error,
                                  result->failedIndex, result->conflict, this)
        : new mervin::MergeDialog(tab->path(), tab->viewer()->pageCount(), tab->password(), this));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setOpenFiles(wm_ ? wm_->openTabPaths() : tabPaths());
    const bool accepted = dialog->exec() == QDialog::Accepted;
    std::shared_ptr<MergeRequest> next;
    if (dialog && accepted) {
        next = std::make_shared<MergeRequest>();
        next->plan = dialog->plan();
        next->output = dialog->outputPath();
        next->consent = dialog->outputConsent();
    }
    delete dialog.data();
    if (next && owner && page && page->isUsable())
        runMerge(page, next);
}

/// Checks identities, reserves output and revalidates replacement consent on the tab worker.
void MainWindow::runMerge(TabPage *tab, const std::shared_ptr<MergeRequest> &request)
{
    auto *owner = qobject_cast<MainWindow *>(tab->window());
    if (!owner || !tab->isUsable() || owner->closeConfirmed_
        || (owner->wm_ && owner->wm_->isClosing()))
        return;
    const auto open = openFileKeys(owner->wm_ ? owner->wm_->openTabPaths() : owner->tabPaths());
    const auto result = std::make_shared<MergeResult>();
    TabPage::Operation operation;
    operation.destination = QFileInfo(request->output).absolutePath();
    operation.work = [request, result, open] {
        if (!mervin::job::writing(QFileInfo(request->output).absolutePath()))
            return;
        const QString destination = mervin::fileKey(QFileInfo(request->output));
        for (const auto &entry : request->plan.entries()) {
            if (!mervin::job::reading(entry.path))
                return;
            if (mervin::fileKey(QFileInfo(entry.path)) == destination) {
                result->conflict = mervin::MergeDialog::Conflict::Input;
                return;
            }
        }
        if (open.contains(destination) || !mervin::job::reserveDestinations({destination})) {
            result->conflict = mervin::MergeDialog::Conflict::OpenTab;
            return;
        }
        QString consentError;
        result->status = PageOps::merge(request->plan.inputs(), request->output,
            &result->error, &result->failedIndex, [&] {
                const auto current = mervin::DestinationConsent::read(request->output, &consentError);
                if (!current) {
                    result->conflict = mervin::MergeDialog::Conflict::CheckFailed;
                    return false;
                }
                if (*current != request->consent) {
                    result->conflict = mervin::MergeDialog::Conflict::Changed;
                    return false;
                }
                return true;
            });
        if (!consentError.isEmpty())
            result->error = consentError;
        else if (result->conflict == mervin::MergeDialog::Conflict::Changed)
            result->error.clear();
    };
    operation.finished = [page = QPointer<TabPage>(tab), request, result] {
        if (page)
            deliverMergeDialog(page, request, result);
    };
    tab->startOperation(std::move(operation));
}

/// Queues the result for its surviving tab owner before reopening or offering output.
void MainWindow::deliverMergeDialog(TabPage *tab, const std::shared_ptr<MergeRequest> &request,
                                    const std::shared_ptr<MergeResult> &result)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window || window->closeConfirmed_ || (window->wm_ && window->wm_->isClosing()))
        return;
    // The old window drops its queue on destruction. A tab that moved away must
    // still receive the result in its surviving owner.
    const auto discarded = connect(window, &QObject::destroyed, tab, [page, request, result] {
        QTimer::singleShot(0, page, [page, request, result] {
            if (page)
                deliverMergeDialog(page, request, result);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), request, result,
                              discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current || current->closeConfirmed_ || (current->wm_ && current->wm_->isClosing()))
            return;
        if (current != owner) {
            deliverMergeDialog(page, request, result);
            return;
        }
        if (result->status == PageOps::Status::Ok) {
            if (!current->hiddenToTray_)
                current->offerToOpen(request->output);
        } else {
            current->showMergeDialog(page, request, result);
        }
    });
}

// Retains captured print choices and any prepared document across owner moves.
struct MainWindow::PrintRequest
{
    std::shared_ptr<mervin::DocumentLifetime> liveLifetime;
    std::shared_ptr<PreparedPrintDocument> prepared;
    int pageCount = 0;
    int currentPage = 0;
    int rotation = 0;
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
    QString baseName;
    QString sourceKey;
    QString error;
};

// Captures source choices and prepares measured print input before showing a dialog.
void MainWindow::printDocument()
{
    const OperationScope operation(this);
    TabPage *tab = currentTab();
    ViewerWidget *viewer = currentViewer();
    if (!tab || !tab->isUsable() || !viewer)
        return;
    const QPointer<MainWindow> owner(this);
    const QPointer<TabPage> page(tab);
    const QPointer<ViewerWidget> view(viewer);
    viewer->commitActiveFormEditor();
    if (!owner || !page || !view)
        return;
    viewer->commitActiveAnnotEditor();
    if (!owner || !page || !view || !page->isUsable() || !view->document())
        return;
    auto request = std::make_shared<PrintRequest>();
    request->liveLifetime = viewer->document()->lifetime();
    request->pageCount = viewer->pageCount();
    request->currentPage = viewer->currentPage();
    request->rotation = viewer->rotation();
    request->baseName = QFileInfo(tab->path()).completeBaseName();
    request->sourceKey = mervin::normalizePathKey(tab->canonicalPath());
    if (request->pageCount <= 0)
        return;
    QSizeF pageSize = viewer->document()->pageSize(request->currentPage);
    if (request->rotation == 90 || request->rotation == 270)
        pageSize.transpose();
    request->orientation = pageSize.width() > pageSize.height()
        ? QPageLayout::Landscape : QPageLayout::Portrait;
    if (!viewer->hasMeasurements()) {
        showPrintDialog(tab, request);
        return;
    }
    const auto measurements = collectMeasureDoc(viewer);
    const auto opener = engine_->opener();
    const QString password = tab->password();
    request->prepared = std::make_shared<PreparedPrintDocument>();
    TabPage::Operation prepare;
    prepare.destination = QDir::tempPath();
    prepare.work = [request, measurements, opener, password] {
        if (!mervin::job::writing(QDir::tempPath()))
            return;
        auto &prepared = *request->prepared;
        prepared.stage = std::make_unique<QTemporaryDir>();
        if (!prepared.stage->isValid()) {
            request->error = prepared.stage->errorString();
            return;
        }
        const QString live = prepared.stage->filePath(QStringLiteral("live.pdf"));
        const QString flat = prepared.stage->filePath(QStringLiteral("measured.pdf"));
        std::vector<mervin::RenderMeasurement> marks;
        {
            if (!mervin::job::beginHolding())
                return;
            const auto release = qScopeGuard([] { mervin::job::endHolding(); });
            const Document::Hold hold(request->liveLifetime, mervin::job::stopped);
            Document *document = hold.document();
            if (!document)
                return;
            marks = renderMeasurements(*document, measurements);
            if (!mervin::job::writing(prepared.stage->path())
                || !document->savePdfTo(live, &request->error))
                return;
        }
        if (mervin::job::stopped()
            || MeasureExport::flatten(live, flat, marks, password, &request->error)
                != MeasureExport::Status::Ok)
            return;
        prepared.document.reset(opener->open(flat, password, &request->error).release());
    };
    prepare.finished = [page = QPointer<TabPage>(tab), request] {
        if (page)
            deliverPrintDialog(page, request);
    };
    tab->startOperation(std::move(prepare));
}

// Queues the prepared print dialog in the surviving owner of its source tab.
void MainWindow::deliverPrintDialog(TabPage *tab, const std::shared_ptr<PrintRequest> &request)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    // The old owner discards its queue when its last tab moves away. Retain the
    // prepared document and deliver through the surviving tab after destruction.
    const auto discarded = connect(window, &QObject::destroyed, tab, [page, request] {
        QTimer::singleShot(0, page, [page, request] {
            if (page)
                deliverPrintDialog(page, request);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), request, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current)
            return;
        if (current != owner) {
            deliverPrintDialog(page, request);
            return;
        }
        current->showPrintDialog(page, request);
    });
}

// Collects printer choices on the GUI, then transfers all output calls to a tab worker.
void MainWindow::showPrintDialog(TabPage *tab, const std::shared_ptr<PrintRequest> &request)
{
    const OperationScope operation(this);
    const QPointer<MainWindow> owner(this);
    const QPointer<TabPage> page(tab);
    if (!tab->isUsable() || tab->viewer()->document()->lifetime() != request->liveLifetime)
        return;
    if (request->prepared && !request->prepared->document) {
        if (!hiddenToTray_)
            tellInWindow(this, QMessageBox::Warning, tr("Print"),
                         tr("Could not prepare the document:\n%1").arg(request->error));
        else if (wm_)
            wm_->notifyInTray(tr("Print"), tr("Could not prepare the document:\n%1").arg(request->error));
        return;
    }
    if (hiddenToTray_)
        return;
    Document *document = request->prepared ? request->prepared->document.get()
                                           : tab->viewer()->document();
    auto printer = std::make_unique<QPrinter>(QPrinter::HighResolution);
    printer->setPageOrientation(request->orientation);
    const QPointer<PrintDialog> dialog(new PrintDialog(
        printer.get(), engine_, document, request->rotation, request->orientation,
        request->currentPage + 1, request->baseName, this));
    const bool accepted = dialog->exec() == QDialog::Accepted;
    QList<int> pages;
    mervin::printing::Settings settings;
    const bool system = dialog && dialog->useSystemDialog();
    if (dialog) {
        pages = dialog->selectedPages();
        settings = dialog->printSettings();
    }
    delete dialog.data();
    if (!accepted || !owner || !page || !page->isUsable())
        return;
    if (system) {
        printer->setFromTo(1, request->pageCount);
        const QPointer<QPrintDialog> native(new QPrintDialog(printer.get(), this));
        native->setWindowTitle(tr("Print"));
        native->setOption(QAbstractPrintDialog::PrintPageRange, true);
        native->setOption(QAbstractPrintDialog::PrintCurrentPage, true);
        const bool accepted = native->exec() == QDialog::Accepted;
        delete native.data();
        if (!accepted || !owner || !page || !page->isUsable())
            return;
        settings = {};
        settings.colourMode = printer->colorMode() == QPrinter::GrayScale
            ? mervin::printing::ColourMode::Grayscale : mervin::printing::ColourMode::Colour;
        pages.clear();
        switch (printer->printRange()) {
        case QPrinter::PageRange: {
            const int first = qBound(1, printer->fromPage(), request->pageCount);
            const int last = qBound(first, printer->toPage(), request->pageCount);
            for (int p = first; p <= last; ++p)
                pages.append(p);
            break;
        }
        case QPrinter::CurrentPage:
            pages.append(request->currentPage + 1);
            break;
        default:
            for (int p = 1; p <= request->pageCount; ++p)
                pages.append(p);
            break;
        }
    }
    if (pages.isEmpty())
        return;
    // Transfers the configured printer to the worker that owns output and teardown.
    struct Work {
        std::unique_ptr<QPrinter> printer;
    };
    auto work = std::make_shared<Work>();
    work->printer = std::move(printer);
    const QString output = work->printer->outputFileName();
    const QString printerName = work->printer->printerName().isEmpty()
        ? QCoreApplication::translate("PrintDialog", "Print to file") : work->printer->printerName();
    QSet<QString> open = openFileKeys(wm_ ? wm_->openTabPaths() : tabPaths());
    open.insert(request->sourceKey);
    const auto lifetime = document->lifetime();
    auto result = std::make_shared<PrintResult>();
    TabPage::Operation print;
    print.destination = output.isEmpty() ? QString() : QFileInfo(output).absolutePath();
    print.work = [work, request, lifetime, pages, settings, printerName, output, open, result] {
        // Once output begins, every QPrinter call and its destruction stays here.
        auto printer = std::move(work->printer);
        std::unique_ptr<QTemporaryDir> stage;
        if (!output.isEmpty()) {
            if (!mervin::job::writing(QFileInfo(output).absolutePath()))
                return;
            const QString key = mervin::fileKey(QFileInfo(output));
            if (open.contains(key) || !mervin::job::reserveDestinations({key})) {
                result->failure = PrintResult::Failure::DestinationOpen;
                return;
            }
            if (!mervin::job::writing(QDir::tempPath()))
                return;
            stage = std::make_unique<QTemporaryDir>();
            if (!stage->isValid()) {
                result->failure = PrintResult::Failure::TemporaryFile;
                result->error = stage->errorString();
                return;
            }
            printer->setOutputFileName(stage->filePath(QStringLiteral("printed.pdf")));
        }
        printPages(*printer, lifetime, pages, request->rotation, settings, printerName,
                   !request->prepared, *result);
        // QPrinter can keep the PDF engine's file open until destruction on Windows.
        printer.reset();
        if (!output.isEmpty() && result->failure == PrintResult::Failure::None
            && !mervin::job::stopped()
            && !mervin::DocumentOutput::replace(stage->filePath(QStringLiteral("printed.pdf")),
                                               output, &result->error))
            result->failure = PrintResult::Failure::Write;
    };
    print.finished = [page, result] {
        if (!page || result->failure == PrintResult::Failure::None)
            return;
        QString message;
        switch (result->failure) {
        case PrintResult::Failure::None:
            return;
        case PrintResult::Failure::DestinationOpen:
            message = tr("The destination is open in a tab.");
            break;
        case PrintResult::Failure::TemporaryFile:
            message = tr("Could not prepare the document:\n%1").arg(result->error);
            break;
        case PrintResult::Failure::Write:
            message = tr("Could not save:\n%1").arg(result->error);
            break;
        case PrintResult::Failure::Start:
            message = tr("Could not start the print job.");
            break;
        case PrintResult::Failure::NextPage:
            message = tr("Could not start the next printed page.");
            break;
        case PrintResult::Failure::Render:
            message = tr("Could not render page %1.").arg(result->page);
            break;
        case PrintResult::Failure::End:
            //: The printer system rejected completion of the print job. It may
            //: have accepted earlier pages.
            message = tr("Could not finish the print job.");
            break;
        }
        deliverPrintFailure(page, message);
    };
    page->startOperation(std::move(print));
}

// Reports terminal printer failure through the current owner after queued tab changes.
void MainWindow::deliverPrintFailure(TabPage *tab, const QString &message)
{
    const QPointer<TabPage> page(tab);
    auto *window = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
    if (!window)
        return;
    const auto discarded = connect(window, &QObject::destroyed, tab, [page, message] {
        QTimer::singleShot(0, page, [page, message] {
            if (page)
                deliverPrintFailure(page, message);
        });
    });
    window->whenTabsCanChange([page, owner = QPointer<MainWindow>(window), message, discarded] {
        disconnect(discarded);
        auto *current = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!current)
            return;
        if (current != owner) {
            deliverPrintFailure(page, message);
            return;
        }
        //: Title of the print dialog and of messages about printing.
        const QString title = tr("Print");
        if (!current->hiddenToTray_)
            tellInWindow(current, QMessageBox::Warning, title, message);
        else if (current->wm_)
            current->wm_->notifyInTray(title, message);
    });
}

// ---- Detachable tabs (M9) --------------------------------------------------
