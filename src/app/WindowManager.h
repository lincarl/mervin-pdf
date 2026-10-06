#pragma once

#include "ipc/Message.h"
#include "recent/RecentEntry.h"
#include "recent/RecentStore.h"
#include "recent/ViewState.h"
#include "recent/ViewStateStore.h"
#include "session/ClosedTabStack.h"
#include "session/SessionStore.h"
#include "session/StartupPlan.h"

#include <QList>
#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <atomic>

#include <functional>
#include <memory>

class MainWindow;
class QLocalSocket;
class QSystemTrayIcon;
class QMenu;
class QTimer;

namespace mervin {

namespace ipc {
class SingleInstanceServer;
} // namespace ipc

class RenderEngine;

// Owns the shared RenderEngine, windows, recent history and view state. The primary process
// routes IPC opens and owns the tray. Shutdown workers before freeing final
// documents, and keep the MuPDF base context alive until every document is destroyed.
class WindowManager : public QObject
{
    Q_OBJECT

public:
    WindowManager();
    ~WindowManager() override;

    RenderEngine *engine() const { return engine_.get(); }

    // Take ownership of the single-instance server (already listening) so opens
    // from later launches are delivered to this process's windows. Call once,
    // on the primary instance only.
    void adoptInstanceServer(std::unique_ptr<ipc::SingleInstanceServer> server);

    // Create and show a new empty window (returns it, never null).
    MainWindow *createWindow();

    // Open a batch of paths per behaviour ("new-window" | anything else =
    // new-tab). Files already open in any window are focused, not duplicated.
    void openPaths(const QStringList &paths, const QString &behavior);

    // Open one document per event-loop turn in a fixed target window. Replace any pending
    // batch. onDone runs even for an empty batch; startup timing waits for it. savedOrder
    // contains canonical session paths for insertion against the live tab bar.
    void openStaged(const QList<StagedOpen> &batch, const QStringList &savedOrder = {},
                    std::function<void()> onDone = {});

    // True when `canonicalPath` is open in any window of this process. Unlike
    // focusExistingTab this only answers the question - it does not select the tab
    // or raise the window, which a background open must not do.
    bool isOpenAnywhere(const QString &canonicalPath) const;

    // Every file open in a tab, in any window of this process (canonical paths).
    // Extract Pages and Merge PDFs refuse to write over one: the viewer holds it
    // open, so the write would fail.
    QStringList openTabPaths() const;

    // Cross-window: focus an already-open file by canonical path; raises and
    // activates the owning window. Returns true if found.
    bool focusExistingTab(const QString &canonicalPath);

    void newWindow(); // Ctrl+N: an empty window
    void quitAll();   // close every window (process then exits)

    // Explicit Quit/update shutdown bypasses closing to the tray. Prompt before destroying
    // any window, so cancelling preserves every open tab.
    bool closeAllForQuit();
    bool canCloseToTray() const;
    void restoreFromTray();
    void applyMemorySettings(int minutes, bool closeToTray);
    void checkDocumentActivity();
    void notifySuspensionFailure(const QString &message);

    // Detachable tabs (M9), all in-process. detachTab moves the tab at `index`
    // of `source` into a fresh window placed near globalPos (no-op if it is the
    // source's only tab). mergeTab moves a tab from one window into another at a
    // drop position, closing the source if it empties.
    void detachTab(MainWindow *source, int index, const QPoint &globalPos);
    void mergeTab(MainWindow *source, int sourceIndex, MainWindow *target, int targetIndex);

    // "Duplicate to new window": open `path` in a fresh window as a second,
    // independent view (bypassing the usual focus-existing-tab dedup), placed
    // near globalPos and seeded with `state` so it mirrors the source tab's
    // current page / zoom / rotation. `password` is the source tab's (empty when
    // the file is not encrypted), so the copy opens without asking again.
    void duplicateToNewWindow(const QString &path, const QString &password,
                              const ViewState &state, const QPoint &globalPos);

