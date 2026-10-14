#include "ui/MainWindow.h"

#include "app/WindowManager.h"
#include "render/ContentSearch.h"
#include "ui/CommentsSidebar.h"
#include "ui/DetachableTabBar.h"
#include "ui/OutlineSidebar.h"
#include "ui/TabPage.h"
#include "ui/ThumbnailSidebar.h"

#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QHideEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

#include <utility>

using mervin::TabPage;

void MainWindow::wireTabLifecycle(TabPage *tab)
{
    if (tabConnections_.contains(tab))
        return;
    auto &connections = tabConnections_[tab];
    connections.append(connect(tab, &TabPage::stateChanged, this, [this, tab] {
        if (currentTab() == tab && !suspending_)
            updateForCurrentTab();
    }));
    connections.append(connect(tab, &TabPage::freshLoadReady, this,
        [this, tab](const QString &canonical, quint64 generation) {
            queueFreshLoad(tab, canonical, generation);
        }));
    connections.append(connect(tab, &TabPage::freshOpenStopped, this,
        [this, tab](quint64 generation) { queueFreshRemoval(tab, generation); }));
    connections.append(connect(tab, &TabPage::resumeFinished, this,
        [this, tab](bool success, const QString &, bool needsPassword) {
            if (closeConfirmed_ || (wm_ && wm_->isClosing()))
                return;
            if (success) {
                if (currentTab() == tab)
                    updateForCurrentTab();
                if (hiddenToTray_)
                    QTimer::singleShot(0, this, [this] { suspendHiddenDocuments(); });
            } else if (needsPassword) {
                queueLoadPassword(tab, tab->loadGeneration());
            } else if (tab->freshOpen()) {
                if (tab->freshOpen()->closeOnFailure && !tab->hasRecoverySnapshot())
                    queueFreshRemoval(tab, tab->loadGeneration());
            } else if (wm_) {
                wm_->closeRemovedTabs();
            }
        }));
    connections.append(connect(tab, &TabPage::unloadFinished, this,
        [this, tab](bool unloaded, const QString &error) {
            const QPointer<MainWindow> self(this);
            const QPointer<TabPage> page(tab);
            if (!page)
                return;
            if (!error.isEmpty()) {
                if (auto *owner = qobject_cast<MainWindow *>(page->window()))
                    owner->reportUnloadFailure(page, error);
            }
            if (self && unloaded && wm_)
                wm_->checkDocumentActivity();
        }));
    connections.append(connect(tab, &QObject::destroyed, this, [this, tab] {
        inactiveSince_.remove(tab);
        suspensionFailed_.remove(tab);
        tabConnections_.remove(tab);
    }));
}

// Late load actions follow the tab even when its previous owner is destroyed
// with an undelivered queue. The tab owns the destruction connection's lifetime.
void MainWindow::queueFreshLoad(TabPage *tab, const QString &canonical, quint64 generation)
{
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    const auto discarded = connect(this, &QObject::destroyed, tab, [page, canonical, generation] {
        QTimer::singleShot(0, page, [page, canonical, generation] {
            if (auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr)
                owner->queueFreshLoad(page, canonical, generation);
        });
    });
    whenTabsCanChange([self, page, canonical, generation, discarded] {
        disconnect(discarded);
        auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!self || !owner || owner->closeConfirmed_ || (owner->wm_ && owner->wm_->isClosing()))
            return;
        if (owner != self) {
            owner->queueFreshLoad(page, canonical, generation);
            return;
        }
        if (page->loadGeneration() != generation || !page->freshOpen() || !page->isLoading())
            return;
        const TabPage::FreshOpen options = *page->freshOpen();
        if (!canonical.isEmpty() && !page->claimPath(canonical)) {
            page->rejectFreshLoad(generation);
            return;
        }
        if (!canonical.isEmpty() && !options.allowDuplicate && !page->hasRecoverySnapshot()
            && (owner->wm_ ? owner->wm_->isOpenAnywhere(canonical, page)
                           : owner->indexOfPath(canonical, page) >= 0)) {
            const bool focus = owner->currentTab() == page && !owner->recentActive_;
            owner->removeTabAt(owner->tabs_->indexOf(page));
            if (!self || !focus)
                return;
            if (owner->wm_)
                owner->wm_->focusExistingTab(canonical);
            else
                owner->focusTabIfOpen(canonical);
            return;
        }
        if (!page->acceptFreshLoad(generation) || !self || !page)
            return;
        const int index = owner->tabs_->indexOf(page);
        if (index < 0)
            return;
        owner->tabs_->setTabText(index, page->tabTitle());
        owner->tabs_->setTabToolTip(index, QDir::toNativeSeparators(page->path()));
        owner->syncDocTabBar();
        if (!page->hasRecoverySnapshot())
            owner->applySettingsToViewer(page->viewer());
        if (!self || !page)
            return;
        if (options.view && !page->hasRecoverySnapshot())
            page->setSavedViewState(*options.view);
        if (!self || !page)
            return;
        if (owner->wm_) {
            owner->wm_->recordOpen(page->canonicalPath(), /*restoreViewState=*/!options.allowDuplicate && !page->hasRecoverySnapshot(),
                                   page->viewer()->pageCount());
            if (self)
                owner->wm_->updateSession();
        }
    });
}

