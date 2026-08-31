#pragma once

#include "config/Settings.h"
#include "recent/ViewState.h"
#include "render/MeasureContent.h"
#include "ui/ViewerWidget.h"

#include <QMainWindow>

#include <memory>
#include <vector>

namespace mervin {
class CommentsSidebar;
class ContentSearch;
class DetachableTabBar;
class Document;
class FindBar;
class OutlineSidebar;
class RecentFilesPanel;
class RenderEngine;
class TabPage;
class ThumbnailSidebar;
class WindowManager;
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

    // Opens a PDF in a new tab, or focuses the existing tab if the file is
    // already open in any window of this process. When allowDuplicate is true
    // the focus-existing-tab dedup is skipped, so the file opens as a second,
    // independent view even if it is already open elsewhere ("Duplicate to new
    // window"); the caller seeds the new tab's view state itself.
    //
    // atIndex places the new tab at a specific position (-1 = append), and
    // makeCurrent=false opens it without pulling focus off the tab on screen.
    // Both exist for the staged session restore, which opens the previously
    // active document first and then slots the remaining documents back into
    // their saved positions around it without ever stealing the view.
    bool openFile(const QString &path, bool allowDuplicate = false, int atIndex = -1,
                  bool makeCurrent = true);

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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override; // tab-bar middle-click
    void closeEvent(QCloseEvent *event) override;               // persist geometry/settings
    void changeEvent(QEvent *event) override;                   // report activation to WindowManager
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
    void closeAllTabs();      // tab right-click menu
    void reopenClosedTab();   // Ctrl+Shift+T: bring back the last tab closed
    void onTabDetachRequested(int index, const QPoint &globalPos);
    void onTabMergeRequested(mervin::DetachableTabBar *source, int sourceIndex, int targetIndex);
    void onOcrRegionSelected(int pageNo, const QRectF &pageRect);
    void toggleMeasure(bool on);
    void toggleForms(bool on);   // Fill-Forms tool (Ctrl+Shift+F)
    void toggleComment(bool on); // Comment tool window (Ctrl+Shift+N): highlight + comment
    void onCalibrationLineDrawn(int pageNo, double lengthPoints);
    void onSetScaleRequested(int pageNo); // "Set Scale" button: manual ratio (1 : N)
    void toggleFullScreen(bool on);
    void toggleAlwaysOnTop(bool on);
    void openSettings();

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
    QList<int> askPageRange(const QString &title, int pageCount);
    QStringList askForPdfs();              // app picker: local multi-select or typed URL
    void openUrl(const QUrl &url, bool inNewWindow = false); // download, then open its cache file
    void offerToOpen(const QString &path); // ask whether to open a written file
    // Gather a viewer's committed measurements + per-page overrides for saving
    // (the Mervin blob), or paired with each page's PDF transform + value label
    // for export/print rendering.
    mervin::MeasureDoc collectMeasureDoc(mervin::ViewerWidget *viewer) const;
    std::vector<mervin::RenderMeasurement> collectRenderMeasurements(
        mervin::ViewerWidget *viewer) const;
    void applyControlStyle(); // theme-aware borders for toolbar / find-bar buttons
    // Re-assert WA_Hover on the chrome buttons after a theme switch re-polishes
    // them (a dropped WA_Hover leaves a button stuck without its :hover highlight).
    void reassertHoverAttributes();
    // Windows 11: colour the native caption to the slate title-bar tone while
    // the UI theme is dark; reset to the DWM default in light. No-op elsewhere.
    void applyTitleBarTheme();
    // How PDF pages should be toned, given the current document theme setting
    // ("light"/"dark"/"comfort"; legacy "follow-ui" resolves against the live
    // palette).
    mervin::PageTheme computeDocumentPageTheme() const;
    void applyDocumentThemeToViewers(); // re-apply the document theme to every tab
    void syncComfortButton();       // moon/sun glyph + tooltip on the toolbar toggle
    void showRecentPanel();         // switch to Recent view (called by pill button)
    void setCommandBarMode(bool recentActive); // hide/show doc-only toolbar widget
    void updateRecentButton();      // sync pill button accent state
    void syncDocTabBar();           // rebuild docTabBar_ to match tabs_
    void updateTabGlyphs();         // tint each doc tab's glyph (accent on the active tab)
    void applyActionIcons();        // set the house icons on every action (theme-aware)
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
    // already recorded the whole bar through rememberOpenTabs().
    void closeTabAt(int index, bool remember);
    // Push one tab onto the reopen history. `index` is the slot to restore it to,
    // which mid-bulk-close is NOT its live index - see rememberOpenTabs().
    void rememberClosedTab(mervin::TabPage *tab, int index);
    // Record every open tab for reopening, before a bulk close (Close all tabs,
    // or the window itself going away).
    void rememberOpenTabs();

    mervin::TabPage *currentTab() const;
    mervin::ViewerWidget *currentViewer() const;
    int indexOfPath(const QString &canonicalPath) const;

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
    QAction *copyAction_ = nullptr;
    QAction *selectAllAction_ = nullptr;
    QAction *ocrAction_ = nullptr;
    QAction *measureAction_ = nullptr;
    QAction *formAction_ = nullptr;                  // Fill Forms (checkable, Ctrl+Shift+F)
    QAction *highlightFormFieldsAction_ = nullptr;   // More-menu toggle (mirrors the setting)
    QAction *commentAction_ = nullptr;               // Comment tool window (checkable, Ctrl+Shift+N)
    QAction *zoomInAction_ = nullptr;
    QAction *zoomOutAction_ = nullptr;
    QAction *fitModeAction_ = nullptr; // toggles Fit Page <-> Fit Width
    QAction *rotateLeftAction_ = nullptr;
    QAction *rotateRightAction_ = nullptr;
    QAction *printAction_ = nullptr;
    QAction *fullScreenAction_ = nullptr;
    QAction *alwaysOnTopAction_ = nullptr;
    QAction *settingsAction_ = nullptr;
    QAction *fitPageAction_ = nullptr;           // menu View: Fit Page
    QAction *fitWidthAction_ = nullptr;          // menu View: Fit Width
    QAction *keyboardShortcutsAction_ = nullptr; // menu App: Keyboard Shortcuts
    QAction *aboutAction_ = nullptr;             // menu App: About Mervin PDF
    QAction *continuousAction_ = nullptr;
    QAction *singleAction_ = nullptr;
    QAction *twoPageAction_ = nullptr;

    // Global adaptive find/search bar (below the tab row, always visible).
    mervin::FindBar *findBar_ = nullptr;

    // Tab row: always visible strip above find bar containing Recent pill + doc tabs.
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
    QToolButton *ocrButton_ = nullptr;      // wide wordmark; larger than square glyph slots
    QToolButton *comfortButton_ = nullptr;  // moon/sun toggle: Comfort <-> Traditional theme

    QLineEdit *pageEdit_ = nullptr;
    QLabel *pageCountLabel_ = nullptr;
    QComboBox *zoomCombo_ = nullptr;
    QLabel *statusInfo_ = nullptr;
};
