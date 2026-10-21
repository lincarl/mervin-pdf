#pragma once

#include "io/FileProbe.h"
#include "ipc/Message.h"
#include "recent/RecentEntry.h"
#include "recent/RecentStore.h"
#include "recent/ViewState.h"
#include "recent/ViewStateStore.h"
#include "session/ClosedTabStack.h"
#include "session/SessionStore.h"
#include "session/SessionEnd.h"
#include "session/StartupPlan.h"
#include "update/Updater.h"

#include <QList>
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <optional>

#include <functional>
#include <memory>

class MainWindow;
class QSystemTrayIcon;
class QMenu;
class QTimer;

namespace mervin {

namespace ipc {
class SingleInstanceServer;
} // namespace ipc

class RenderEngine;
class TabPage;
class CloseFlow;

// Owns the shared RenderEngine, windows, recent history and view state. The primary process
// routes IPC opens and owns the tray. Shutdown workers before freeing final
// documents, and keep the MuPDF base context alive until every document is destroyed.
class WindowManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    // Initializes shared services and Windows session hooks before creating any window.
    // Without `ownsStores` (a standalone copy beside a live copy that holds the lock but
    // did not take its files) it reads Recent and the view states but writes neither of
    // them nor the session.
    explicit WindowManager(bool ownsStores = true);
    // Releases windows and skips the ordinary close and write budget during session end.
    ~WindowManager() override;

    // Force Qt's colour scheme for a "light"/"dark" setting, or follow the OS for
    // "system". Before any window exists, call this and then Theme::applyApp().
    static void applyColorSchemeToQt(const QString &scheme);

    RenderEngine *engine() const { return engine_.get(); }

    // Take ownership of the single-instance server (already listening) so opens
    // from later launches are delivered to this process's windows, including those
    // the server acknowledged before this call. Call once, on the primary instance only.
    void adoptInstanceServer(std::unique_ptr<ipc::SingleInstanceServer> server);

    // Create and show a new empty window (returns it, never null).
    MainWindow *createWindow();

    // Open a batch of paths per behaviour ("new-window" | anything else =
    // new-tab). Files already open in any window are focused, not duplicated.
    void openPaths(const QStringList &paths, const QString &behavior);

    // Open one document per event-loop turn in a fixed target window: `into`, or the
    // active window when that is null or gone. Replace any pending batch. onDone runs
    // even for an empty batch; startup timing waits for it. savedOrder contains
    // canonical session paths for insertion against the live tab bar.
    void openStaged(const QList<StagedOpen> &batch, const QStringList &savedOrder = {},
                    std::function<void()> onDone = {}, MainWindow *into = nullptr);

    // True when `canonicalPath` is open in any window of this process. Unlike
    // focusExistingTab this only answers the question - it does not select the tab
    // or raise the window, which a background open must not do.
    bool isOpenAnywhere(const QString &canonicalPath, const TabPage *except = nullptr) const;
    // Reports whether shutdown or an active session request blocks new window work.
    bool isClosing() const { return shuttingDown_ || sessionFinal_ || sessionend::active(); }
    bool hasLoadingDocuments() const;

    // Direct Windows session callback, on the GUI thread. Existing recovery
    // snapshots get a bounded grace period, then logoff proceeds in all cases.
    sessionend::Result prepareSessionEnd(int graceMs = sessionend::kGraceMs);
    // Receives WM_ENDSESSION. Cancellation reopens snapshot admission.
    void finishSessionEnd(bool ended);

    // Every file open in a tab, in any window of this process (canonical paths).
    // Extract Pages and Merge PDFs refuse to write over one: the viewer holds it
    // open, so the write would fail.
    QStringList openTabPaths() const;

    // Cross-window: focus an already-open file by canonical path; raises and
    // activates the owning window. Returns true if found.
    bool focusExistingTab(const QString &canonicalPath);

    void newWindow(); // Ctrl+N: an empty window
    void quitAll();   // close every window (process then exits)

