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
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class MainWindow;
class QLocalSocket;

namespace mervin {

namespace ipc {
class SingleInstanceServer;
} // namespace ipc

class RenderEngine;

// Process-level owner of the UI session: one shared RenderEngine and all the
// MainWindows in this process. Created once per UI process.
//
// This process is also the single-instance "primary": it owns the recent-files
// history and per-file view state directly (single writer, persisted as JSON),
// and - when handed the instance server - accepts file-opens from later launches
// over IPC and routes them into its own windows. There is no separate background
// process; when the last window closes the process exits and nothing lingers.
//
// Teardown ordering is structural and load-bearing: render workers dereference
// a live Document, and the MuPDF base context must outlive every Document.
// So the engine is declared before the window list (destroyed last), and on
// the last window closing we stop the workers BEFORE the final Documents are
// freed, then drop the context.
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

    // Open a batch one document per event-loop turn, into the active window.
    // Startup uses this so the window paints and stays responsive while the batch
    // trickles in: on a cold file cache a single open is dominated by disk reads
    // and the virus scanner's first pass, and doing a whole session inline froze
    // the process with nothing on screen. `onDone` runs after the last document
    // (and still runs for an empty batch) - --quit-after-startup waits for it so
    // tst_perf_startup keeps timing the whole batch rather than just first paint.
    // A batch replaces any batch still in flight. The batch is pinned to one
    // window, so a window created while it drains does not inherit the rest of it.
    // `savedOrder` is the session's canonical paths in order, used to place each
    // restored tab against the live tab bar (see insertIndexForSaved).
    void openStaged(const QList<StagedOpen> &batch, const QStringList &savedOrder = {},
                    std::function<void()> onDone = {});

    // True when `canonicalPath` is open in any window of this process. Unlike
    // focusExistingTab this only answers the question - it does not select the tab
    // or raise the window, which a background open must not do.
    bool isOpenAnywhere(const QString &canonicalPath) const;

    // Cross-window: focus an already-open file by canonical path; raises and
    // activates the owning window. Returns true if found.
    bool focusExistingTab(const QString &canonicalPath);

    void newWindow(); // Ctrl+N: an empty window
    void quitAll();   // close every window (process then exits)

    // Detachable tabs (M9), all in-process. detachTab moves the tab at `index`
    // of `source` into a fresh window placed near globalPos (no-op if it is the
    // source's only tab). mergeTab moves a tab from one window into another at a
    // drop position, closing the source if it empties.
    void detachTab(MainWindow *source, int index, const QPoint &globalPos);
    void mergeTab(MainWindow *source, int sourceIndex, MainWindow *target, int targetIndex);

    // "Duplicate to new window": open `path` in a fresh window as a second,
    // independent view (bypassing the usual focus-existing-tab dedup), placed
    // near globalPos and seeded with `state` so it mirrors the source tab's
    // current page / zoom / rotation.
    void duplicateToNewWindow(const QString &path, const ViewState &state,
                              const QPoint &globalPos);

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

    // ---- Recent files + view-state (M6), owned in-process ---------------------
    // Record that a file was opened (push to recent history). When
    // restoreViewState is true (a freshly opened tab), the file's saved view
    // state (if any) is applied to the matching tab immediately.
    void recordOpen(const QString &canonicalPath, bool restoreViewState, int pageCount = 0);

    // Persist a tab's current view state (called on tab/window close).
    void saveViewState(const QString &canonicalPath, const ViewState &state);

    // Remove a file from the recent history (panel "Remove from history").
    void removeRecent(const QString &canonicalPath);

    // Remove the listed recent entries whose files no longer exist on disk.
    void clearMissingRecent(const QStringList &paths);

    // Set or clear the favourite flag on a recent entry.
    void setFavorite(const QString &canonicalPath, bool favorite);

    // Re-broadcast the current recent list to every panel.
    void refreshRecent();

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

    // ---- Reopen closed tab (Ctrl+Shift+T) -------------------------------------
    // Record a tab that is being closed. `siblings` are the canonical paths of the
    // whole tab bar it is leaving, in order, and `index` its position in them;
    // together they let a reopen re-derive the slot against whatever the bar looks
    // like by then (see ClosedTab). Held in memory only: this is an undo for the
    // session in front of you, not a second session file (the real session restore
    // is sessionPaths() above, and it lists only what is still open).
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

private slots:
    void onWindowDestroyed(QObject *obj);
    // An open delivered by a later launch over the single-instance pipe.
    void onInstanceMessage(QLocalSocket *socket, const mervin::ipc::Message &msg);

private:
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
    QString colorScheme_;
    QString documentTheme_;
    bool themeRefreshPending_ = false; // a deferred Theme::applyApp() is queued
};

} // namespace mervin