void MainWindow::queueFreshRemoval(TabPage *tab, quint64 generation)
{
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    const auto discarded = connect(this, &QObject::destroyed, tab, [page, generation] {
        QTimer::singleShot(0, page, [page, generation] {
            if (auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr)
                owner->queueFreshRemoval(page, generation);
        });
    });
    whenTabsCanChange([self, page, generation, discarded] {
        disconnect(discarded);
        auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!self || !owner || owner->closeConfirmed_ || (owner->wm_ && owner->wm_->isClosing()))
            return;
        if (owner != self) {
            owner->queueFreshRemoval(page, generation);
            return;
        }
        if (!page->freshOpen() || page->loadGeneration() != generation || page->hasRecoverySnapshot())
            return;
        const bool closeEmpty = page->freshOpen()->closeOnFailure;
        const int index = owner->tabs_->indexOf(page);
        if (index < 0)
            return;
        owner->removeTabAt(index);
        if (self && closeEmpty && owner->tabCount() == 0)
            owner->closeForQuit();
    });
}

void MainWindow::queueLoadPassword(TabPage *tab, quint64 generation)
{
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    const auto discarded = connect(this, &QObject::destroyed, tab, [page, generation] {
        QTimer::singleShot(0, page, [page, generation] {
            if (auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr)
                owner->queueLoadPassword(page, generation);
        });
    });
    whenTabsCanChange([self, page, generation, discarded] {
        disconnect(discarded);
        auto *owner = page ? qobject_cast<MainWindow *>(page->window()) : nullptr;
        if (!self || !owner || owner->closeConfirmed_ || (owner->wm_ && owner->wm_->isClosing()))
            return;
        if (owner != self) {
            owner->queueLoadPassword(page, generation);
            return;
        }
        if (page->loadGeneration() != generation || page->isLoading() || page->isLoaded()
            || owner->currentTab() != page || owner->recentActive_ || owner->hiddenToTray_
            || !owner->isVisible() || owner->isMinimized())
            return;
        // Quit from another window can destroy this owner during the nested loop.
        const QPointer<QInputDialog> prompt = new QInputDialog(owner);
        prompt->setObjectName(QStringLiteral("documentPassword"));
        prompt->setWindowModality(Qt::WindowModal);
        prompt->setWindowTitle(tr("Password required"));
        //: %1 is the name of the document being opened or resumed. This asks for
        //: the password needed to open its contents, not its owner password.
        prompt->setLabelText(tr("Enter the password for %1.").arg(page->tabTitle()));
        prompt->setTextEchoMode(QLineEdit::Password);
        const bool accepted = prompt->exec() == QDialog::Accepted;
        const QString password = accepted && prompt ? prompt->textValue() : QString();
        delete prompt.data();
        if (!self || !page || page->loadGeneration() != generation)
            return;
        if (accepted) {
            page->setPassword(password);
            page->retryLoad();
        } else if (page->freshOpen() && page->freshOpen()->closeOnFailure) {
            owner->queueFreshRemoval(page, generation);
        }
    });
}

void MainWindow::unwireTabLifecycle(TabPage *tab)
{
    for (const auto &connection : tabConnections_.take(tab))
        disconnect(connection);
    inactiveSince_.remove(tab);
    suspensionFailed_.remove(tab);
}