    // Requests Quit approval for every window, bypassing close to tray.
    // Running tab work finishes first, and Cancel preserves all open tabs.
    void closeAllForQuit();
    // Quit as above, then start Mervin again (see Relaunch.h); used after a change that
    // only takes effect at startup. Cancel keeps the windows open, and the change
    // waits for the next start.
    void restart();
    // The updater closes windows only if no wait or Save is required. Any answer
    // but Closed may queue chosen saves, but never schedules a later shutdown.
    Updater::CloseResult tryCloseAllForUpdate();
    // Whether closing `window` hides it in the tray instead of closing it. A window
    // without documents closes for real while another window remains.
    bool canCloseToTray(const MainWindow *window) const;
    // Shows every window hidden in the tray, the one hidden last in front. With
    // none hidden, brings up the active window instead.
    void restoreFromTray();
    void applyMemorySettings(int minutes, bool closeToTray);
    // Unloads idle documents only when Quit and session requests permit new work.
    void checkDocumentActivity();
    void notifySuspensionFailure(const QString &message);
    // Shows a warning from the tray icon, for a window hidden in the tray.
    void notifyInTray(const QString &title, const QString &message);

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
    // Broadcast a new default Recent search scope ("names", "contents", "all")
    // from Settings to every window.
    void setRecentSearchScope(const QString &scope) { emit recentSearchScopeChanged(scope); }

    // ---- Recent files + view-state (M6), owned in-process ---------------------
    // Record that a file was opened (push to recent history). When
    // restoreViewState is true (a freshly opened tab), the file's saved view
    // state (if any) is applied to the matching tab immediately. The entry keeps
    // the mount recorded for it until a check on a background thread finds the
    // file and records the mount it is on now.
    void recordOpen(const QString &canonicalPath, bool restoreViewState, int pageCount = 0);

    // Persist a tab's current view state (called on tab/window close).
    void saveViewState(const QString &canonicalPath, const ViewState &state);
    void saveViewStates(const QList<QPair<QString, ViewState>> &states);

    // Remove a file from the recent history (panel "Remove from history").
    void removeRecent(const QString &canonicalPath);

    // Remove the listed recent entries whose files no longer exist on disk, as a
    // check on a background thread finds them.
    void clearMissingRecent(const QStringList &paths);

    // Set or clear the favourite flag on a recent entry. A favourite never ages
    // out of Recent, and neither does its view state.
    void setFavorite(const QString &canonicalPath, bool favorite);

    // Re-broadcast the current recent list to every panel, then prune it (see
    // pruneMissingRecent).
    void refreshRecent();

    // Apply Settings > General > Recent files after OK: the shown count goes to
    // every panel, a lower retention trims both stores now, and switching off
    // "Keep removed files in list" prunes at once.
    void applyRecentSettings(int visibleCount, int retention, bool keepMissing);

    // With that setting off, forget entries whose file is gone while its drive,
    // share or mount is attached (FileFacts::removed, given the mount recorded for
    // the file). fileProbe() checks the disk. A no-op while the setting is on.
    // Its checks, as any other, record the mount of a file found there whose entry
    // has none (RecentStore::setMissingMounts).
    void pruneMissingRecent();
    // Mounts found that way are saved, and the list announced, together when the
    // prune pass that found them ends, and otherwise at most this long after the
    // first was found. Later finds do not delay the save, and a quit saves them.
    static constexpr int kMountSaveMs = 500;

    // Crash recovery / session restore (M11). restoreSession() loads the saved
    // session, has fileProbe() check its documents and calls `done` with the ones
    // to reopen. It waits until every check has answered or kRestoreWaitMs has
    // passed, with the window live. Documents on an unavailable drive, share or
    // mount stay in the session unopened (see SessionStore::prepareRestore), judged
    // by the mount recorded for each file when there is one (SessionStore::mount),
    // and so do those whose check has not answered in time. One that answers later
    // opens then, unloaded, right after the last tab in the bar saved before it, or
    // first when none is, without taking the view unless the window shows no document yet.
    // updateSession() saves the current open set (union across live windows)
    // followed by those kept documents, and waits while the restore does. The
    // single UI process is the only writer.
    static constexpr int kRestoreWaitMs = 1500;
    void restoreSession(std::function<void(const QStringList &)> done);
    void updateSession();
    // While held, updateSession() saves nothing. Startup holds it from the restore
    // until the restored documents are queued with openStaged(), so a save in
    // between, such as for a command-line file, cannot drop them from the session.
    // Documents that answered late open once it is released and the batch is in.
    void holdSession(bool hold);

