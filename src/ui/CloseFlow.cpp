#include "ui/CloseFlow.h"

#include "app/Relaunch.h"
#include "app/WindowManager.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace mervin {
namespace {

// Finds a tab's current MainWindow after any move.
MainWindow *owner(TabPage *tab)
{
    return tab ? qobject_cast<MainWindow *>(tab->window()) : nullptr;
}

// Identifies requests that cover every window in the process.
bool global(CloseFlow::Intent intent)
{
    return intent == CloseFlow::Intent::Quit || intent == CloseFlow::Intent::Restart;
}

} // namespace

CloseFlow::CloseFlow(WindowManager *manager, MainWindow *window, Intent intent,
                     TabPage *tab, bool remember)
    : QObject(manager ? static_cast<QObject *>(manager) : static_cast<QObject *>(window))
    , manager_(manager)
    , origin_(window)
    , intent_(intent)
    , remember_(remember)
{
    if (intent == Intent::Tab)
        add(tab);
    else if (intent == Intent::AllTabs && window) {
        for (int i = 0; i < window->tabs_->count(); ++i)
            add(qobject_cast<TabPage *>(window->tabs_->widget(i)));
    } else if (intent == Intent::Window || global(intent)) {
        for (const QPointer<MainWindow> &owner : windows())
            if (owner)
                for (int i = 0; i < owner->tabs_->count(); ++i)
                    add(qobject_cast<TabPage *>(owner->tabs_->widget(i)));
    }
}

CloseFlow::~CloseFlow()
{
    clearWait();
    releaseClaims();
    if (!ended_ && global(intent_) && manager_ && manager_->closeFlows_.contains(this))
        manager_->quitting_ = false;
}

bool CloseFlow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ParentChange)
        post(qobject_cast<TabPage *>(watched));
    return QObject::eventFilter(watched, event);
}

void CloseFlow::request(MainWindow *window, Intent intent, TabPage *tab, bool remember)
{
    if (window)
        start(window->wm_, window, intent, tab, remember);
}

void CloseFlow::requestQuit(WindowManager *manager, bool restart)
{
    if (manager)
        start(manager, nullptr, restart ? Intent::Restart : Intent::Quit);
}

// Coalesces compatible close requests and begins collecting their current tab owners.
CloseFlow *CloseFlow::start(WindowManager *manager, MainWindow *window, Intent intent,
                           TabPage *tab, bool remember)
{
    // Construct the scope before comparing requests. Claims are acquired only
    // after overlapping older requests have released theirs.
    auto *flow = new CloseFlow(manager, window, intent, tab, remember);
    auto &flows = manager ? manager->closeFlows_ : window->closeFlows_;
    for (const QPointer<CloseFlow> &existing : std::as_const(flows)) {
        if (!existing || existing->ended_)
            continue;
        if (existing->covers(*flow)) {
            if (intent == Intent::Restart && global(existing->intent_))
                existing->intent_ = Intent::Restart;
            delete flow;
            return existing;
        }
    }
    const auto previous = flows;
    for (const QPointer<CloseFlow> &existing : previous) {
        if (existing && !existing->ended_ && flow->covers(*existing)) {
            for (const Entry &item : std::as_const(existing->entries_))
                if (item.tab && flow->includes(item.tab))
                    if (Entry *next = flow->entry(item.tab)) {
                        next->accepted = item.accepted;
                        next->save = item.save;
                        next->saving = item.saving;
                    }
            existing->cancel();
        }
    }
    flows.removeIf([](const QPointer<CloseFlow> &item) { return !item || item->ended_; });
    flows.append(flow);
    if (intent == Intent::AllTabs)
        window->rememberOpenTabs();
    if (global(intent))
        manager->quitting_ = true;
    flow->collect();
    flow->post();
    return flow;
}

bool CloseFlow::covers(const CloseFlow &other) const
{
    if (global(intent_))
        return true;
    if (global(other.intent_) || intent_ == Intent::SaveOnly || other.intent_ == Intent::SaveOnly)
        return false;
    if (intent_ == Intent::Window)
        return other.intent_ == Intent::Tab
            ? !other.entries_.isEmpty() && owner(other.entries_.front().tab) == origin_
            : other.origin_ == origin_;
    if (intent_ == Intent::AllTabs) {
        if (other.intent_ == Intent::AllTabs)
            return other.origin_ == origin_;
        return other.intent_ == Intent::Tab && !other.entries_.isEmpty()
            && std::any_of(entries_.cbegin(), entries_.cend(), [&other](const Entry &item) {
                   return item.tab == other.entries_.front().tab;
               });
    }
    return intent_ == Intent::Tab && other.intent_ == Intent::Tab
        && !entries_.isEmpty() && !other.entries_.isEmpty()
        && entries_.front().tab == other.entries_.front().tab;
}