void MainWindow::scheduleCurrentResume()
{
    const QPointer<MainWindow> self(this);
    if (!closeConfirmed_ && !hiddenToTray_ && !recentActive_ && isVisible() && !isMinimized()) {
        if (auto *tab = currentTab(); tab && !tab->isClosing())
            tab->selectForUse();
        if (!self)
            return;
    }
    if (resumeScheduled_)
        return;
    resumeScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        resumeScheduled_ = false;
        if (closeConfirmed_ || hiddenToTray_ || recentActive_ || !isVisible() || isMinimized())
            return;
        const QPointer<MainWindow> self(this);
        const QPointer<TabPage> tab(currentTab());
        if (!tab || tab->isClosing())
            return;
        inactiveSince_.remove(tab);
        suspensionFailed_.remove(tab);
        if (!self || !tab || tab->window() != this)
            return;
        if (tab->isSuspended())
            tab->resumeAsync();
        if (self && wm_)
            wm_->checkDocumentActivity();
    });
}

bool MainWindow::canSuspendDocuments() const
{
    // Nested dialogs and document operations may hold raw document pointers. Defer until
    // their event loops return, including native print and save dialogs.
    return !closeConfirmed_ && !suspending_ && documentOperationDepth_ == 0
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
}

bool MainWindow::canChangeTabs() const
{
    // A held button can be an in-bar tab move, and a tab drag can end on the bar
    // of any window, so a drag from any window counts.
    return canSuspendDocuments() && QGuiApplication::mouseButtons() == Qt::NoButton
        && !mervin::DetachableTabBar::dragInProgress();
}

void MainWindow::whenTabsCanChange(std::function<void()> change)
{
    tabChanges_.push_back(std::move(change));
    postTabChanges();
}

void MainWindow::postTabChanges()
{
    if (tabChangesPosted_ || tabChanges_.empty())
        return;
    tabChangesPosted_ = true;
    QTimer::singleShot(0, this, [this] {
        tabChangesPosted_ = false;
        runTabChanges();
    });
}

void MainWindow::runTabChanges()
{
    // A change can show a box, whose event loop can call this again. The outer
    // call goes on once that change returns, so the order holds.
    if (runningTabChanges_)
        return;
    const QPointer<MainWindow> self(this);
    while (!tabChanges_.empty() && canChangeTabs()) {
        const std::function<void()> change = std::move(tabChanges_.front());
        tabChanges_.pop_front();
        runningTabChanges_ = true;
        change();
        if (!self)
            return; // the change deleted this window
        runningTabChanges_ = false;
    }
}

MainWindow::OperationScope::OperationScope(MainWindow *window)
    : window_(window)
{
    ++window->documentOperationDepth_;
}

MainWindow::OperationScope::~OperationScope()
{
    // Posted, so the code that ran the operation finishes before a tab moves.
    if (window_ && --window_->documentOperationDepth_ == 0)
        window_->postTabChanges();
}

void MainWindow::clearDocumentSidebars()
{
    sidebarDoc_ = nullptr;
    if (thumbnailSidebar_)
        thumbnailSidebar_->setDocument(nullptr);
    if (outlineSidebar_)
        outlineSidebar_->setOutline({});
    if (commentsSidebar_)
        commentsSidebar_->setAnnotations({});
}

bool MainWindow::suspendTab(TabPage *tab)
{
    // A busy tab keeps its document until its operation ends.
    if (tab->isClosing() || !tab->isUsable() || suspensionFailed_.contains(tab))
        return false;
    if (sidebarDoc_ == tab->viewer()->document())
        clearDocumentSidebars();
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    const bool previous = std::exchange(suspending_, true);
    const auto transition = qScopeGuard([self, previous] {
        if (self)
            self->suspending_ = previous;
    });
    QString error;
    const bool accepted = tab->suspend(&error);
    if (!self || !page)
        return accepted;
    if (!accepted) {
        if (!error.isEmpty())
            reportUnloadFailure(tab, error);
        return false;
    }
    return true;
}