    // Checks files for the UI on background threads (see FileProbe). Shared by
    // every window, so each file is checked once whichever list shows it.
    FileProbe *fileProbe() { return &probe_; }

    // Close the unloaded tabs whose file is gone: deleted, moved or renamed from a
    // folder that is still there, while its drive, share or mount is attached
    // (RecentStore::isRemovedFromDisk, given the mount recorded for the file). That
    // is the rule session restore uses to drop a document. Other tabs
    // stay. A loaded tab still shows its document, a tab with unsaved edits reloads
    // from its recovery copy, and a tab whose folder or volume is away keeps Retry. Windows in the tray
    // keep their tabs until they are shown again. The disk is checked on a worker
    // thread, since network paths can stall. Runs when a window is activated and
    // after an unloaded tab fails to load.
    void closeRemovedTabs();

    // The document that was on screen when the session was recorded. Valid once
    // restoreSession() has called back; empty when the session predates the field,
    // recorded no current document, or that document is not restored.
    QString sessionActivePath() const { return session_.activePath(); }

    // The current recent list (so a newly shown panel has data immediately).
    const QList<RecentEntry> &recentEntries() const { return recent_.entries(); }

    // Remember canonical sibling order and the closed index so reopening derives a position in
    // the live bar. Memory-only undo history; updateSession separately persists open tabs.
    void rememberClosedTab(const QString &path, const QString &canonicalPath,
                           const QStringList &siblings, int index);

    enum class Reopen {
        Reopened,        // a tab came back
        Failed,          // an entry was spent trying: openFile said no and has
                         // already told the user why (an error box, or their own
                         // Cancel on the password prompt)
        NothingToReopen, // the history held nothing that could still be reopened
    };

    // Reopen the most recently closed tab into `into` (the window whose shortcut
    // fired; falls back to the active window), and report the outcome to `done`.
    // A window in the tray, or none, gets no tab: the press fails instead.
    // Entries whose file is open again or gone from disk are skipped rather than
    // spending the press, so the three outcomes above are genuinely distinct - the
    // caller must not report a Failed press as an empty history, which would be a
    // lie with entries still waiting. fileProbe() checks each file first, and the
    // press waits for its answer with the window live, for up to kReopenWaitMs.
    // When the check does not answer by then, the press fails and the entry stays
    // for the next one. Presses made meanwhile wait their turn.
    static constexpr int kReopenWaitMs = 5000;
    void reopenClosedTab(MainWindow *into = nullptr, std::function<void(Reopen)> done = {});

signals:
    void recentListChanged(const QList<mervin::RecentEntry> &entries);
    void colorSchemeChanged(const QString &scheme);
    void documentThemeChanged(const QString &theme);
    void autoUpdateChanged(bool on);
    void recentVisibleCountChanged(int count);
    void recentSearchScopeChanged(const QString &scope);
    void annotationDefaultsChanged(const QString &color, const QString &author);
    void memorySettingsChanged(int minutes, bool closeToTray);

protected:
    // Reapplies the theme on an OS theme or palette change (see the constructor).
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Preserves Qt session handling while avoiding its global logoff flush.
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    // Takes the launches the single-instance server has acknowledged, to open them.
    void takeInstanceOpens();
    // Opens the oldest launch taken and not opened yet, unless a quit or an update's
    // close is in progress.
    void openNextInstanceOpen();
    // Opens the launches a cancelled quit or update held.
    void resumeInstanceOpens();

private:
    friend class CloseFlow;
    // Stops acknowledging launches once this process is quitting, so they wait for
    // it to exit instead of handing it files it would not show. The lock stays until
    // the exit wait is over, so the next copy reads the last writes. Launches it
    // acknowledged before and did not open yet go to relaunch::carry, so the copy
    // started next opens them.
    void endHandoffs();
    // Queues a write of one of the stores. A standalone copy writes none.
    void writeStore(const QString &file, const QByteArray &data) const;
    void onWindowDestroyed(MainWindow *window);
    void stagedStep(); // opens one document of stagedQueue_, then re-arms itself
    // Coalesce staged turns, including a wake after a canceled session request.
    void postStagedStep();
    // Resume retained manager work on the turn after session cancellation.
    void postSessionContinuations();
    // An accepted path batch retained across a pending OS session request.
    struct SessionOpen
    {
        QStringList paths;
        QString behavior;
        QPointer<MainWindow> window; // retain a partially delivered new-window batch
    };
    // Deliver accepted paths, retaining any remainder while session end is pending.
    void continueOpenPaths(SessionOpen batch);