bool CloseFlow::includes(TabPage *tab) const
{
    if (!tab)
        return false;
    MainWindow *window = owner(tab);
    if (global(intent_))
        return window && manager_ && manager_->windows_.contains(window);
    if (intent_ == Intent::Window || intent_ == Intent::AllTabs)
        return window && window == origin_;
    return window != nullptr;
}

QList<QPointer<MainWindow>> CloseFlow::windows() const
{
    if (global(intent_) && manager_)
        return {manager_->windows_.cbegin(), manager_->windows_.cend()};
    return origin_ ? QList<QPointer<MainWindow>>{origin_} : QList<QPointer<MainWindow>>{};
}

void CloseFlow::add(TabPage *tab)
{
    if (tab && !entry(tab))
        entries_.append({tab, {}, false});
}

CloseFlow::Entry *CloseFlow::entry(TabPage *tab)
{
    for (Entry &item : entries_)
        if (item.tab == tab)
            return &item;
    return nullptr;
}

void CloseFlow::collect()
{
    if (intent_ == Intent::Window || global(intent_))
        for (const QPointer<MainWindow> &window : windows())
            if (window)
                for (int i = 0; i < window->tabs_->count(); ++i)
                    add(qobject_cast<TabPage *>(window->tabs_->widget(i)));
    for (Entry &item : entries_) {
        if (!item.tab)
            continue;
        if (includes(item.tab)) {
            if (item.tab->closeOwner_ && item.tab->closeOwner_ != this)
                item.tab->closeOwner_->cancel();
            item.tab->closeOwner_ = this;
            item.tab->installEventFilter(this);
        } else {
            if (item.tab->closeOwner_ == this) {
                item.tab->closeOwner_.clear();
                item.tab->removeEventFilter(this);
            }
            // Close all tabs snapshots its original bar and skips moved-out tabs.
            if (intent_ == Intent::AllTabs)
                item.tab.clear();
        }
    }
}

void CloseFlow::post(TabPage *tab)
{
    if (ended_ || posted_)
        return;
    posted_ = true;
    const QPointer<TabPage> page(tab);
    QTimer::singleShot(0, this, [this, page] {
        MainWindow *window = page ? owner(page) : origin_.data();
        if (!window && manager_ && !manager_->windows_.isEmpty())
            window = manager_->windows_.front();
        if (!window) {
            posted_ = false;
            advance(nullptr);
            return;
        }
        const QPointer<CloseFlow> self(this);
        const QPointer<MainWindow> queuedWindow(window);
        // That window owns its queue and drops it on destruction. Wake the
        // surviving flow so a tab moved elsewhere can still finish closing.
        const auto discarded = connect(window, &QObject::destroyed, this, [this, page] {
            posted_ = false;
            post(page);
        });
        window->whenTabsCanChange([self, page, queuedWindow, discarded] {
            disconnect(discarded);
            if (!self || self->ended_)
                return;
            self->posted_ = false;
            if (page && owner(page) != queuedWindow) {
                self->post(page);
                return;
            }
            self->advance(queuedWindow);
        });
    });
}

void CloseFlow::attention(TabPage *tab)
{
    const QPointer<MainWindow> window = owner(tab);
    const QPointer<TabPage> page(tab);
    if (!window)
        return;
    window->recentActive_ = false;
    window->tabs_->setCurrentWidget(tab);
    if (!window || !page)
        return;
    if (window->isInTray())
        window->restoreFromTray();
    else {
        if (window->isMinimized())
            window->showNormal();
        else
            window->show();
        window->raise();
        window->activateWindow();
    }
    if (window)
        window->updateForCurrentTab();
}

// Observes the active tab prerequisite and makes its Stop choice match the close stage.
void CloseFlow::wait(Entry &item, WaitReason reason)
{
    clearWait();
    waitingTab_ = item.tab;
    waitingWindow_ = owner(item.tab);
    waiting_ = reason;
    auto changed = [this] { post(waitingTab_); };
    waitConnections_.append(connect(item.tab, &TabPage::operationFinished, this,
                                    changed, Qt::QueuedConnection));
    waitConnections_.append(connect(item.tab, &TabPage::resumeFinished, this,
                                    changed, Qt::QueuedConnection));
    waitConnections_.append(connect(item.tab, &TabPage::stateChanged, this,
                                    changed, Qt::QueuedConnection));
    waitConnections_.append(connect(item.tab, &QObject::destroyed, this,
                                    changed, Qt::QueuedConnection));
    waitConnections_.append(connect(item.tab, &TabPage::waitingStopped, this, [this] {
        if (waiting_ == WaitReason::Operation) {
            clearWait();
            post();
        } else if (waiting_ == WaitReason::Save
                   || waiting_ == WaitReason::DocumentReleaseForSave) {
            cancel();
        }
    }));
    const QPointer<CloseFlow> self(this);
    const QPointer<TabPage> tab = item.tab;
    attention(tab);
    if (self && !ended_ && tab)
        tab->showWaitImmediately(reason == WaitReason::DocumentReleaseForSave);
}