void MainWindow::reportUnloadFailure(TabPage *tab, const QString &error)
{
    const QPointer<MainWindow> self(this);
    const QPointer<TabPage> page(tab);
    if (!page || page->window() != this || closeConfirmed_ || error.isEmpty())
        return;
    suspensionFailed_.insert(page);
    if (currentTab() == page && page->isUsable()) {
        updateSidebars();
        if (!self || !page)
            return;
        refreshCommentsSidebar();
    }
    if (!self || !page)
        return;
    //: Shown when an inactive document could not be unloaded from memory.
    //: %1 is the document name, %2 the reason.
    const QString message = tr("%1 remains loaded. %2").arg(page->tabTitle(), error);
    statusBar()->showMessage(message);
    if (self && wm_ && hiddenToTray_)
        wm_->notifySuspensionFailure(message);
}

bool MainWindow::updateDocumentActivity(qint64 nowMs, int timeoutMinutes)
{
    const QPointer<MainWindow> self(this);
    bool released = false;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *tab = qobject_cast<TabPage *>(tabs_->widget(i));
        if (!tab)
            continue;
        const bool shown = tab == currentTab() && !recentActive_ && !hiddenToTray_
            && isVisible() && !isMinimized();
        if (shown) {
            inactiveSince_.remove(tab);
            suspensionFailed_.remove(tab);
            continue;
        }
        if (!inactiveSince_.contains(tab))
            inactiveSince_.insert(tab, nowMs);
        if (timeoutMinutes <= 0 || !canSuspendDocuments())
            continue;
        if (hiddenToTray_ || nowMs - inactiveSince_.value(tab) >= qint64(timeoutMinutes) * 60000) {
            released = suspendTab(tab) || released;
            if (!self)
                return released;
        }
    }
    return released;
}

bool MainWindow::suspendHiddenDocuments()
{
    const QPointer<MainWindow> self(this);
    if (settings_.unloadInactiveMinutes == 0 || !canSuspendDocuments())
        return false;
    bool released = false;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *tab = qobject_cast<TabPage *>(tabs_->widget(i));
        if (tab && (hiddenToTray_ || !isVisible() || isMinimized() || recentActive_ || tab != currentTab())) {
            released = suspendTab(tab) || released;
            if (!self)
                return released;
        }
    }
    return released;
}

bool MainWindow::hasResidentDocuments() const
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *tab = qobject_cast<TabPage *>(tabs_->widget(i)); tab && (tab->isLoaded() || tab->isLoading()))
            return true;
    return false;
}

QList<TabPage *> MainWindow::unloadedTabs() const
{
    QList<TabPage *> unloaded;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *tab = qobject_cast<TabPage *>(tabs_->widget(i)); tab && tab->isSuspended())
            unloaded << tab;
    return unloaded;
}

bool MainWindow::hasLoadingDocuments() const
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *tab = qobject_cast<TabPage *>(tabs_->widget(i)); tab && tab->isLoading())
            return true;
    return false;
}

void MainWindow::applyMemorySettings(int minutes, bool closeToTray)
{
    settings_.unloadInactiveMinutes = minutes;
    settings_.closeToTray = closeToTray;
    inactiveSince_.clear();
    suspensionFailed_.clear();
    if (hiddenToTray_) {
        if (!closeToTray)
            restoreFromTray();
        else
            suspendHiddenDocuments();
    }
}

void MainWindow::persistWindowState()
{
    saveAllViewStates();
    if (wm_)
        wm_->updateSession();
    if (!isFullScreen()) {
        settings_.windowGeometry = saveGeometry();
        settings_.windowState = saveState();
    }
    settings_.save();
}

void MainWindow::hideToTray()
{
    if (hiddenToTray_)
        return;
    persistWindowState();
    beforeTrayState_ = windowState() & ~Qt::WindowMinimized;
    hiddenToTray_ = true;
    static quint64 lastTrayOrder = 0;
    trayOrder_ = ++lastTrayOrder;
    if (contentSearch_)
        contentSearch_->cancel();
    hide();
    // Let the window disappear before a large edited document is checkpointed.
    QTimer::singleShot(0, this, [this] {
        if (!hiddenToTray_)
            return;
        suspendHiddenDocuments();
        if (wm_)
            wm_->checkDocumentActivity();
    });
}

void MainWindow::restoreFromTray()
{
    hiddenToTray_ = false;
    setWindowState(beforeTrayState_ & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    scheduleCurrentResume();
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    if (wm_)
        QTimer::singleShot(0, this, [this] { wm_->checkDocumentActivity(); });
}

void MainWindow::closeForQuit()
{
    closeConfirmed_ = true;
    close();
}