    // Serve the first waiting reopenClosedTab press, once fileProbe() has
    // answered for the entry it would bring back.
    void serveReopen();
    // Answers from fileProbe() for the checks this object asked for.
    void onFileChecked(const FileCheck &file, const FileFacts &facts);
    void finishRestore();
    // A session document whose check answered after the restore was decided.
    void restoreLate(const QString &path, const FileFacts &facts);
    void openLateWaiting();
    // Forget the recent entries the checks found gone, all in one save.
    void dropRecent(const QString &path);
    void dropFoundRecent();
    // Collect the mounts that checks found for recent entries that have none, and
    // save them together (RecentStore::setMissingMounts, kMountSaveMs).
    void recordMount(const QString &path, const QString &mount);
    void recordFoundMounts();
    // Give viewState_ the favourites, whose view state never ages out
    // (ViewStateStore::setKept). Run after anything that changes them.
    void keepFavouriteViewStates();

    MainWindow *activeOrNewWindow();
    MainWindow *emptyWindow() const; // a document-less window, or nullptr
    // The windows hidden in the tray, in the order they were hidden.
    QList<MainWindow *> trayWindows() const;
    void applyViewStateIfStored(const QString &canonicalPath);
    // The mount recorded for a document, from the session's copy, else Recent's.
    QString recordedMount(const QString &canonicalPath) const;
    // Close the tabs of files a closeRemovedTabs scan has just found gone, each as
    // soon as it is found. A window that has to keep a tab for now
    // (MainWindow::closeRemovedTabs) queues the whole check again
    // (MainWindow::whenTabsCanChange), so a late close follows a fresh answer.
    void closeFoundRemovedTabs(const QStringList &canonicalPaths);
    // Run every window's queued tab changes (MainWindow::runTabChanges) now, or
    // on the next turn of the event loop.
    void runTabChanges();
    void postTabChanges();

    // Rebuild the app-wide Theme stylesheet on the next event-loop turn. Deferred
    // (and coalesced) because the colour-scheme signals that drive it fire BEFORE
    // Qt finishes propagating the new palette; rebuilding immediately would read a
    // stale palette and often produce an identical sheet that setStyleSheet() then
    // skips, leaving the chrome on the old theme until the next restart.
    void scheduleThemeRefresh();
    void updateTrayAvailability();