// Continues close only after Saved, asking again if completion introduced another edit.
void CloseFlow::finishSave(TabPage *tab, TabPage::SaveResult result)
{
    if (ended_ || !tab || !includes(tab))
        return;
    Entry *item = entry(tab);
    if (!item || !item->saving)
        return;
    if (result != TabPage::SaveResult::Saved) {
        cancel();
        return;
    }
    item->save = false;
    item->saving = false;
    // A direct unfreeze observer can introduce another edit before completion.
    // Ask again rather than accepting changes this Save did not write.
    item->accepted = tab->hasUnsavedEdits() ? std::nullopt
        : std::optional<quint64>(tab->editGeneration());
    clearWait();
    post(tab);
}

void CloseFlow::clearWait()
{
    const QPointer<TabPage> tab = waitingTab_;
    for (const auto &connection : std::as_const(waitConnections_))
        disconnect(connection);
    waitConnections_.clear();
    waitingTab_.clear();
    waitingWindow_.clear();
    waiting_ = WaitReason::None;
    if (tab)
        tab->clearCloseWait();
}

// Processes current-owner decisions and asynchronous prerequisites before final close.
void CloseFlow::advance(MainWindow *readyWindow)
{
    if (ended_)
        return;
    if (advancing_) {
        advanceAgain_ = true;
        return;
    }
    advancing_ = true;
    const QPointer<CloseFlow> self(this);
    const auto clearAdvancing = qScopeGuard([self] {
        if (!self)
            return;
        self->advancing_ = false;
        if (std::exchange(self->advanceAgain_, false))
            self->post();
    });
    collect();
    if (waiting_ != WaitReason::None) {
        const QPointer<TabPage> tab = waitingTab_;
        if (tab && includes(tab)) {
            if (owner(tab) != waitingWindow_) {
                waitingWindow_ = owner(tab);
                attention(tab);
                if (!self || ended_)
                    return;
                if (!tab) {
                    post();
                    return;
                }
            }
            if ((waiting_ == WaitReason::Operation || waiting_ == WaitReason::DocumentReleaseForSave)
                && tab->isBusy())
                return;
            if (waiting_ == WaitReason::Save)
                if (const Entry *item = entry(tab); item && item->saving)
                    return;
        }
        clearWait();
    }

    for (Entry &item : entries_) {
        const QPointer<TabPage> tab = item.tab;
        if (!tab || !includes(tab))
            continue;
        if (tab->isUsable()) {
            const QPointer<CloseFlow> self(this);
            tab->viewer()->commitActiveFormEditor();
            if (!self || ended_ || !tab)
                return;
            tab->viewer()->commitActiveAnnotEditor();
            if (!self || ended_ || !tab)
                return;
        }
        const bool accepted = item.accepted && (!tab->isBusy() || tab->isOperationStopped())
            && (intent_ == Intent::SaveOnly || !tab->hasUnsavedEdits()
                || *item.accepted == tab->editGeneration());
        if (accepted && intent_ != Intent::Tab && intent_ != Intent::AllTabs)
            continue;
        MainWindow *window = owner(tab);
        if (window != readyWindow) {
            post(tab);
            return;
        }
        if (accepted) {
            const int index = window->tabs_->indexOf(tab);
            if (index >= 0) {
                window->saveTabViewState(tab);
                if (remember_ && intent_ == Intent::Tab)
                    window->rememberClosedTab(tab, index);
                window->removeTabAt(index);
            }
            post();
            return;
        }
        if (tab->isBusy() && !tab->isOperationStopped()) {
            wait(item, item.save ? WaitReason::DocumentReleaseForSave : WaitReason::Operation);
            return;
        }
        // A housekeeping read is abandoned without waiting. Save can take its
        // read back later, while Don't save may remove the unloaded tab now.
        if (tab->isLoading() && !item.save) {
            tab->abandonLoadForClose();
            if (!self || ended_)
                return;
            post(tab);
            return;
        }
        item.accepted.reset();
        if (!item.save && tab->hasUnsavedEdits()) {
            const QPointer<CloseFlow> self(this);
            attention(tab);
            if (!self || ended_ || !tab)
                return;
            if (owner(tab) != window) {
                post(tab);
                return;
            }
            const int answer = window->askClose(tab);
            if (!self || ended_) {
                // A broader request can supersede this flow while its question
                // runs. The user's answer still applies to the same tab.
                const QPointer<CloseFlow> replacement = tab ? tab->closeOwner_ : nullptr;
                if (replacement && replacement != self && !replacement->ended_
                    && replacement->includes(tab)) {
                    if (answer == QMessageBox::Cancel)
                        replacement->cancel();
                    else if (Entry *next = replacement->entry(tab)) {
                        next->save = answer == QMessageBox::Save;
                        next->accepted = next->save ? std::nullopt
                                                   : std::optional<quint64>(tab->editGeneration());
                        replacement->post(tab);
                    }
                }
                return;
            }
            if (answer == QMessageBox::Cancel) {
                cancel();
                return;
            }
            if (!tab || !includes(tab)) {
                post();
                return;
            }
            item.save = answer == QMessageBox::Save;
            if (!item.save)
                item.accepted = tab->editGeneration();
            // The question's event loop may have added tabs or windows. Collect
            // them before proceeding with this decision or final approval.
            post(tab);
            return;
        }
        if (item.save) {
            if (item.saving) {
                wait(item, WaitReason::Save);
                return;
            }
            if (tab->isBusy()) {
                wait(item, WaitReason::DocumentReleaseForSave);
                return;
            }
            const QPointer<CloseFlow> self(this);
            item.saving = true;
            wait(item, WaitReason::Save);
            if (!self || ended_ || !tab)
                return;
            // A broader request can take ownership while Save runs. Its explicit
            // result follows the tab's current close owner, rather than this flow.
            window->saveTab(tab, [tab](TabPage::SaveResult result) {
                if (tab && tab->closeOwner_)
                    tab->closeOwner_->finishSave(tab, result);
            });
            if (self && !ended_ && tab && tab->isBusy())
                tab->showWaitImmediately();
            return;
        }
        item.accepted = tab->editGeneration();
        if (intent_ == Intent::Tab || intent_ == Intent::AllTabs) {
            // The prompt may have moved this identity. Queue again in its owner
            // before removing it, after that window's earlier tab changes.
            post(tab);
            return;
        }
    }

    if (intent_ == Intent::Window || global(intent_)) {
        collect();
        for (const Entry &item : std::as_const(entries_)) {
            if (item.tab && includes(item.tab)
                && (!item.accepted || (item.tab->isBusy() && !item.tab->isOperationStopped())
                    || (item.tab->hasUnsavedEdits()
                        && *item.accepted != item.tab->editGeneration()))) {
                post(item.tab);
                return;
            }
        }
        // Run after earlier queued changes in every window. The next turn
        // collects arrivals and commits editors again before final approval.
        for (const QPointer<MainWindow> &window : windows()) {
            if (window && (!window->canChangeTabs() || !window->tabChanges_.empty())) {
                const QPointer<CloseFlow> self(this);
                window->whenTabsCanChange([self, window] {
                    if (self)
                        self->advance(window);
                });
                return;
            }
        }
    }
    finish();
}

