#include "ui/MainWindow.h"

#include "app/WindowManager.h"
#include "render/ContentSearch.h"
#include "ui/CommentsSidebar.h"
#include "ui/DetachableTabBar.h"
#include "ui/OutlineSidebar.h"
#include "ui/TabPage.h"
#include "ui/ThumbnailSidebar.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHideEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

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
    connections.append(connect(tab, &TabPage::resumeFinished, this,
        [this, tab](bool success, const QString &, bool needsPassword) {
            if (success) {
                if (currentTab() == tab)
                    updateForCurrentTab();
                if (hiddenToTray_)
                    QTimer::singleShot(0, this, [this] { suspendHiddenDocuments(); });
                return;
            }
            // A file that is gone takes its tab with it. The check runs on a worker
            // and closes the tab later, never inside this signal from the tab.
            if (!needsPassword) {
                if (wm_)
                    wm_->closeRemovedTabs();
                return;
            }
            if (currentTab() != tab || !isVisible() || isMinimized())
                return;
            bool accepted = false;
            //: %1 is the name of the document.
            const QString prompt = tr("Enter the password for %1.").arg(tab->tabTitle());
            // The prompt runs its own event loop, in which the tab can go away.
            const QPointer<TabPage> page(tab);
            const QString password = QInputDialog::getText(
                this, tr("Password required"), prompt, QLineEdit::Password, {}, &accepted);
            if (accepted && page) {
                page->setPassword(password);
                page->resumeAsync();
            }
        }));
    connections.append(connect(tab, &QObject::destroyed, this, [this, tab] {
        inactiveSince_.remove(tab);
        suspensionFailed_.remove(tab);
        tabConnections_.remove(tab);
    }));
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
    if (resumeScheduled_)
        return;
    resumeScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        resumeScheduled_ = false;
        if (closeConfirmed_ || hiddenToTray_ || recentActive_ || !isVisible() || isMinimized())
            return;
        TabPage *tab = currentTab();
        if (!tab)
            return;
        inactiveSince_.remove(tab);
        suspensionFailed_.remove(tab);
        if (tab->isSuspended())
            tab->resumeAsync();
        if (wm_)
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
    if (!tab->isLoaded() || suspensionFailed_.contains(tab))
        return false;
    if (sidebarDoc_ == tab->viewer()->document())
        clearDocumentSidebars();
    QScopedValueRollback<bool> transition(suspending_, true);
    QString error;
    if (!tab->suspend(&error)) {
        suspensionFailed_.insert(tab);
        if (currentTab() == tab && tab->isLoaded()) {
            updateSidebars();
            refreshCommentsSidebar();
        }
        if (!error.isEmpty()) {
            //: Shown when an inactive document could not be unloaded from memory.
            //: %1 is the document name, %2 the reason.
            const QString message = tr("%1 remains loaded. %2").arg(tab->tabTitle(), error);
            statusBar()->showMessage(message);
            if (wm_ && hiddenToTray_)
                wm_->notifySuspensionFailure(message);
        }
        return false;
    }
    return true;
}

bool MainWindow::updateDocumentActivity(qint64 nowMs, int timeoutMinutes)
{
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
        if (hiddenToTray_ || nowMs - inactiveSince_.value(tab) >= qint64(timeoutMinutes) * 60000)
            released = suspendTab(tab) || released;
    }
    return released;
}

bool MainWindow::suspendHiddenDocuments()
{
    if (settings_.unloadInactiveMinutes == 0 || !canSuspendDocuments())
        return false;
    bool released = false;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *tab = qobject_cast<TabPage *>(tabs_->widget(i));
        if (tab && (hiddenToTray_ || !isVisible() || isMinimized() || recentActive_ || tab != currentTab()))
            released = suspendTab(tab) || released;
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

bool MainWindow::prepareToClose()
{
    const OperationScope operation(this);
    // Ask about each tab once, in tab order, and find the next one again after each
    // question. A tab can arrive during a question, from a session restore or another
    // instance, and move the others along. It is asked about too.
    QList<QPointer<TabPage>> asked;
    for (;;) {
        TabPage *next = nullptr;
        for (int i = 0; i < tabs_->count() && !next; ++i)
            if (auto *tab = qobject_cast<TabPage *>(tabs_->widget(i)); tab && !asked.contains(tab))
                next = tab;
        if (!next)
            return true;
        asked.append(next);
        if (!confirmClose(next))
            return false;
    }
}

void MainWindow::closeForQuit()
{
    closeConfirmed_ = true;
    close();
}