    // Declared first so it is destroyed LAST (after all windows/Documents).
    std::unique_ptr<RenderEngine> engine_;
    FileProbe probe_;
    std::unique_ptr<ipc::SingleInstanceServer> instanceServer_; // primary only; may be null
    SessionStore session_;
    ClosedTabStack closedTabs_; // in-memory undo history for Ctrl+Shift+T
    RecentStore recent_;        // recent-files history (single writer = this process)
    ViewStateStore viewState_;  // per-file resume state (page/zoom/rotation)
    // Where the stores are kept. They are read once, at startup, and written by
    // appfiles::write on a background thread.
    QString recentFile_;        // persisted path for recent_
    QString viewStateFile_;     // persisted path for viewState_
    QString sessionFile_;       // persisted path for session_
    bool ownsStores_ = true;    // false in a standalone copy, which writes none of them
    QList<ipc::Message> instanceOpens_; // launches acknowledged and not opened yet
    bool holdingOpens_ = false; // an update asks about unsaved edits; instanceOpens_ waits
    QList<MainWindow *> windows_;
    QList<QPointer<CloseFlow>> closeFlows_;
    QList<StagedOpen> stagedQueue_;      // documents still to open, front first
    QStringList stagedOrder_;            // the session's saved order, for tab placement
    MainWindow *stagedWindow_ = nullptr; // the window the batch opens into
    std::function<void()> stagedDone_;   // runs when stagedQueue_ empties
    MainWindow *active_ = nullptr;
#ifdef Q_OS_WIN
    bool forwardingSessionQuery_ = false; // GUI-thread native forwarding recursion guard
#endif
    bool shuttingDown_ = false;
    bool quitting_ = false;
    bool checkingActivity_ = false;
    int unloadInactiveMinutes_ = 30;
    bool closeToTray_ = true;
    QElapsedTimer activityClock_;
    QTimer *activityTimer_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    std::unique_ptr<QMenu> trayMenu_;
    QString colorScheme_;
    QString documentTheme_;
    bool themeRefreshPending_ = false; // a deferred Theme::applyApp() is queued
    bool keepMissingRecent_ = false;   // Settings::recentKeepMissing
    bool movingTab_ = false;           // detachTab or mergeTab is moving a tab
    bool tabScanRunning_ = false;      // a closeRemovedTabs scan is on its worker
    bool tabScanPending_ = false;      // ...and another was asked for meanwhile
    bool tabChangesPosted_ = false;    // postTabChanges has a run on its way
    bool stagedStepPosted_ = false;
    bool stagedSessionPaused_ = false;
    bool reopenSessionPaused_ = false;
    bool sessionContinuationsPosted_ = false;
    QList<SessionOpen> sessionOpens_; // accepted IPC/path batches, including empty launches
    // Set on shutdown so a closeRemovedTabs scan stops between paths. Nothing waits
    // for the scan, so one stuck on an unreachable share is left behind.
    std::shared_ptr<std::atomic<bool>> scanCancel_ = std::make_shared<std::atomic<bool>>(false);

    // Checks whose fileProbe() answer is awaited, each set by FileProbe::keyOf.
    QSet<QString> restoreWaiting_; // session documents, while the restore waits
    QSet<QString> restoreLate_;    // ...still unanswered when it stopped waiting
    QSet<QString> mountWanted_;    // recordOpen: record the mount if the file is there
    QSet<QString> pruneWanted_;    // pruneMissingRecent: forget it if removed from disk
    QSet<QString> clearWanted_;    // clearMissingRecent: forget it if missing
    bool restorePending_ = false;  // restoreSession is waiting for its checks
    bool sessionHeld_ = false;     // see holdSession
    bool sessionFinal_ = false;    // quitting saved the session for the last time
    std::function<void(const QStringList &)> restoreDone_;
    QHash<QString, FileCheck> restoreChecks_; // by normalizePathKey: each document's check
    QStringList restoreOrder_;          // the saved session order, for late documents
    QPointer<MainWindow> restoreWindow_; // where late documents open
    QStringList lateWaiting_;           // late documents waiting for the session's tabs
    QStringList recentDue_;             // found by pruneWanted_ and clearWanted_
    QHash<QString, QString> mountsDue_; // path -> mount, found there by any check
    QTimer *mountSaveTimer_ = nullptr;  // saves mountsDue_ kMountSaveMs after the first
    // Ctrl+Shift+T presses waiting for an answer, oldest first.
    struct ReopenPress
    {
        QPointer<MainWindow> into;
        std::function<void(Reopen)> done;
    };
    QList<ReopenPress> reopenPresses_;
    std::optional<FileCheck> reopenCheck_; // the check the first press waits on
    quint64 reopenAttempt_ = 0;            // tells a stale timeout from the current one
    quint64 reopenTimedOut_ = 0;           // the attempt whose wait ran out
};

} // namespace mervin