void CloseFlow::releaseClaims()
{
    // A close question can select an unloaded tab while its claim blocks resume.
    // Clear all claims before scheduling each affected window's current tab.
    QList<QPointer<MainWindow>> owners;
    for (Entry &item : entries_)
        if (item.tab && item.tab->closeOwner_ == this) {
            item.tab->closeOwner_.clear();
            item.tab->removeEventFilter(this);
            if (MainWindow *window = owner(item.tab); window && !owners.contains(window))
                owners.append(window);
        }
    for (const QPointer<MainWindow> &window : owners)
        if (window)
            window->scheduleCurrentResume();
}

void CloseFlow::cancel()
{
    if (ended_)
        return;
    ended_ = true;
    clearWait();
    releaseClaims();
    if (global(intent_) && manager_)
        manager_->quitting_ = false;
    deleteLater();
}

void CloseFlow::finish()
{
    ended_ = true;
    clearWait();
    releaseClaims();
    const auto targets = windows();
    const QPointer<WindowManager> manager = manager_;
    const Intent intent = intent_;
    deleteLater();
    if (global(intent) && manager) {
        manager->updateSession();
        manager->sessionFinal_ = true;
        if (intent == Intent::Restart)
            relaunch::request();
        for (const QPointer<MainWindow> &window : targets)
            if (window)
                window->closeForQuit();
        if (targets.isEmpty())
            QCoreApplication::quit();
    } else if (intent == Intent::Window) {
        if (!targets.isEmpty() && targets.front())
            targets.front()->closeForQuit();
    }
}