    // Active-window tracking, called by MainWindow on activation.
    void notifyActivated(MainWindow *w);

    // UI theme: "system" | "light" | "dark". Applies process-wide immediately
    // and persists to config. Broadcasts colorSchemeChanged to all windows.
    QString colorScheme() const { return colorScheme_; }
    void setColorScheme(const QString &scheme);

    // Document theme: "light" | "dark" | "comfort" | "follow-ui". A global
    // preference for how PDF pages are tinted, independent of the UI theme.
    // Persists to config and broadcasts documentThemeChanged so every window
    // re-applies it to its tabs.
    QString documentTheme() const { return documentTheme_; }
    void setDocumentTheme(const QString &theme);

    // New default annotation colour ("#RRGGBB") and author name (empty = the OS
    // user name) from Settings OK, already saved. Broadcasts
    // annotationDefaultsChanged so every window's open tabs use them for new marks.
    void setAnnotationDefaults(const QString &color, const QString &author);

    // Automatic updates on or off. Persists to config and broadcasts
    // autoUpdateChanged: every window keeps a settings copy that it rewrites
    // whole on close, so each must hear of the change, and the Updater drops a
    // downloaded update when updates are switched off.
    void setAutoUpdate(bool on);

    // ---- Recent files + view-state (M6), owned in-process ---------------------
    // Record that a file was opened (push to recent history). When
    // restoreViewState is true (a freshly opened tab), the file's saved view
    // state (if any) is applied to the matching tab immediately.
    void recordOpen(const QString &canonicalPath, bool restoreViewState, int pageCount = 0);

    // Persist a tab's current view state (called on tab/window close).
    void saveViewState(const QString &canonicalPath, const ViewState &state);
    void saveViewStates(const QList<QPair<QString, ViewState>> &states);

    // Remove a file from the recent history (panel "Remove from history").
    void removeRecent(const QString &canonicalPath);

    // Remove the listed recent entries whose files no longer exist on disk.
    void clearMissingRecent(const QStringList &paths);

    // Set or clear the favourite flag on a recent entry.
    void setFavorite(const QString &canonicalPath, bool favorite);

    // Re-broadcast the current recent list to every panel, then prune it (see
    // pruneMissingRecent).
    void refreshRecent();

    // Apply Settings > General > Recent files after OK: the shown count goes to
    // every panel, a lower retention trims both stores now, and switching off
    // "Keep removed files in list" prunes at once.
    void applyRecentSettings(int visibleCount, int retention, bool keepMissing);

    // With that setting off, forget entries whose file is gone from a folder that
    // still exists (RecentStore::isRemovedFromDisk). The disk is checked on a
    // worker thread, since network paths can stall. A no-op while the setting is on.
    void pruneMissingRecent();

    // Crash recovery / session restore (M11). sessionPaths() loads the
    // previously-open documents; updateSession() saves the current open set
    // (union across live windows). The single UI process is the only writer.
    QStringList sessionPaths();
    void updateSession();

    // The document that was on screen when the session was recorded. Valid after
    // sessionPaths() has loaded the file; empty when the session predates the
    // field or recorded no current document.
    QString sessionActivePath() const { return session_.activePath(); }

    // The current recent list (so a newly shown panel has data immediately).
    const QList<RecentEntry> &recentEntries() const { return recent_.entries(); }

    // Remember canonical sibling order and the closed index so reopening derives a position in
    // the live bar. Memory-only undo history; sessionPaths separately persists open tabs.
    void rememberClosedTab(const QString &path, const QStringList &siblings, int index);

    enum class Reopen {
        Reopened,        // a tab came back
        Failed,          // an entry was spent trying: openFile said no and has
                         // already told the user why (an error box, or their own
                         // Cancel on the password prompt)
        NothingToReopen, // the history held nothing that could still be reopened
    };

