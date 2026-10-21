#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include "ui/TabPage.h"
#include "update/Updater.h"

#include <optional>

class MainWindow;

namespace mervin {

class TabPage;
class WindowManager;

// Resolves a close request through the event loop. It never waits for a document
// worker and changes tabs only through their current window's tab-change queue.
class CloseFlow : public QObject
{
public:
    enum class Intent { Tab, AllTabs, Window, Quit, Restart, SaveOnly };

    // Starts or joins a close request in the window without blocking its event loop.
    static void request(MainWindow *window, Intent intent, TabPage *tab = nullptr,
                        bool remember = true);
    // Requests process-wide close approval, optionally followed by relaunch.
    static void requestQuit(WindowManager *manager, bool restart = false);
    // The updater needs a synchronous answer. Chosen saves run later without
    // closing windows, and answer Busy. Only Closed closes the windows; the other
    // answers never schedule a later shutdown. The Settings dialog may stay open,
    // see MainWindow::canCloseForUpdate.
    static Updater::CloseResult tryCloseForUpdate(WindowManager *manager);

    // Disconnects waiting signals and releases tab claims when the flow is destroyed.
    ~CloseFlow() override;

protected:
    // Requeues a claimed tab when moving it changes the window that owns its queue.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Tracks a retained close decision and active Save for one guarded tab.
    struct Entry
    {
        QPointer<TabPage> tab;
        std::optional<quint64> accepted;
        bool save = false;
        bool saving = false;
    };
    enum class WaitReason { None, Operation, DocumentReleaseForSave, Save };

    // Captures the initial close scope before acquiring any tab claims.
    CloseFlow(WindowManager *manager, MainWindow *window, Intent intent,
              TabPage *tab, bool remember);
    // Coalesces overlapping requests and transfers decisions from superseded flows.
    static CloseFlow *start(WindowManager *manager, MainWindow *window, Intent intent,
                            TabPage *tab = nullptr, bool remember = true);
    // Determines whether this request includes the other request's close scope.
    bool covers(const CloseFlow &other) const;
    // Checks whether a tab still belongs to this flow after any window move.
    bool includes(TabPage *tab) const;
    // Returns the surviving windows covered by this request.
    QList<QPointer<MainWindow>> windows() const;
    // Adds newly covered tabs and refreshes claims after window or tab membership changes.
    void collect();
    // Records a tab once, preserving decisions already attached to its identity.
    void add(TabPage *tab);
    // Finds the decision record for a tab identity.
    Entry *entry(TabPage *tab);
    // Schedules progression through the tab's current owner queue and retries discarded queues.
    void post(TabPage *tab = nullptr);
    // Resolves one close step after the owning window permits tab changes.
    void advance(MainWindow *readyWindow);
    // Observes the operation or Save prerequisite and immediately shows its wait notice.
    void wait(Entry &entry, WaitReason reason);
    // Disconnects the current prerequisite and removes its close-specific Stop offer.
    void clearWait();
    void finishSave(TabPage *tab, TabPage::SaveResult result);
    // Selects the waiting tab and restores its current window from the tray or minimized state.
    void attention(TabPage *tab);
    // Applies approved closes and requests exit or relaunch only after final approval.
    void finish();
    // Ends the request while preserving tabs, releasing claims and resuming selected documents.
    void cancel();
    // Releases owned tab claims and queues ordinary selection-driven resume again.
    void releaseClaims();

    QPointer<WindowManager> manager_;
    QPointer<MainWindow> origin_;
    Intent intent_;
    QList<Entry> entries_;
    QPointer<TabPage> waitingTab_;
    QPointer<MainWindow> waitingWindow_;
    WaitReason waiting_ = WaitReason::None;
    QList<QMetaObject::Connection> waitConnections_;
    bool remember_ = true;
    bool posted_ = false;
    bool ended_ = false;
    bool advancing_ = false;
    bool advanceAgain_ = false;
};

} // namespace mervin
