#pragma once

#include "config/Settings.h"
#include "dialogs/SettingsDialog.h"
#include "extract/ExtractPlan.h"
#include "recent/ViewState.h"
#include "render/MeasureContent.h"
#include "ui/ViewerWidget.h"
#include "ui/TabPage.h"

#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QSet>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace mervin {
class CommentsSidebar;
class ContentSearch;
class DetachableTabBar;
class Document;
class OutlineSidebar;
class RecentFilesPanel;
class RenderEngine;
class TabPage;
class ThumbnailSidebar;
class WindowManager;
class CloseFlow;
} // namespace mervin

class QLineEdit;
class QLabel;
class QComboBox;
class QMenu;
class QDockWidget;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QToolBar;
class QToolButton;
class QUrl;

// Owns one window's document tabs, commands and ordered tab-change queue.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // engine and wm are owned by the process-level WindowManager and outlive
    // every window. Either may be null only in degraded/standalone use.
    explicit MainWindow(mervin::RenderEngine *engine,
                        mervin::WindowManager *wm,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // Open or focus an existing tab across this process. allowDuplicate creates an independent
    // view whose state the caller seeds. atIndex=-1 appends; makeCurrent=false preserves focus
    // during staged restore. Returns whether a tab was accepted or focused, before
    // the background reader finishes. Try knownPassword before prompting.
    bool openFile(const QString &path, bool allowDuplicate = false, int atIndex = -1,
                  bool makeCurrent = true, const QString &knownPassword = QString(),
                  bool lazy = false, const std::optional<mervin::ViewState> &initialView = {},
                  bool closeOnFailure = false);

    // If the given canonical path is open in THIS window, select its tab and
    // return true. (Cross-window focusing is orchestrated by WindowManager.)
    bool focusTabIfOpen(const QString &canonicalPath);

    // If the given canonical path is open in THIS window, restore the saved
    // view state (page / zoom / rotation) into its viewer. Returns true if the
    // tab was found here. Called by WindowManager when restoring view state.
    bool applyViewState(const QString &canonicalPath, const mervin::ViewState &state);

    // Number of open document tabs (0 == an empty window showing the start page).
    int tabCount() const;

    // Canonical paths of this window's open documents (for session restore).
    QStringList tabPaths() const;

    // Canonical path of the document tab currently shown, or empty when this
    // window holds none. Stays valid while the Recent view is up (that is a
    // separate page of the central stack, so the current tab is unchanged), which
    // is what the session wants: the document to reopen first next time.
    QString currentTabPath() const;

    // Detachable-tabs support (M9): move a live TabPage between windows.
    // releaseTab removes the tab WITHOUT destroying it and returns it (parent
    // cleared); adoptTab inserts an existing TabPage and makes it current.
    mervin::TabPage *releaseTab(int index);
    void adoptTab(mervin::TabPage *page, int atIndex = -1);

    // A hidden tray window retains its tabs and resumes only the selected document on show.
    void hideToTray();
    void restoreFromTray();
    bool isInTray() const { return hiddenToTray_; }
    // Rises with every hideToTray() across all windows, so the window hidden last has
    // the highest value. WindowManager restores tray windows in this order.
    quint64 trayOrder() const { return trayOrder_; }
    // Bypasses questions and close-to-tray after approval, or for an empty window.
    void closeForQuit();
    void applyMemorySettings(int minutes, bool closeToTray);
    // nowMs is monotonic process time; explicit input also makes deadline tests deterministic.
    bool updateDocumentActivity(qint64 nowMs, int timeoutMinutes);
    bool suspendHiddenDocuments();
    bool hasResidentDocuments() const;
    bool hasLoadingDocuments() const;
    // Tabs without their document in memory: unloaded, or failed to load.
    QList<mervin::TabPage *> unloadedTabs() const;
    // Close, without asking, every unloaded tab of `canonicalPath` that has no
    // unsaved edits. WindowManager::closeRemovedTabs has just found the file gone,
    // so this touches no disk. The tab is neither remembered for Ctrl+Shift+T nor
    // given a view state. A window in the tray keeps its tabs. False when a tab has
    // to stay for now, because the window cannot change its tabs (canChangeTabs) or
    // tab changes queued earlier still wait (whenTabsCanChange), which go first.
    bool closeRemovedTabs(const QString &canonicalPath);

    // True when nothing may hold a position in this window's tab bar across a
    // nested event loop. That means no modal dialog or popup in the application,
    // no document operation or unload in this window, no mouse button held and no
    // tab drag. It stays false once a quit or restart closes the window
    // (closeForQuit), so changes still queued then go with the window.
    bool canChangeTabs() const;
    // Run `change` once this window can change its tabs, after every change queued
    // before it. It never runs inside this call, so a tab's own signal handler can
    // ask to remove that tab. A change finds its tab and window again when it runs,
    // through a QPointer, never by a position taken earlier. Changes still queued
    // when the window goes away are dropped.
    void whenTabsCanChange(std::function<void()> change);
    // Run the queued changes while the window can change its tabs. WindowManager
    // calls it after a mouse button release, after a modal window closes and on
    // its activity tick. The window also calls it when a document operation ends.
    void runTabChanges();

    // Exposes queued tab-change state for completion-delivery tests.
    struct testing {
        // Reports pending tab changes without running the queue.
        static bool hasQueuedTabChanges(const MainWindow &window)
        {
            return !window.tabChanges_.empty();
        }
    };

protected:
    bool eventFilter(QObject *watched, QEvent *event) override; // tab-bar middle-click
    // Handles tray hiding and close approval before persisting window state.
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;                   // report activation to WindowManager
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;                 // apply the caption colour once native
    void dragEnterEvent(QDragEnterEvent *event) override;       // drag-and-drop to open
    void dropEvent(QDropEvent *event) override;

private slots:
    void onOpen();
    void onOpenInNewWindow();
    void showAbout();
    void showShortcuts();
    void onPageEditReturn();
    void onZoomComboActivated();
    void onCurrentTabChanged(int index);
    void closeTab(int index);
    // Requests close for the recorded tab bar, preserving tabs that arrive later.
    void closeAllTabs();
    void reopenClosedTab();   // Ctrl+Shift+T: bring back the last tab closed
    void onTabDetachRequested(int index, const QPoint &globalPos);
    void onTabMergeRequested(mervin::DetachableTabBar *source, int sourceIndex, int targetIndex);
    void onOcrRegionSelected(int pageNo, const QRectF &pageRect);
    // Reads the installed OCR languages off the UI thread, then recognizeRegion().
    void listThenRecognize(int pageNo, const QRectF &pageRect, bool offered);
    // OCR of the region with `installed` languages; `offered` once the language
    // manager was offered because none was installed.
    void recognizeRegion(int pageNo, const QRectF &pageRect, const QStringList &installed,
                         bool offered);
    void toggleMeasure(bool on);
    void toggleForms(bool on);   // Fill-Forms tool (Ctrl+Shift+F)
    void toggleComment(bool on); // Comment tool window (Ctrl+Shift+N): highlight + comment
    void onCalibrationLineDrawn(int pageNo, double lengthPoints);
    void onSetScaleRequested(int pageNo); // "Set Scale" button: manual ratio (1 : N)
    void toggleFullScreen(bool on);
    void toggleAlwaysOnTop(bool on);
    // Open Settings on the requested page.
    void openSettings(SettingsDialog::Page page = SettingsDialog::Page::General);
    // Save `next` and put what changed into effect: Settings' Apply and OK.
    // False, with the reason in `saveError`, when the save failed. Nothing changes
    // then.
    bool applySettings(const mervin::Settings &next, QString *saveError = nullptr);

    // Document menu (M8).
    void openSecurity();
    void saveAsCopy();         // embed the current edits (measurements) into a chosen new file
    void saveMeasurements();   // embed measurements in the open file (Mervin-only), in place
    void exportMeasuredCopy(); // export a flattened or annotated copy carrying the measurements
    void printDocument();
    void rotatePagesOp();
    void deletePagesOp();
    void extractPagesOp();
    void splitDocument();
    void mergeDocuments();

private:
    friend class mervin::CloseFlow;
    // Extract Pages or Delete Pages (`mode`) for the current document. The one row
    // holds `seedPage` (0-based) when given: the clicked page from the page's Save
    // Page As, or for Delete the page in view. Extract without it holds every page.
    void pagesDialog(mervin::ExtractPlan::Mode mode, std::optional<int> seedPage);
    void createActions();
    void createToolBar();
    void createMenus();
    void createDocumentMenu(); // Security + page operations; owned by the Document button
    // Adds a non-interactive uppercase section header row to a popup menu, matching
    // the design's menu group labels. `accent` switches to the Document popover's
    // blue heading tone.
    void addSectionHeader(QMenu *menu, const QString &text);
    void createSidebars();     // outline + thumbnails + comments dock widgets
    void updateSidebars();     // sync sidebars with the current document/page
    void refreshCommentsSidebar(); // repopulate the comments list from the current viewer
    // Prompt for a 0-based page set ("all", "1-5", "1,3,5-9"); empty == cancel.
    // Rotate Pages uses it.
    QList<int> askPageRange(const QString &title, int pageCount);
    QStringList askForPdfs();              // app picker: local multi-select or typed URL
    void openUrl(const QUrl &url, bool inNewWindow = false); // download, then open its cache file
    // Ask whether to open a written file. `password` is tried before any prompt,
    // for outputs that keep the source tab's encryption. The question blocks only this
    // window.
    void offerToOpen(const QString &path, const QString &password = QString());
    // A page tool, Rotate Pages or Split, that runs as its tab's operation (see
    // runPageTool). Defined in DocumentActions.cpp.
    struct PageTool;
    struct PageToolResult;
    // Runs `tool` on the file of the usable `tab` with `password`, as the tab's
    // operation, so only the tab waits. Once the tool ends, the window that holds the
    // tab by then finishes it (finishPageTool) after the tab changes queued there
    // before, as its boxes hold the tab across their event loops. `asked` when the
    // user typed the password for this run.
    static void runPageTool(mervin::TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                            const QString &password, bool asked);
    // Queues the result in the tab's current owner, including after its previous
    // owner is destroyed while the result waits in that window's queue.
    static void deliverPageTool(mervin::TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                                const QString &password, bool asked,
                                const std::shared_ptr<PageToolResult> &result);
    // A file that needs a password asks for one, window-modal, and runs the tool
    // again with it, remembered on the tab if it works. A failure says why, and
    // success runs the tool's own question. A window in the tray shows failures as a
    // tray message and asks nothing.
    void finishPageTool(mervin::TabPage *tab, const std::shared_ptr<const PageTool> &tool,
                        const QString &password, bool asked, const PageToolResult &result);
    struct CopyResult;
    // Copy and export borrow the live document only under a stop-aware Hold.
    void startCopy(mervin::TabPage *tab, const QString &destination, bool flatten);
    static void deliverCopy(mervin::TabPage *tab, const QString &destination,
                            const QString &password, bool flatten,
                            const std::shared_ptr<CopyResult> &result);
    void finishCopy(const QString &destination, const QString &password, bool flatten,
                    const CopyResult &result);
    // Gather a viewer's committed measurements + per-page overrides for saving
    // (the Mervin blob), or paired with each page's PDF transform + value label
    // for export/print rendering.
    mervin::MeasureDoc collectMeasureDoc(mervin::ViewerWidget *viewer) const;
    // Builds flattened-output values from the viewer while its document is protected.
    std::vector<mervin::RenderMeasurement> collectRenderMeasurements(
        mervin::ViewerWidget *viewer) const;
    void applyControlStyle(); // re-tint palette-driven chrome: toolbar dividers, Recent pill, icons
    // Re-assert WA_Hover on the chrome buttons after a theme switch re-polishes
    // them (a dropped WA_Hover leaves a button stuck without its :hover highlight).
    void reassertHoverAttributes();
    // Restore Windows 11's default caption and text colours in both UI themes.
    // No-op elsewhere.
    void applyTitleBarTheme();
    // How PDF pages should be toned, given the current document theme setting
    // ("light"/"dark"/"comfort"; legacy "follow-ui" resolves against the live
    // palette).
    mervin::PageTheme computeDocumentPageTheme() const;
    void applyDocumentThemeToViewers(); // re-apply the document theme to every tab
    void applyAnnotationDefaultsToTabs(); // settings_ colour + author to every tab's new marks
    void syncComfortButton();       // moon/sun glyph + tooltip on the toolbar toggle
    void showRecentPanel();         // switch to Recent view (called by pill button)
    void setCommandBarMode(bool recentActive); // hide/show doc-only toolbar widget
    void syncFindToggle();          // toolbar search button follows the current find card
    void syncOcrAction();           // OCR follows the visible document and stays inactive on Recent
    void updateRecentButton();      // sync pill button accent state
    void syncDocTabBar();           // rebuild docTabBar_ to match tabs_
    void updateTabGlyphs();         // tint each doc tab's glyph (accent on the active tab)
    void applyActionIcons();        // set the Lucide icons on every action (theme-aware)
    void setUiEnabled(bool enabled);
    void updateForCurrentTab();
    void updatePageLabels(int current, int total);
    void syncZoomCombo(mervin::ViewerWidget *viewer);
    void syncViewActions(mervin::ViewerWidget *viewer);
    void wireCurrentViewer(mervin::ViewerWidget *viewer); // connect only the current viewer
    void applySettingsToViewer(mervin::ViewerWidget *viewer);
    void applyZoom(mervin::ViewerWidget *viewer, const QString &zoom);

    // View-state persistence (M6).
    void updateStartPage();                                   // swap start page <-> tabs
    mervin::ViewState captureViewState(mervin::ViewerWidget *viewer) const;
    void applyViewStateToViewer(mervin::ViewerWidget *viewer, const mervin::ViewState &state);
    void saveTabViewState(mervin::TabPage *tab);              // push one tab's state
    void saveAllViewStates();                                 // push every open tab

    // Close the tab at `index`. `remember` pushes it onto the process-wide reopen
    // history (Ctrl+Shift+T); it is false for a bulk close, whose caller has
    // already recorded the whole bar through rememberOpenTabs(). The request
    // continues through the event loop and can be canceled.
    void closeTabAt(int index, bool remember);
    // Remove and delete the tab at `index`, then record the session. The caller has
    // already asked about unsaved edits.
    void removeTabAt(int index);
    // Returns the unsaved-edit decision after committing active editors, without applying it.
    // The window-modal question guards tab and window lifetimes across its event loop.
    int askClose(mervin::TabPage *tab);
    void saveTab(mervin::TabPage *tab,
                 std::function<void(mervin::TabPage::SaveResult)> completion);
    static void askSaveReplacement(QPointer<mervin::TabPage> page,
                                   std::function<bool()> valid,
                                   std::function<void(bool)> answer);
    static void deliverSave(QPointer<mervin::TabPage> page,
                            const mervin::TabPage::SaveCompletion &result);
    // Push one tab onto the reopen history. `index` is the slot to restore it to,
    // which mid-bulk-close is NOT its live index - see rememberOpenTabs().
    void rememberClosedTab(mervin::TabPage *tab, int index);
    // Record every open tab for reopening, before a bulk close (Close all tabs,
    // or the window itself going away).
    void rememberOpenTabs();

    mervin::TabPage *currentTab() const;
    mervin::ViewerWidget *currentViewer() const;
    int indexOfPath(const QString &canonicalPath, const mervin::TabPage *except = nullptr) const;

    void wireTabLifecycle(mervin::TabPage *tab);
    void queueFreshLoad(mervin::TabPage *tab, const QString &canonical, quint64 generation);
    void queueFreshRemoval(mervin::TabPage *tab, quint64 generation);
    void queueLoadPassword(mervin::TabPage *tab, quint64 generation);
    void unwireTabLifecycle(mervin::TabPage *tab);
    // Queues resume of the selected document after visibility and close-claim checks.
    void scheduleCurrentResume();
    // Unloads a usable tab unless a close claim or earlier suspension failure keeps it resident.
    bool suspendTab(mervin::TabPage *tab);
    void clearDocumentSidebars();
    bool canSuspendDocuments() const;
    void persistWindowState();
    // Run the queued tab changes on the next turn of the event loop.
    void postTabChanges();
    QList<QPointer<mervin::CloseFlow>> closeFlows_; // standalone windows only

    // Marks a document operation of `window` for its lifetime. Unloading and tab
    // changes wait while one runs, since it may hold a tab or a document across
    // its dialogs. The queued tab changes run once the last one ends.
    class OperationScope
    {
    public:
        explicit OperationScope(MainWindow *window);
        ~OperationScope();
        Q_DISABLE_COPY_MOVE(OperationScope)

    private:
        QPointer<MainWindow> window_;
    };

    QHash<mervin::TabPage *, qint64> inactiveSince_;
    QHash<mervin::TabPage *, QList<QMetaObject::Connection>> tabConnections_;
    QSet<mervin::TabPage *> suspensionFailed_;
    bool hiddenToTray_ = false;
    quint64 trayOrder_ = 0;
    bool closeConfirmed_ = false;
    bool suspending_ = false;
    bool resumeScheduled_ = false;
    bool removingTab_ = false; // a tab is leaving this window (removeTabAt, releaseTab)
    int documentOperationDepth_ = 0; // OperationScope objects alive
    std::deque<std::function<void()>> tabChanges_; // whenTabsCanChange, oldest first
    bool tabChangesPosted_ = false;  // postTabChanges has a run on its way
    bool runningTabChanges_ = false; // runTabChanges is in a change
    // The newest OCR selection, which alone goes on once the installed languages
    // have been read, if that took less than kOcrLanguagesWaitMs.
    quint64 ocrRequest_ = 0;
    static constexpr int kOcrLanguagesWaitMs = 5000;
    Qt::WindowStates beforeTrayState_;

    mervin::Settings settings_;
    QList<QMetaObject::Connection> viewerConns_; // current viewer's signal links
    bool recentActive_ = false;     // true while the Recent panel is the active view

    // Non-owning: the engine and window manager are owned by the process-level
    // WindowManager and outlive every window.
    mervin::RenderEngine *engine_ = nullptr;
    mervin::WindowManager *wm_ = nullptr;
    QStackedWidget *stack_ = nullptr;             // [0] start page, [1] tabs
    mervin::RecentFilesPanel *recentPanel_ = nullptr;
    mervin::ContentSearch *contentSearch_ = nullptr; // start-page content search
    QTabWidget *tabs_ = nullptr;

    // Navigation sidebars (M11).
    mervin::OutlineSidebar *outlineSidebar_ = nullptr;
    mervin::ThumbnailSidebar *thumbnailSidebar_ = nullptr;
    mervin::CommentsSidebar *commentsSidebar_ = nullptr;
    QDockWidget *outlineDock_ = nullptr;
    QDockWidget *thumbnailDock_ = nullptr;
    QDockWidget *commentsDock_ = nullptr;
    mervin::Document *sidebarDoc_ = nullptr; // document the sidebars currently reflect

    QAction *openAction_ = nullptr;
    QAction *openInNewWindowAction_ = nullptr; // dropdown on the split Open button
    // Save split button (dropdown-only): a bare click opens the menu; nothing is
    // saved without an explicit choice.
    QAction *saveEditsAction_ = nullptr;          // embed edits in place
    QAction *saveAsCopyAction_ = nullptr;         // embed edits into a new file
    QAction *exportMeasurementsAction_ = nullptr; // flatten measurements into a copy
    QAction *newWindowAction_ = nullptr;
    QAction *newTabAction_ = nullptr;
    QAction *closeTabAction_ = nullptr;
    QAction *reopenTabAction_ = nullptr; // Reopen Closed Tab (Ctrl+Shift+T)
    QAction *nextTabAction_ = nullptr;
    QAction *prevTabAction_ = nullptr;
    QAction *prevPageAction_ = nullptr;
    QAction *nextPageAction_ = nullptr;
    QAction *findAction_ = nullptr;
    QAction *findNextAction_ = nullptr;
    QAction *findPrevAction_ = nullptr;
    QAction *findCardAction_ = nullptr; // toolbar toggle for the current tab's find card
    QAction *copyAction_ = nullptr;
    QAction *selectAllAction_ = nullptr;
    QAction *ocrAction_ = nullptr;
    QAction *measureAction_ = nullptr;
    QAction *formAction_ = nullptr;                  // Fill Forms (checkable, Ctrl+Shift+F)
    QAction *highlightFormFieldsAction_ = nullptr;   // More-menu toggle (mirrors the setting)
    QAction *commentAction_ = nullptr;               // Comment tool window (checkable, Ctrl+Shift+N)
    QAction *zoomInAction_ = nullptr;
    QAction *zoomOutAction_ = nullptr;
    QAction *fitModeAction_ = nullptr; // Home toggles Fit Page / Fit Width
    QAction *rotateLeftAction_ = nullptr;
    QAction *rotateRightAction_ = nullptr;
    QAction *printAction_ = nullptr;
    QAction *fullScreenAction_ = nullptr;
    QAction *alwaysOnTopAction_ = nullptr;
    QAction *settingsAction_ = nullptr;
    QAction *fitPageAction_ = nullptr;           // toolbar and menu Fit Page
    QAction *fitWidthAction_ = nullptr;          // toolbar and menu Fit Width
    QAction *continuousAction_ = nullptr;
    QAction *singleAction_ = nullptr;
    QAction *twoPageAction_ = nullptr;

    // Tab row: always visible strip containing the Recent pill + doc tabs.
    QWidget *tabRow_ = nullptr;
    mervin::DetachableTabBar *docTabBar_ = nullptr; // visible tab bar, synced with tabs_

    // Toolbar doc-only controls (hidden in recent mode).
    QPushButton *recentBtn_ = nullptr;
    QWidget *docControls_ = nullptr;
    QToolBar *mainToolBar_ = nullptr;   // the top button row; sets the window's min width
    QToolButton *moreButton_ = nullptr; // ⋯ popup holding the former menu-bar entries
    QToolButton *saveButton_ = nullptr; // dropdown-only Save, right of Open
    QMenu *saveMenu_ = nullptr;         // its dropdown (Save edits / as copy / export)
    QToolButton *documentButton_ = nullptr; // dropdown-only Document button, right of Save
    QMenu *documentMenu_ = nullptr;         // its popover (page operations + Security)
    QToolButton *comfortButton_ = nullptr;  // moon/sun toggle: Comfort <-> Traditional theme

    QLineEdit *pageEdit_ = nullptr;
    QLabel *pageCountLabel_ = nullptr;
    QComboBox *zoomCombo_ = nullptr;
    QLabel *statusInfo_ = nullptr;
};