bool CloseFlow::tryCloseForUpdate(WindowManager *manager)
{
    const QPointer<WindowManager> liveManager(manager);
    const auto pending = [liveManager] {
        if (!liveManager || liveManager->quitting_)
            return true;
        for (const QPointer<CloseFlow> &flow : std::as_const(liveManager->closeFlows_))
            if (flow && !flow->ended_)
                return true;
        return false;
    };
    if (pending())
        return false;
    QList<Entry> decisions;
    QList<QPointer<TabPage>> saves;
    // A Save already chosen stays requested even if a later answer or a newly
    // started operation makes this synchronous shutdown attempt fail.
    const auto queueSaves = qScopeGuard([&] {
        if (!liveManager || saves.isEmpty())
            return;
        CloseFlow *flow = nullptr;
        for (const QPointer<TabPage> &tab : saves) {
            if (!tab)
                continue;
            if (tab->closeOwner_) {
                auto *closing = tab->closeOwner_.data();
                if (Entry *item = closing->entry(tab); item && !closing->ended_) {
                    item->save = true;
                    item->accepted.reset();
                    if (closing->waitingTab_ == tab && closing->waiting_ == WaitReason::Operation)
                        closing->waiting_ = WaitReason::DocumentReleaseForSave;
                    closing->post(tab);
                    continue;
                }
            }
            if (!flow) {
                flow = new CloseFlow(liveManager, nullptr, Intent::SaveOnly, nullptr, false);
                liveManager->closeFlows_.append(flow);
            }
            flow->add(tab);
            flow->entry(tab)->save = true;
        }
        if (flow) {
            flow->collect();
            flow->post();
        }
    });
    bool approved = true;
    for (;;) {
        // A window-modal question can start Quit or Restart in another window.
        // Its posted continuation may not have joined a window queue yet.
        if (pending())
            return false;
        QList<QPointer<MainWindow>> targets(manager->windows_.cbegin(), manager->windows_.cend());
        for (const QPointer<MainWindow> &window : targets) {
            if (!window || !window->canChangeTabs() || !window->tabChanges_.empty())
                return false;
            for (int i = 0; i < window->tabs_->count(); ++i)
                if (auto *tab = qobject_cast<TabPage *>(window->tabs_->widget(i)); tab && tab->isBusy())
                    return false;
        }
        QPointer<TabPage> next;
        for (const QPointer<MainWindow> &window : targets) {
            if (!window)
                continue;
            for (int i = 0; window && i < window->tabs_->count(); ++i) {
                const QPointer<TabPage> tab = qobject_cast<TabPage *>(window->tabs_->widget(i));
                if (!tab)
                    continue;
                if (tab->isUsable()) {
                    tab->viewer()->commitActiveFormEditor();
                    if (!liveManager)
                        return false;
                    if (!window || !tab)
                        continue;
                    tab->viewer()->commitActiveAnnotEditor();
                    if (!liveManager)
                        return false;
                    if (!window || !tab)
                        continue;
                }
                bool accepted = !tab->hasUnsavedEdits();
                for (const Entry &item : std::as_const(decisions))
                    if (item.tab == tab && item.accepted == tab->editGeneration())
                        accepted = true;
                if (!accepted) {
                    next = tab;
                    break;
                }
            }
            if (next)
                break;
        }
        if (!next)
            break;
        MainWindow *window = owner(next);
        const int answer = window->askClose(next);
        if (!liveManager)
            return false;
        if (answer == QMessageBox::Cancel) {
            approved = false;
            break;
        }
        if (next) {
            decisions.append({next, next->editGeneration(), false});
            if (answer == QMessageBox::Save && !saves.contains(next))
                saves.append(next);
        }
    }
    if (!saves.isEmpty())
        return false;
    if (!approved)
        return false;
    if (pending())
        return false;
    const QList<QPointer<MainWindow>> targets(manager->windows_.cbegin(), manager->windows_.cend());
    manager->quitting_ = true;
    manager->updateSession();
    manager->sessionFinal_ = true;
    for (const QPointer<MainWindow> &window : targets)
        if (window)
            window->closeForQuit();
    if (targets.isEmpty())
        QCoreApplication::quit();
    return true;
}

} // namespace mervin