    // Reopen the most recently closed tab into `into` (the window whose shortcut
    // fired; falls back to the active window). Entries whose file is open again or
    // gone from disk are skipped rather than spending the press, so the three
    // outcomes above are genuinely distinct - the caller must not report a Failed
    // press as an empty history, which would be a lie with entries still waiting.
    Reopen reopenClosedTab(MainWindow *into = nullptr);

signals:
    void recentListChanged(const QList<mervin::RecentEntry> &entries);
    void colorSchemeChanged(const QString &scheme);
    void documentThemeChanged(const QString &theme);
    void autoUpdateChanged(bool on);
    void recentVisibleCountChanged(int count);
    void annotationDefaultsChanged(const QString &color, const QString &author);
    void memorySettingsChanged(int minutes, bool closeToTray);

protected:
    // Reapplies the theme on an OS theme or palette change (see the constructor).
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // An open delivered by a later launch over the single-instance pipe.
    void onInstanceMessage(QLocalSocket *socket, const mervin::ipc::Message &msg);

private:
    void onWindowDestroyed(MainWindow *window);
    void stagedStep(); // opens one document of stagedQueue_, then re-arms itself

    // Forget closed-tab entries that can no longer be brought back: the file is
    // open again somewhere, or it is no longer on disk.
    void pruneClosedTabs();

    MainWindow *activeOrNewWindow();
    MainWindow *emptyWindow() const; // a document-less window, or nullptr
    void applyViewStateIfStored(const QString &canonicalPath);

    static void applyColorSchemeToQt(const QString &scheme);

    // Rebuild the app-wide Theme stylesheet on the next event-loop turn. Deferred
    // (and coalesced) because the colour-scheme signals that drive it fire BEFORE
    // Qt finishes propagating the new palette; rebuilding immediately would read a
    // stale palette and often produce an identical sheet that setStyleSheet() then
    // skips, leaving the chrome on the old theme until the next restart.
    void scheduleThemeRefresh();
    void updateTrayAvailability();

    // Declared first so it is destroyed LAST (after all windows/Documents).
    std::unique_ptr<RenderEngine> engine_;
    std::unique_ptr<ipc::SingleInstanceServer> instanceServer_; // primary only; may be null
    SessionStore session_;
    ClosedTabStack closedTabs_; // in-memory undo history for Ctrl+Shift+T
    RecentStore recent_;        // recent-files history (single writer = this process)
    ViewStateStore viewState_;  // per-file resume state (page/zoom/rotation)
    QString recentFile_;        // persisted path for recent_
    QString viewStateFile_;     // persisted path for viewState_
    QList<MainWindow *> windows_;
    QList<StagedOpen> stagedQueue_;      // documents still to open, front first
    QStringList stagedOrder_;            // the session's saved order, for tab placement
    MainWindow *stagedWindow_ = nullptr; // the window the batch opens into
    std::function<void()> stagedDone_;   // runs when stagedQueue_ empties
    MainWindow *active_ = nullptr;
    bool shuttingDown_ = false;
    bool quitting_ = false;
    bool checkingActivity_ = false;
    bool cacheTrimmed_ = false;
    int unloadInactiveMinutes_ = 30;
    bool closeToTray_ = true;
    QElapsedTimer activityClock_;
    QTimer *activityTimer_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    std::unique_ptr<QMenu> trayMenu_;
    QString colorScheme_;
    QString documentTheme_;
    bool themeRefreshPending_ = false; // a deferred Theme::applyApp() is queued
    bool keepMissingRecent_ = true;    // Settings::recentKeepMissing
    bool pruneRunning_ = false;        // a pruneMissingRecent scan is on the worker
    bool prunePending_ = false;        // ...and another was asked for meanwhile
    // Set on shutdown so a scan stuck on slow network paths stops between paths
    // instead of keeping the process alive.
    std::atomic<bool> pruneCancel_{false};
    // Declared last so it is destroyed first: its destructor waits for a running
    // scan while the rest of this object is still intact.
    QThreadPool prunePool_;
};

} // namespace mervin
