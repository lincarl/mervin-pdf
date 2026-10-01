#include "security/DocumentOutput.h"
#include <QTemporaryFile>
#include <QThreadPool>
#include "ui/MainWindow.h"

#include "app/WindowManager.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/CalibrationDialog.h"
#include "dialogs/ExportMeasureDialog.h"
#include "dialogs/ExtractDialog.h"
#include "dialogs/ManageLanguagesDialog.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/OcrPopup.h"
#include "dialogs/PrintDialog.h"
#include "dialogs/SecurityDialog.h"
#include "dialogs/SettingsDialog.h"
#include "ui/Icons.h"
#include "ui/MeasurePanel.h"
#include "ui/Theme.h"
#include "ui/ThemeTokens.h"
#include "ocr/TessdataManager.h"
#include "recent/RecentEntry.h"
#include "net/UrlOpen.h"
#include "net/UrlDownloadLocation.h"
#include "render/ContentSearch.h"
#include "render/Document.h"
#include "render/FormModel.h"
#include "render/MeasureContent.h"
#include "render/MeasureMath.h"
#include "render/OcrService.h"
#include "render/RenderEngine.h"
#include "security/MeasureExport.h"
#include "security/PageOps.h"
#include "ui/AnnotPanel.h"
#include "ui/CommentsSidebar.h"
#include "ui/DetachableTabBar.h"
#include "ui/FileContextMenu.h"
#include "ui/FindBar.h"
#include "ui/OutlineSidebar.h"
#include "ui/OpenPdfDialog.h"
#include "ui/SidebarLayout.h"
#include "ui/RecentFilesPanel.h"
#include "ui/TabPage.h"
#include "ui/ThumbnailSidebar.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QTabBar>
#include <QMimeData>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPrintDialog>
#include <QPrinter>
#include <QAbstractButton>
#include <QPushButton>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSet>
#include <QToolButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <limits>

#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

using mervin::ViewLayout;

using mervin::RecentFilesPanel;
using mervin::TabPage;
using mervin::ViewerWidget;

using mervin::Document;
using mervin::ExportMeasureDialog;
using mervin::Measurement;
using mervin::MeasureDoc;
using mervin::MeasureExport;
using mervin::MeasureModel;
using mervin::MeasureScale;
using mervin::RenderMeasurement;

namespace {
// Use a cosmetic pen for exactly one device pixel at fractional scaling. applyControlStyle
// supplies lineColor.
class ToolSeparator : public QWidget
{
public:
    explicit ToolSeparator(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("toolSep"));
        // A few px wide so the centered cosmetic line is always well inside the
        // widget's clip rect. A 1px-wide widget put the line on the very edge,
        // where fractional scaling (125%) clipped it away for some dividers.
        setFixedSize(5, 20);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QColor c = property("lineColor").value<QColor>();
        if (!c.isValid())
            c = mervin::theme::chrome(palette()).hairline;
        QPainter p(this);
        p.setPen(QPen(c, 0)); // width 0 == cosmetic pen: always 1 device pixel
        const int x = width() / 2; // centered: clears the clip edges at any DPI
        p.drawLine(x, 0, x, height());
    }
};

// Draw the Lucide chevron in the split-button menu strip; QSS suppresses the native arrow and
// defines the matching strip width.
class SplitToolButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        if (menu() == nullptr)
            return;
        constexpr int kMenuWidth = 18; // matches ::menu-button width in applyControlStyle()
        const QColor ink = mervin::Theme::iconInk(palette());
        const QRect arrow(width() - kMenuWidth, 0, kMenuWidth, height());
        QPainter p(this);
        const int px = qMax(8, height() * 40 / 100);
        const QPixmap chevron =
            mervin::icons::glyphPixmap(mervin::icons::Glyph::ChevronDown, ink, px);
        p.drawPixmap(arrow.center().x() - px / 2, arrow.center().y() - px / 2, chevron);
    }
};

// Paint right-edge accent checks for actions tagged "rightCheck". Qt uses the left column for
// either an icon or a check, so icon-bearing toggles need this separate mark and reserved
// padding. Checked rows receive no background tint. Resolve the accent on every popup to
// reflect changes in other windows.
class RightCheckMenu : public QMenu
{
public:
    explicit RightCheckMenu(QWidget *parent = nullptr)
        : QMenu(parent)
    {
        // Reserve the check column (see the note above). The first popup is the
        // earliest point where sizeHint() reflects the polished style metrics,
        // and aboutToShow still runs before the popup geometry is computed.
        // minimumWidth is 0 until then, so the reserve is added exactly once and
        // cannot compound across popups.
        connect(this, &QMenu::aboutToShow, this, [this] {
            if (minimumWidth() == 0)
                setMinimumWidth(sizeHint().width() + kCheckColumn);
        });
    }

protected:
    void changeEvent(QEvent *event) override
    {
        // A new style or font re-measures every row: drop the reserve so the next
        // popup recomputes it against the new metrics.
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)
            setMinimumWidth(0);
        QMenu::changeEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        // Resolve against the APPLICATION palette, not the menu's own: a popup
        // receives no PaletteChange while unshown (and the Win11 style polishes
        // popups with its own palette), so palette() goes stale across theme
        // switches. qApp's palette is what Theme::applyApp() keeps current.
        const QPalette appPal = QApplication::palette();
        checkColor_ = mervin::Theme::accentColor(mervin::Settings::load().accentColor,
                                                 appPal);
        QMenu::showEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QMenu::paintEvent(event);
        const QColor color = checkColor_.isValid()
                                 ? checkColor_
                                 : palette().color(QPalette::Accent);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        constexpr int sz = kCheckSize;
        p.setPen(QPen(color, sz / 7.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (QAction *a : actions()) {
            if (!a->isChecked() || !a->property("rightCheck").toBool())
                continue;
            const QRect r = actionGeometry(a);
            if (!r.isValid())
                continue;
            // Right-aligned inside the item's horizontal padding.
            const QPointF org(r.right() - kCheckInset - sz, r.top() + (r.height() - sz) / 2.0);
            const double s = sz / 16.0;
            QPainterPath check(org + QPointF(3.2 * s, 8.6 * s));
            check.lineTo(org + QPointF(6.6 * s, 12.0 * s));
            check.lineTo(org + QPointF(12.8 * s, 4.8 * s));
            p.drawPath(check);
        }
    }

private:
    static constexpr int kCheckSize = 15;  // matches the QSS QMenu::indicator size
    static constexpr int kCheckInset = 10; // the row's right padding, from the QSS
    // Width added to the popup so the check clears the longest label with a gap.
    static constexpr int kCheckColumn = kCheckSize + 8;

    QColor checkColor_; // refreshed in showEvent; falls back to QPalette::Accent
};

// Hide menu shortcut hints while retaining QAction bindings and window registration. Shortcuts
// remain listed in the Keyboard Shortcuts dialog. Qt applies this flag only to context-menu
// chains; these popups do not originate from a QMenuBar.
void hideShortcutHints(QMenu *menu)
{
    for (QAction *a : menu->actions()) {
        if (QMenu *sub = a->menu())
            hideShortcutHints(sub);
        a->setShortcutVisibleInContextMenu(false);
    }
}
} // namespace

MainWindow::MainWindow(mervin::RenderEngine *engine, mervin::WindowManager *wm, QWidget *parent)
    : QMainWindow(parent)
    , engine_(engine)
    , wm_(wm)
{
    settings_ = mervin::Settings::load();

    // The QTabWidget hosts the live TabPage content; its own tab bar is hidden
    // (docTabBar_ below is the visible bar and the actual drag-detach/merge
    // surface - see further down). The drag-out-to-detach / drop-to-merge
    // gestures (M9) are wired on docTabBar_, not here.
    auto *tabWidget = new mervin::DetachableTabWidget;
    tabs_ = tabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);

    // Start page (recent files) shown when the window holds no document.
    recentPanel_ = new RecentFilesPanel;
    recentPanel_->setVisibleCount(settings_.recentVisibleCount);
    connect(recentPanel_, &RecentFilesPanel::openRequested, this,
            [this](const QString &path) { openFile(path); });
    connect(recentPanel_, &RecentFilesPanel::openInNewWindowRequested, this,
            [this](const QString &path) {
                if (wm_)
                    wm_->openPaths({path}, QStringLiteral("new-window"));
            });
    connect(recentPanel_, &RecentFilesPanel::removeRequested, this, [this](const QString &path) {
        if (wm_)
            wm_->removeRecent(path);
    });
    connect(recentPanel_, &RecentFilesPanel::clearMissingRequested,
            this, [this](const QStringList &paths) {
                if (wm_)
                    wm_->clearMissingRecent(paths);
            });
    connect(recentPanel_, &RecentFilesPanel::favoriteToggled,
            this, [this](const QString &path, bool isFav) {
                if (wm_)
                    wm_->setFavorite(path, isFav);
            });
    if (wm_) {
        connect(wm_, &mervin::WindowManager::recentListChanged, this,
                [this](const QList<mervin::RecentEntry> &entries) {
                    recentPanel_->setEntries(entries);
                });
        recentPanel_->setEntries(wm_->recentEntries()); // seed from the cached snapshot
    }

    // On-demand content search over the recent history (M7). The panel is a pure
    // view; this window owns the MuPDF-backed search and routes hits back to it.
    contentSearch_ = new mervin::ContentSearch(engine_, this);
    connect(recentPanel_, &RecentFilesPanel::contentSearchRequested, this,
            [this](const QString &query, bool favoritesOnly) {
                QStringList paths;
                if (wm_) {
                    const auto entries = wm_->recentEntries(); // full history, newest first
                    paths.reserve(entries.size());
                    for (const mervin::RecentEntry &e : entries) {
                        if (favoritesOnly && !e.favorite)
                            continue;
                        paths.append(e.path);
                    }
                }
                contentSearch_->start(paths, query);
            });
    connect(recentPanel_, &RecentFilesPanel::contentSearchCanceled,
            contentSearch_, &mervin::ContentSearch::cancel);
    connect(contentSearch_, &mervin::ContentSearch::hit,
            recentPanel_, &RecentFilesPanel::addContentHit);
    connect(contentSearch_, &mervin::ContentSearch::progress,
            recentPanel_, &RecentFilesPanel::setContentProgress);
    connect(contentSearch_, &mervin::ContentSearch::finished, this,
            [this](bool canceled, int matched) { recentPanel_->endContentSearch(canceled, matched); });

    stack_ = new QStackedWidget(this);
    stack_->addWidget(recentPanel_); // index 0
    stack_->addWidget(tabs_);        // index 1

    // Global adaptive find/search bar.
    findBar_ = new mervin::FindBar(this);
    connect(findBar_, &mervin::FindBar::recentFilterChanged, this,
            [this](const QString &text, bool contentSearch) {
                recentPanel_->setSearch(text, contentSearch);
            });
    // The recent listing's one-line summary ("Your last N opened documents")
    // lives in the status bar - the same place that shows the open file's path.
    // Only reflect it there while the Recent view is the active page.
    connect(recentPanel_, &mervin::RecentFilesPanel::statusSummaryChanged, this,
            [this](const QString &text) {
                if (statusInfo_ && recentActive_)
                    statusInfo_->setText(text);
            });

    // Permanent tab row: Recent pill + visible QTabBar.
    // This row stays visible at all times (Recent view or document view).
    // The QTabWidget's own built-in tab bar is hidden so it does not double up.
    tabRow_ = new QWidget(this);
    tabRow_->setObjectName(QStringLiteral("tabRowWidget"));
    tabRow_->setFixedHeight(46);
    auto *tabRowLayout = new QHBoxLayout(tabRow_);
    tabRowLayout->setContentsMargins(8, 6, 8, 6);
    tabRowLayout->setSpacing(0);

    recentBtn_ = new QPushButton(tr("Recent"), tabRow_);
    recentBtn_->setObjectName(QStringLiteral("recentPillBtn"));
    recentBtn_->setFlat(true);
    connect(recentBtn_, &QPushButton::clicked, this, &MainWindow::showRecentPanel);
    tabRowLayout->addWidget(recentBtn_);
    tabRowLayout->addSpacing(8);

    // The visible bar IS a DetachableTabBar (M9): dragging a tab out of it
    // detaches to a new window, and dropping it on another window's bar merges
    // it there - i.e. tabs can be dragged between windows. The QTabWidget's own
    // (hidden) bar is just the content host; this is the surface the user drags.
    docTabBar_ = new mervin::DetachableTabBar(tabRow_);
    docTabBar_->setObjectName(QStringLiteral("docTabBar"));
    docTabBar_->setTabsClosable(true);
    docTabBar_->setMovable(true);
    docTabBar_->setExpanding(false);
    docTabBar_->setDocumentMode(true);
    docTabBar_->setDrawBase(false);
    connect(docTabBar_, &mervin::DetachableTabBar::detachRequested, this,
            &MainWindow::onTabDetachRequested);
    connect(docTabBar_, &mervin::DetachableTabBar::mergeRequested, this,
            &MainWindow::onTabMergeRequested);
    docTabBar_->installEventFilter(this); // middle-click to close
    // Built-in in-bar reordering moves only the visible bar; mirror it onto the
    // real tab widget so their indices stay aligned (tabs_ is the source of
    // truth).
    connect(docTabBar_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from == to)
            return;
        {
            QSignalBlocker bd(docTabBar_);
            QSignalBlocker bt(tabs_);
            // Keep the internal tab-bar signal active: QTabWidget needs it to reorder its pages.
            // Block only the mirrored visible bar to avoid duplicate moves.
            tabs_->tabBar()->moveTab(from, to);
            tabs_->setCurrentIndex(docTabBar_->currentIndex());
        }
        // QTabBar emits currentChanged AND tabMoved during an interactive move;
        // if currentChanged ran first it wired the toolbar to a stale index
        // (tabs_ was not yet reordered). Reconcile from the now-correct state so
        // the current viewer / sidebars match the tab actually shown.
        updateForCurrentTab();
        if (wm_)
            wm_->updateSession();
    });
    connect(docTabBar_, &QTabBar::currentChanged, this, [this](int idx) {
        if (idx < 0) return;
        recentActive_ = false;
        QSignalBlocker b(tabs_);
        tabs_->setCurrentIndex(idx);
        updateForCurrentTab();
    });
    // tabBarClicked fires even when the already-selected tab is clicked, which
    // handles the case where the user switches to Recent and then clicks the same
    // tab that was active before - currentChanged won't fire in that case.
    connect(docTabBar_, &QTabBar::tabBarClicked, this, [this](int idx) {
        if (!recentActive_ || idx < 0) return;
        recentActive_ = false;
        QSignalBlocker b(tabs_);
        tabs_->setCurrentIndex(idx);
        updateForCurrentTab();
    });
    connect(docTabBar_, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
    docTabBar_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(docTabBar_, &QTabBar::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int idx = docTabBar_->tabAt(pos);
        if (idx < 0) return;
        auto *t = qobject_cast<TabPage *>(tabs_->widget(idx));
        if (!t) return;
        const QString dupPath = t->path();
        const QString dupPassword = t->password(); // the copy opens without asking
        const mervin::ViewState dupState =
            t->viewer() ? captureViewState(t->viewer()) : mervin::ViewState{};
        const QColor ink = mervin::Theme::iconInk(palette());
        mervin::showFileContextMenu(
            this, t->path(), docTabBar_->mapToGlobal(pos),
            {{tr("Duplicate to new window"),
              [this, dupPath, dupPassword, dupState] {
                  if (wm_)
                      wm_->duplicateToNewWindow(dupPath, dupPassword, dupState,
                                                this->pos() + QPoint(40, 40));
              },
              true,
              mervin::icons::glyph(mervin::icons::Glyph::ShowAllWindows, ink)},
             {tr("Move to new window"),
              [this, idx] { onTabDetachRequested(idx, this->pos() + QPoint(40, 40)); },
              tabs_->count() > 1,
              mervin::icons::glyph(mervin::icons::Glyph::OpenInNewWindow, ink)},
             {tr("Close all tabs"),
              [this] { closeAllTabs(); },
              tabs_->count() > 0,
              mervin::icons::glyph(mervin::icons::Glyph::Close, ink)}});
    });
    tabRowLayout->addWidget(docTabBar_, 1);

    // Hide the QTabWidget's internal tab bar; docTabBar_ is the visible one.
    tabs_->tabBar()->hide();
    tabs_->tabBar()->setMaximumHeight(0);

    auto *centralContainer = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(centralContainer);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(tabRow_);   // tab row is always visible - above find bar
    centralLayout->addWidget(findBar_);
    centralLayout->addWidget(stack_, 1);
    setCentralWidget(centralContainer);

    createSidebars(); // before createMenus so their toggle actions exist
    createActions();
    createToolBar();
    createMenus();
    applyControlStyle();

    // Don't let the window shrink narrower than the toolbar's natural width, or
    // its controls would collapse into an overflow ">>" menu. The document
    // controls stay visible on the Recent view too (greyed out, not hidden), so
    // the toolbar keeps this width in both modes.
    mainToolBar_->ensurePolished();
    setMinimumWidth(mainToolBar_->sizeHint().width() + 24);

    statusInfo_ = new QLabel(this);
    statusBar()->addWidget(statusInfo_);

    setAcceptDrops(true); // drag-and-drop to open (M11)

    if (wm_) {
        connect(wm_, &mervin::WindowManager::colorSchemeChanged, this,
                [this](const QString &scheme) {
                    settings_.colorScheme = scheme;
                    // Defer icon and document-theme refresh until the application palette and stylesheet settle.
                    QTimer::singleShot(0, this, [this] {
                        applyControlStyle();
                        applyDocumentThemeToViewers();
                    });
                });
        connect(wm_, &mervin::WindowManager::documentThemeChanged, this,
                [this](const QString &theme) {
                    settings_.documentTheme = theme;
                    syncComfortButton(); // moon <-> sun on the toolbar toggle
                    applyDocumentThemeToViewers();
                });
        // Never (in the update prompt) or another window's Settings switched
        // auto update: keep this copy current so a later save doesn't undo it.
        connect(wm_, &mervin::WindowManager::autoUpdateChanged, this,
                [this](bool on) { settings_.autoUpdate = on; });
    }

    setWindowTitle(tr("Mervin PDF"));
    if (!settings_.windowGeometry.isEmpty())
        restoreGeometry(settings_.windowGeometry);
    else
        resize(1100, 820);
    if (!settings_.windowState.isEmpty())
        restoreState(settings_.windowState);
    mervin::dockSidebarsOnLeft(*this, {thumbnailDock_, outlineDock_, commentsDock_});
    updateForCurrentTab();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < tabs_->count(); ++i) {
        if (!confirmClose(qobject_cast<TabPage *>(tabs_->widget(i)))) {
            event->ignore();
            return;
        }
    }
    saveAllViewStates(); // resume works after a clean window close, not just per-tab

    // A closing window takes its tabs with it as plain child widgets, never
    // through closeTab(), so record them for Ctrl+Shift+T here: pressing it in a
    // surviving window brings them back one at a time. When this is the last
    // window the process exits and the history goes with it, which is correct -
    // restoring THAT is what the session file is for.
    rememberOpenTabs();

    // Don't persist the temporary fullscreen geometry.
    if (!isFullScreen()) {
        mervin::dockSidebarsOnLeft(*this, {thumbnailDock_, outlineDock_, commentsDock_});
        settings_.windowGeometry = saveGeometry();
        settings_.windowState = saveState();
    }
    settings_.save();
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow() = default;
// The engine is owned by WindowManager (process-level) and outlives every
// window; it stops the render workers before the last window's Documents are
// freed and drops the MuPDF context last. Tabs (and their Documents) are
// destroyed here as ordinary child widgets, with the engine still alive.

mervin::TabPage *MainWindow::currentTab() const
{
    return tabs_ ? qobject_cast<TabPage *>(tabs_->currentWidget()) : nullptr;
}

ViewerWidget *MainWindow::currentViewer() const
{
    TabPage *t = currentTab();
    return t ? t->viewer() : nullptr;
}

int MainWindow::indexOfPath(const QString &canonicalPath) const
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *t = qobject_cast<TabPage *>(tabs_->widget(i)))
            if (t->canonicalPath() == canonicalPath)
                return i;
    return -1;
}

bool MainWindow::focusTabIfOpen(const QString &canonicalPath)
{
    const int idx = indexOfPath(canonicalPath);
    if (idx < 0)
        return false;
    recentActive_ = false;       // showing a document leaves the Recent view
    tabs_->setCurrentIndex(idx); // switch content (no signal if idx is unchanged)
    syncDocTabBar();             // keep the visible tab bar's selection in sync
    updateForCurrentTab();       // refresh even when the index did not change
    return true;
}

int MainWindow::tabCount() const
{
    return tabs_ ? tabs_->count() : 0;
}

QString MainWindow::currentTabPath() const
{
    TabPage *t = currentTab();
    return t ? t->canonicalPath() : QString();
}

QStringList MainWindow::tabPaths() const
{
    QStringList paths;
    if (!tabs_)
        return paths;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *t = qobject_cast<TabPage *>(tabs_->widget(i)))
            paths << t->canonicalPath();
    return paths;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime->hasUrls())
        return;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (!mime->hasUrls())
        return;
    bool opened = false;
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
            openFile(path);
            opened = true;
        }
    }
    if (opened)
        event->acceptProposedAction();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && wm_)
        wm_->notifyActivated(this);
    if (event->type() == QEvent::PaletteChange) {
        applyControlStyle();
        // A "follow-ui" document theme tracks the UI theme; the palette has now
        // settled, so this is where the page tint follows a light/dark switch.
        applyDocumentThemeToViewers();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::createActions()
{
    openAction_ = new QAction(tr("&Open"), this);
    openAction_->setShortcut(QKeySequence::Open); // Ctrl+O
    connect(openAction_, &QAction::triggered, this, &MainWindow::onOpen);

    // Dropdown variant on the split Open button: pick a file, open in a new window.
    openInNewWindowAction_ = new QAction(tr("Open in New Window"), this);
    connect(openInNewWindowAction_, &QAction::triggered, this, &MainWindow::onOpenInNewWindow);

    // Dropdown items on the split Save button (created in createToolBar).
    saveEditsAction_ = new QAction(tr("Save edits"), this);
    saveEditsAction_->setShortcut(QKeySequence::Save); // Ctrl+S
    saveEditsAction_->setToolTip(
        tr("Save measurements, annotations and filled form fields into this file"));
    connect(saveEditsAction_, &QAction::triggered, this, &MainWindow::saveMeasurements);
    // The action lives only in the Save button's popup menu; register it on the
    // window too so Ctrl+S fires even while that menu is closed.
    addAction(saveEditsAction_);

    saveAsCopyAction_ = new QAction(tr("Save as copy…"), this);
    saveAsCopyAction_->setToolTip(tr("Save the document with its edits to a new file"));
    connect(saveAsCopyAction_, &QAction::triggered, this, &MainWindow::saveAsCopy);

    exportMeasurementsAction_ = new QAction(tr("Export with measurements…"), this);
    exportMeasurementsAction_->setToolTip(
        tr("Export a copy with the measurements burned in as non-editable marks"));
    connect(exportMeasurementsAction_, &QAction::triggered, this, &MainWindow::exportMeasuredCopy);

    newWindowAction_ = new QAction(tr("New &Window"), this);
    newWindowAction_->setShortcut(QKeySequence::New); // Ctrl+N
    connect(newWindowAction_, &QAction::triggered, this, [this] {
        if (wm_)
            wm_->newWindow();
    });

    newTabAction_ = new QAction(tr("&New Tab"), this);
    newTabAction_->setShortcut(QKeySequence::AddTab); // Ctrl+T
    connect(newTabAction_, &QAction::triggered, this, &MainWindow::onOpen);

    closeTabAction_ = new QAction(tr("&Close Tab"), this);
    // Bind Ctrl+W explicitly (QKeySequence::Close is Ctrl+F4 on Windows); keep
    // Ctrl+F4 too. The spec lists Ctrl+W for close.
    closeTabAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_W),
                                   QKeySequence(Qt::CTRL | Qt::Key_F4)});
    connect(closeTabAction_, &QAction::triggered, this, [this] {
        if (tabs_->currentIndex() >= 0)
            closeTab(tabs_->currentIndex());
    });

    // Ctrl+Shift+T restores the newest closed document from the process-wide history.
    reopenTabAction_ = new QAction(tr("Reopen Closed &Tab"), this);
    reopenTabAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(reopenTabAction_, &QAction::triggered, this, &MainWindow::reopenClosedTab);
    addAction(reopenTabAction_);

    nextTabAction_ = new QAction(tr("Next Tab"), this);
    nextTabAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab));
    connect(nextTabAction_, &QAction::triggered, this, [this] {
        const int n = tabs_->count();
        if (n > 1)
            tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % n);
    });

    prevTabAction_ = new QAction(tr("Previous Tab"), this);
    prevTabAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab));
    connect(prevTabAction_, &QAction::triggered, this, [this] {
        const int n = tabs_->count();
        if (n > 1)
            tabs_->setCurrentIndex((tabs_->currentIndex() - 1 + n) % n);
    });
    // No longer in a menu, so register on the window to keep the shortcuts live.
    addAction(nextTabAction_);
    addAction(prevTabAction_);

    prevPageAction_ = new QAction(tr("Previous"), this);
    prevPageAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    connect(prevPageAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->prevPage();
    });

    nextPageAction_ = new QAction(tr("Next"), this);
    nextPageAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    connect(nextPageAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->nextPage();
    });

    findAction_ = new QAction(tr("&Find"), this);
    findAction_->setShortcut(QKeySequence::Find); // Ctrl+F
    connect(findAction_, &QAction::triggered, this, [this] {
        // In document mode, seed from the current selection; in recent mode
        // just focus the field so the user can type a search query.
        if (findBar_) {
            QString preset;
            if (!recentActive_) {
                if (auto *t = currentTab()) {
                    const QString sel = t->viewer()->selectedText();
                    if (!sel.isEmpty()) {
                        preset = sel.section(QLatin1Char('\n'), 0, 0).trimmed();
                        if (preset.size() > 100)
                            preset.clear();
                    }
                }
            }
            findBar_->activate(preset);
        }
    });

    findNextAction_ = new QAction(tr("Find &Next"), this);
    findNextAction_->setShortcut(QKeySequence::FindNext); // F3
    connect(findNextAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->findNext();
    });

    findPrevAction_ = new QAction(tr("Find &Previous"), this);
    findPrevAction_->setShortcut(QKeySequence::FindPrevious); // Shift+F3
    connect(findPrevAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->findPrev();
    });

    copyAction_ = new QAction(tr("&Copy"), this);
    copyAction_->setShortcut(QKeySequence::Copy); // Ctrl+C
    connect(copyAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->copySelection();
    });

    selectAllAction_ = new QAction(tr("Select &All"), this);
    selectAllAction_->setShortcut(QKeySequence::SelectAll); // Ctrl+A
    connect(selectAllAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->selectAll();
    });

    ocrAction_ = new QAction(tr("&OCR Selection"), this);
    ocrAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    ocrAction_->setToolTip(tr("OCR Selection: drag a region to recognise its text"));
    connect(ocrAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->setOcrMode(true); // next drag rubber-bands a region to OCR
    });

    measureAction_ = new QAction(tr("&Measure"), this);
    measureAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    measureAction_->setCheckable(true);
    connect(measureAction_, &QAction::toggled, this, &MainWindow::toggleMeasure);

    // Ctrl+Shift+F (consistent with Measure / OCR; avoids the AltGr = Ctrl+Alt
    // collision on European keyboards). Enabled only for documents with fields.
    formAction_ = new QAction(tr("Fill &Forms"), this);
    formAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    formAction_->setCheckable(true);
    connect(formAction_, &QAction::toggled, this, &MainWindow::toggleForms);

    // Comment tool: a single command-bar button that opens the floating Comment
    // window (which docks beside the measure panel). The window carries the
    // highlight (markup style + colour) and sticky-note (colour) settings and a
    // mode selector. Ctrl+Shift+N keeps the Ctrl+Shift tool family and avoids the
    // AltGr = Ctrl+Alt collision.
    commentAction_ = new QAction(tr("Comme&nt"), this);
    commentAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    commentAction_->setCheckable(true);
    commentAction_->setToolTip(tr("Comment: highlight text and add sticky-note comments"));
    connect(commentAction_, &QAction::toggled, this, &MainWindow::toggleComment);
    addAction(commentAction_); // keep the shortcut live even outside any menu

    highlightFormFieldsAction_ = new QAction(tr("Highlight Form Fields"), this);
    highlightFormFieldsAction_->setCheckable(true);
    highlightFormFieldsAction_->setProperty("rightCheck", true); // see RightCheckMenu
    highlightFormFieldsAction_->setChecked(settings_.highlightFormFields);
    connect(highlightFormFieldsAction_, &QAction::toggled, this, [this](bool on) {
        settings_.highlightFormFields = on;
        settings_.save();
        // Apply to every open viewer (the setting is window/app-wide).
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto *t = qobject_cast<TabPage *>(tabs_->widget(i)))
                t->viewer()->setHighlightFormFields(on);
    });

    zoomInAction_ = new QAction(tr("Zoom In"), this);
    zoomInAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Equal),
                                 QKeySequence(Qt::CTRL | Qt::Key_Plus)});
    connect(zoomInAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->zoomIn();
    });

    zoomOutAction_ = new QAction(tr("Zoom Out"), this);
    zoomOutAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(zoomOutAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->zoomOut();
    });

    // One-tap toggle between the two fit modes (the current mode is also shown
    // in the adjacent zoom combo). Any custom zoom snaps to Fit Page first.
    fitModeAction_ = new QAction(tr("Fit Page / Fit Width"), this);
    // Home toggles the two fit modes. (The viewer's bare-Home "go to first page"
    // moved to Ctrl+Home; this window-level shortcut intercepts the plain key.)
    fitModeAction_->setShortcut(QKeySequence(Qt::Key_Home));
    connect(fitModeAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->setZoomMode(v->zoomMode() == ViewerWidget::ZoomMode::FitPage
                               ? ViewerWidget::ZoomMode::FitWidth
                               : ViewerWidget::ZoomMode::FitPage);
    });

    rotateLeftAction_ = new QAction(tr("Rotate Left"), this);
    rotateLeftAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(rotateLeftAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->rotateLeft();
    });

    rotateRightAction_ = new QAction(tr("Rotate Right"), this);
    rotateRightAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(rotateRightAction_, &QAction::triggered, this, [this] {
        if (auto *v = currentViewer())
            v->rotateRight();
    });

    // How the reader moves through the document (mutually exclusive) and whether
    // pages are shown in facing pairs (an independent toggle). These are two
    // separate questions: asking for a spread must not throw away the scrolling
    // choice, and every one of the four combinations is a real reading mode.
    auto *scrollGroup = new QActionGroup(this);
    continuousAction_ = new QAction(tr("&Continuous Scroll"), scrollGroup);
    singleAction_ = new QAction(tr("&Single Page Scroll"), scrollGroup);
    continuousAction_->setData(static_cast<int>(ViewLayout::Scroll::Continuous));
    singleAction_->setData(static_cast<int>(ViewLayout::Scroll::Single));
    for (QAction *a : {continuousAction_, singleAction_}) {
        a->setCheckable(true);
        // These carry a left glyph icon, so the menu marks the active mode with
        // a right-side check instead of the (icon-suppressed) left indicator.
        a->setProperty("rightCheck", true);
        connect(a, &QAction::triggered, this, [this, a] {
            if (auto *v = currentViewer())
                v->setScrollMode(static_cast<ViewLayout::Scroll>(a->data().toInt()));
        });
    }
    continuousAction_->setChecked(true);

    twoPageAction_ = new QAction(tr("&Two-Page Spread"), this);
    twoPageAction_->setCheckable(true);
    twoPageAction_->setProperty("rightCheck", true);
    // triggered(), not toggled(), so syncViewActions can set the check state
    // without firing back into the viewer.
    connect(twoPageAction_, &QAction::triggered, this, [this](bool on) {
        if (auto *v = currentViewer())
            v->setSpread(on);
    });

    fullScreenAction_ = new QAction(tr("&Full Screen"), this);
    fullScreenAction_->setShortcut(QKeySequence(Qt::Key_F11));
    fullScreenAction_->setCheckable(true);
    connect(fullScreenAction_, &QAction::toggled, this, &MainWindow::toggleFullScreen);

    alwaysOnTopAction_ = new QAction(tr("Always on &Top"), this);
    alwaysOnTopAction_->setCheckable(true);
    alwaysOnTopAction_->setProperty("rightCheck", true); // icon-bearing, no shortcut
    connect(alwaysOnTopAction_, &QAction::toggled, this, &MainWindow::toggleAlwaysOnTop);

    settingsAction_ = new QAction(tr("&Settings"), this);
    settingsAction_->setShortcut(QKeySequence::Preferences);
    connect(settingsAction_, &QAction::triggered, this, &MainWindow::openSettings);

    printAction_ = new QAction(tr("&Print"), this);
    printAction_->setShortcut(QKeySequence::Print); // Ctrl+P
    connect(printAction_, &QAction::triggered, this, &MainWindow::printDocument);
}

void MainWindow::createToolBar()
{
    QToolBar *bar = addToolBar(tr("Main"));
    mainToolBar_ = bar;
    bar->setObjectName(QStringLiteral("mainToolBar")); // for saveState/restoreState
    bar->setMovable(false);
    // Most buttons are icon-only; Open keeps label beside icon.
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setIconSize(QSize(20, 20));

    // One consistent vertical divider used everywhere on the bar. ToolSeparator
    // paints a cosmetic 1px line so every divider stays crisp and identical even
    // at fractional display scaling (125% etc.); its colour is set in
    // applyControlStyle(). Earlier approaches (QFrame::VLine, then a stylesheet
    // background fill) rendered inconsistently under fractional scaling.
    auto makeToolSep = [](QWidget *parent) -> QWidget * {
        return new ToolSeparator(parent);
    };

    // Hamburger menu (☰): first button on the row, holds the former menu-bar
    // entries. Stays visible in Recent mode (docControls_ hides) so
    // File/Settings/Help remain reachable. Populated by createMenus(), next.
    moreButton_ = new QToolButton(this);
    moreButton_->setObjectName(QStringLiteral("menuButton")); // scopes the arrow-hiding QSS
    moreButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    moreButton_->setIconSize(QSize(20, 20));
    moreButton_->setPopupMode(QToolButton::InstantPopup);
    moreButton_->setToolTip(tr("Menu"));
    bar->addWidget(moreButton_);
    bar->addWidget(makeToolSep(bar)); // divider between the hamburger and Open

    // Open anchors the left side of the bar and keeps its text label. It is a
    // split button: clicking the main part opens a file as a new tab in this
    // window (openAction_); the dropdown arrow offers "Open in New Window".
    auto *openButton = new SplitToolButton(this);
    openButton->setObjectName(QStringLiteral("openButton")); // scopes the menu-section QSS
    openButton->setDefaultAction(openAction_);
    openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    openButton->setIconSize(QSize(20, 20));
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    auto *openMenu = new QMenu(openButton);
    openMenu->addAction(openInNewWindowAction_);
    openButton->setMenu(openMenu);
    bar->addWidget(openButton);

    // Save sits immediately right of Open and mirrors its split-button look, but is
    // dropdown-only: it carries no default action, so a click anywhere (main area or
    // chevron) just opens the menu - nothing is ever written by clicking the button
    // itself. The chevron is painted by SplitToolButton; the ::menu-button QSS
    // reserves the 18px strip for it, exactly as on Open.
    saveButton_ = new SplitToolButton(this);
    saveButton_->setObjectName(QStringLiteral("saveButton")); // scopes the menu-section QSS
    saveButton_->setText(tr("Save"));
    saveButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveButton_->setIconSize(QSize(20, 20));
    saveButton_->setPopupMode(QToolButton::MenuButtonPopup);
    saveButton_->setToolTip(tr("Save edits, save a copy, or export with measurements"));
    saveMenu_ = new QMenu(saveButton_);
    saveMenu_->addAction(saveEditsAction_);
    saveMenu_->addAction(saveAsCopyAction_);
    saveMenu_->addAction(exportMeasurementsAction_);
    hideShortcutHints(saveMenu_); // "Save edits" keeps Ctrl+S, just not on the row
    saveButton_->setMenu(saveMenu_);
    // A bare click on the main section opens the menu too (rather than triggering a
    // default action), so the whole button reads as "open the Save menu".
    connect(saveButton_, &QToolButton::clicked, saveButton_, &QToolButton::showMenu);
    // "Export with measurements" is only meaningful when the current document
    // actually carries measurements; hide it otherwise.
    connect(saveMenu_, &QMenu::aboutToShow, this, [this] {
        ViewerWidget *v = currentViewer();
        exportMeasurementsAction_->setVisible(v && v->hasMeasurements());
    });
    bar->addWidget(saveButton_);

    // Document sits immediately right of Save and mirrors its dropdown-only
    // split-button look. It opens the Document popover (page operations +
    // Security) - the design promotes these from a buried submenu to a first-
    // class toolbar button. createDocumentMenu() builds and attaches the menu.
    documentButton_ = new SplitToolButton(this);
    documentButton_->setObjectName(QStringLiteral("documentButton")); // scopes the menu-section QSS
    documentButton_->setText(tr("Document"));
    documentButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    documentButton_->setIconSize(QSize(20, 20));
    documentButton_->setPopupMode(QToolButton::MenuButtonPopup);
    documentButton_->setToolTip(tr("Rotate, delete, extract, split or merge pages; document security"));
    createDocumentMenu(); // populates documentMenu_ and calls setMenu()
    // A bare click on the main section opens the menu too (like Save).
    connect(documentButton_, &QToolButton::clicked, documentButton_, &QToolButton::showMenu);
    bar->addWidget(documentButton_);

    // Document-only controls: page nav + zoom + rotate.
    docControls_ = new QWidget(this);
    // Keep the group at its natural width so the toolbar never stretches it
    // (which would let the zoom combo expand and push the later buttons off-screen).
    docControls_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *dcLayout = new QHBoxLayout(docControls_);
    dcLayout->setContentsMargins(0, 0, 0, 0);
    dcLayout->setSpacing(4);

    auto addSep = [&]() { dcLayout->addWidget(makeToolSep(docControls_)); };

    auto addDocAction = [&](QAction *a) -> QToolButton * {
        auto *btn = new QToolButton(docControls_);
        btn->setDefaultAction(a);
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btn->setIconSize(QSize(20, 20));
        dcLayout->addWidget(btn);
        return btn;
    };

    addSep();
    addDocAction(prevPageAction_);

    pageEdit_ = new QLineEdit(docControls_);
    pageEdit_->setFixedWidth(48);
    pageEdit_->setAlignment(Qt::AlignCenter);
    pageEdit_->setValidator(new QIntValidator(1, 9999999, pageEdit_));
    connect(pageEdit_, &QLineEdit::returnPressed, this, &MainWindow::onPageEditReturn);
    dcLayout->addWidget(pageEdit_);

    pageCountLabel_ = new QLabel(QStringLiteral(" / 0 "), docControls_);
    dcLayout->addWidget(pageCountLabel_);

    addDocAction(nextPageAction_);
    addSep();
    addDocAction(zoomOutAction_);

    zoomCombo_ = new QComboBox(docControls_);
    // Named so the theme can trim this combo's padding-right (see Theme.cpp);
    // the shared 24px reserve is additive with the ::drop-down and made the box
    // needlessly wide.
    zoomCombo_->setObjectName(QStringLiteral("zoomCombo"));
    zoomCombo_->setEditable(true);
    zoomCombo_->setInsertPolicy(QComboBox::NoInsert);
    // Fixed width: never expand to eat the rest of the toolbar. This stays a
    // *minimum* rather than a fixed width so the box still grows with a larger
    // system font instead of clipping "Fit Width".
    zoomCombo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    zoomCombo_->setMinimumWidth(96);
    zoomCombo_->addItems({tr("Fit Page"), tr("Fit Width"),
                          QStringLiteral("50%"), QStringLiteral("75%"),
                          QStringLiteral("100%"), QStringLiteral("125%"),
                          QStringLiteral("150%"), QStringLiteral("200%")});
    zoomCombo_->setCurrentText(tr("Fit Width"));
    connect(zoomCombo_, &QComboBox::activated, this, &MainWindow::onZoomComboActivated);
    connect(zoomCombo_->lineEdit(), &QLineEdit::returnPressed,
            this, &MainWindow::onZoomComboActivated);
    // Centre the value. An editable combo paints its text through the embedded
    // line edit, which is left-aligned by default - that left "75%" hugging the
    // left border with a wide gap before the chevron. The line edit's rect
    // already excludes the ::drop-down subcontrol, so centring here centres the
    // text in the well it actually occupies and never slides under the arrow.
    zoomCombo_->lineEdit()->setAlignment(Qt::AlignCenter);
    // Keep the zoom editor read-only while unfocused: Qt otherwise retains its caret for
    // popup/window focus loss. eventFilter enables typing on focus.
    zoomCombo_->lineEdit()->setReadOnly(true);
    zoomCombo_->lineEdit()->installEventFilter(this);
    dcLayout->addWidget(zoomCombo_);

    addDocAction(zoomInAction_);
    addSep();
    addDocAction(fitModeAction_);
    addDocAction(rotateLeftAction_);
    addDocAction(rotateRightAction_);
    addSep();
    addDocAction(ocrAction_); // rubber-band a region and recognise its text
    addDocAction(measureAction_);
    addDocAction(commentAction_); // opens the Comment window (highlight + comment)
    addDocAction(printAction_);
    addSep();

    // Comfort document-theme toggle: one button, two faces. A crescent moon
    // while pages show as authored (click -> the Comfort dark theme), a sun
    // while Comfort is active (click -> back to Traditional).
    // syncComfortButton() keeps the glyph and tooltip current.
    comfortButton_ = new QToolButton(docControls_);
    comfortButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    comfortButton_->setIconSize(QSize(20, 20));
    connect(comfortButton_, &QToolButton::clicked, this, [this] {
        if (!wm_)
            return;
        const bool comfort = settings_.documentTheme == QLatin1String("comfort");
        wm_->setDocumentTheme(comfort ? QStringLiteral("light")
                                      : QStringLiteral("comfort"));
    });
    dcLayout->addWidget(comfortButton_);
    syncComfortButton();

    // Fill Forms is intentionally NOT on the command bar - it lives only in the
    // ⋯ menu (and Ctrl+Shift+F). The toolbar stays focused on the common actions.

    bar->addWidget(docControls_);
}

void MainWindow::createMenus()
{
    // The former menu bar (File / Edit / View / Document / Help) now lives in the
    // toolbar's ⋯ popup. Build it as that button's menu and hide the classic bar.
    // RightCheckMenu (not a plain QMenu) so the scroll-mode items can show their
    // active check on the right, beside their left glyph icons.
    QMenu *mainMenu = new RightCheckMenu(this);
    moreButton_->setMenu(mainMenu);
    menuBar()->hide();

    // Flat overflow menu: actions sit directly on the popup, grouped by
    // separators. This used to mirror a classic File/Edit/View/Document/Help
    // menu bar, but a single ☰ button reads better as one flat list than as
    // five hover-to-open submenus. Only Color Scheme and Document remain
    // submenus - a small radio group and a deep set of dialog-driven page ops.

    // Layout mirrors the design's Main Menu popover: a flat list grouped by
    // section headers (addSectionHeader), with an icon on every
    // command and a check on the toggle/layout items. Icons are assigned in
    // applyActionIcons() so they re-tint with the theme. Page operations are no
    // longer here - they moved to the toolbar's Document button.

    // Print sits at the very top (file-level), alone above the first group.
    mainMenu->addAction(printAction_);
    mainMenu->addSeparator();

    // View - fit presets, then the two window-level view toggles.
    addSectionHeader(mainMenu, tr("View"));
    fitPageAction_ = mainMenu->addAction(tr("Fit &Page"), QKeySequence(Qt::CTRL | Qt::Key_1), this, [this] {
        if (auto *v = currentViewer())
            v->setZoomMode(ViewerWidget::ZoomMode::FitPage);
    });
    fitWidthAction_ = mainMenu->addAction(tr("Fit &Width"), QKeySequence(Qt::CTRL | Qt::Key_2), this, [this] {
        if (auto *v = currentViewer())
            v->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    });
    mainMenu->addAction(fullScreenAction_);
    // Two-Page Spread belongs here and not in Scroll: it is an on/off view of the
    // document that composes with either scrolling mode, so it is a toggle rather
    // than a third row of the Scroll radio group.
    mainMenu->addAction(twoPageAction_);
    mainMenu->addAction(alwaysOnTopAction_);
    mainMenu->addSeparator();

    // Scroll - how pages advance, independent of the spread toggle above.
    addSectionHeader(mainMenu, tr("Scroll"));
    mainMenu->addAction(continuousAction_);
    mainMenu->addAction(singleAction_);
    mainMenu->addSeparator();

    // Panels - the navigation docks (icon + right-side check when shown).
    addSectionHeader(mainMenu, tr("Panels"));
    for (QDockWidget *d : {outlineDock_, thumbnailDock_, commentsDock_}) {
        if (!d)
            continue;
        QAction *a = d->toggleViewAction();
        a->setProperty("rightCheck", true); // icon-bearing, no shortcut
        mainMenu->addAction(a);
    }
    mainMenu->addSeparator();

    // Select & Annotate - selection plus the annotate / OCR / measure / forms tools.
    addSectionHeader(mainMenu, tr("Select & Annotate"));
    mainMenu->addAction(selectAllAction_);
    mainMenu->addAction(commentAction_);
    mainMenu->addAction(ocrAction_);
    mainMenu->addAction(measureAction_);
    mainMenu->addAction(formAction_);
    mainMenu->addAction(highlightFormFieldsAction_);
    mainMenu->addSeparator();

    // Document-theme defaults live in Settings; the toolbar toggles Traditional/Comfort. The
    // colour_scheme setting still controls chrome.
    addSectionHeader(mainMenu, tr("App"));
    mainMenu->addAction(settingsAction_);
    keyboardShortcutsAction_ = mainMenu->addAction(tr("&Keyboard Shortcuts"), this, &MainWindow::showShortcuts);
    aboutAction_ = mainMenu->addAction(tr("&About Mervin PDF"), this, &MainWindow::showAbout);

    // The classic menu bar is hidden, so the ⋯ popup is the only home for these
    // actions. A popup menu is not a visible widget, so its actions' shortcuts
    // would be inactive while it is closed - register them with the window too.
    std::function<void(QMenu *)> registerShortcuts = [&](QMenu *m) {
        for (QAction *a : m->actions()) {
            if (a->menu())
                registerShortcuts(a->menu());
            else if (!a->isSeparator() && !a->shortcut().isEmpty())
                addAction(a);
        }
    };
    registerShortcuts(mainMenu);

    // ...and only then drop the hints, so the walk above still sees the (intact)
    // key sequences it registers on the window.
    hideShortcutHints(mainMenu);

    // Register shortcut actions on the window even when they are absent from the menu.
    for (QAction *a : {newTabAction_, newWindowAction_, closeTabAction_, findAction_,
                       findNextAction_, findPrevAction_, copyAction_, zoomInAction_,
                       zoomOutAction_, rotateLeftAction_, rotateRightAction_,
                       printAction_})
        addAction(a);
}

mervin::PageTheme MainWindow::computeDocumentPageTheme() const
{
    // "light"/"dark"/"comfort" are the values Settings > Viewing offers
    // (Traditional / Inverted / Comfort). A legacy "follow-ui" config resolves
    // against the live palette, so those pages still render correctly.
    const QString &t = settings_.documentTheme;
    if (t == QLatin1String("light"))
        return mervin::PageTheme::Light;
    if (t == QLatin1String("dark"))
        return mervin::PageTheme::Inverted;
    if (t == QLatin1String("comfort"))
        return mervin::PageTheme::Comfort;
    // "follow-ui" (and any unrecognised value): invert only when the UI is dark.
    return mervin::theme::isDark(palette()) ? mervin::PageTheme::Inverted
                                            : mervin::PageTheme::Light;
}

void MainWindow::applyDocumentThemeToViewers()
{
    if (!tabs_)
        return;
    const mervin::PageTheme theme = computeDocumentPageTheme();
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *t = qobject_cast<TabPage *>(tabs_->widget(i)))
            if (auto *v = t->viewer())
                v->setPageTheme(theme);
}

void MainWindow::syncComfortButton()
{
    // Same button, two faces: a crescent moon while a light page is shown
    // (click to switch to the Comfort dark document theme), a sun while
    // Comfort is active (click to return to the Traditional theme).
    if (!comfortButton_)
        return;
    const bool comfort = settings_.documentTheme == QLatin1String("comfort");
    const QColor ink = mervin::Theme::iconInk(palette()); // same ink as the other toolbar glyphs
    comfortButton_->setIcon(mervin::icons::glyph(
        comfort ? mervin::icons::Glyph::Sun : mervin::icons::Glyph::UiTheme, ink));
    comfortButton_->setToolTip(comfort
        ? tr("Switch to the traditional document theme")
        : tr("Switch to the comfort (dark) document theme"));
}

void MainWindow::applyControlStyle()
{
    // The window chrome is styled centrally by mervin::Theme (built into the
    // app-wide stylesheet on qApp), so this per-window hook only refreshes what
    // QSS cannot express and what must track the active palette: the custom-
    // painted toolbar dividers (below) and the Lucide glyph icons, plus the
    // Recent pill / tab-bar selected state, which a dynamic property drives.
    const mervin::theme::Chrome t = mervin::theme::chrome(palette());

    // Colour the custom-painted toolbar dividers: the Compact Slate spec uses a
    // 10%-white hairline between toolbar groups; light sits just above the
    // central sheet's button-border alpha (subtle, but visible on the chrome).
    for (QWidget *sep : findChildren<QWidget *>(QStringLiteral("toolSep"))) {
        sep->setProperty("lineColor", t.hairline);
        sep->update();
    }

    applyTitleBarTheme(); // slate caption colour on Windows 11 (no-op elsewhere)
    updateRecentButton(); // re-apply pill style with the updated palette
    applyActionIcons();   // re-draw icons with the current text colour

    // Reapplying the stylesheet can remove WA_Hover; restore it on toolbar controls.
    QTimer::singleShot(0, this, [this] { reassertHoverAttributes(); });
}

void MainWindow::reassertHoverAttributes()
{
    // Re-establish hover for the chrome buttons after a theme switch. Fully
    // re-polish each button rather than just flipping WA_Hover: that re-runs the
    // stylesheet style's polish, which re-registers the widget's :hover rule AND
    // re-sets WA_Hover - restoring the hover repaint that the app-wide
    // setStyleSheet() re-application can otherwise leave stale.
    const auto refresh = [](QWidget *w) {
        if (!w)
            return;
        w->setAttribute(Qt::WA_Hover, true);
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    };
    if (mainToolBar_)
        for (QAbstractButton *b : mainToolBar_->findChildren<QAbstractButton *>())
            refresh(b);
    refresh(recentBtn_);
}

void MainWindow::applyTitleBarTheme()
{
#ifdef Q_OS_WIN
    // Colour the native caption to the slate title-bar tone. DWMWA_CAPTION_COLOR
    // needs Windows 11; older Windows rejects the attribute, which is harmless
    // (Qt's own dark-scheme handling still gives a dark title bar there). Light
    // mode resets to the DWM default so the OS draws its standard caption.
    if (!windowHandle())
        return; // no native window yet; showEvent re-applies once created
    constexpr DWORD kCaptionColor = 35;        // DWMWA_CAPTION_COLOR (Win11+)
    constexpr DWORD kCaptionTextColor = 36;    // DWMWA_TEXT_COLOR (Win11+)
    constexpr COLORREF kColorDefault = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT (an OS
                                                   // sentinel, not a colour)
    // COLORREF is 0x00BBGGRR, so the RGB() macro (not a hex literal) does the swap.
    const mervin::theme::Chrome t = mervin::theme::chrome(palette());
    const COLORREF caption =
        t.dark ? RGB(t.status.red(), t.status.green(), t.status.blue()) : kColorDefault;
    const COLORREF captionText =
        t.dark ? RGB(t.inkPrimary.red(), t.inkPrimary.green(), t.inkPrimary.blue())
               : kColorDefault;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, kCaptionTextColor, &captionText, sizeof(captionText));
#endif
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    applyTitleBarTheme(); // the native window handle exists by now
}

void MainWindow::applyActionIcons()
{
    // Use bundled Lucide glyphs across toolbar and menus, tinted from the current theme.
    using mervin::icons::Glyph;
    const QColor col = mervin::Theme::iconInk(palette());
    if (copyAction_)
        copyAction_->setIcon(mervin::icons::glyph(Glyph::Copy, col));
    if (ocrAction_)
        ocrAction_->setIcon(mervin::icons::glyph(Glyph::Ocr, col));
    openAction_->setIcon(mervin::icons::glyph(Glyph::Open, col));
    prevPageAction_->setIcon(mervin::icons::glyph(Glyph::PrevPage, col));
    nextPageAction_->setIcon(mervin::icons::glyph(Glyph::NextPage, col));
    zoomOutAction_->setIcon(mervin::icons::glyph(Glyph::ZoomOut, col));
    zoomInAction_->setIcon(mervin::icons::glyph(Glyph::ZoomIn, col));
    fitModeAction_->setIcon(mervin::icons::glyph(Glyph::FitMode, col));
    rotateLeftAction_->setIcon(mervin::icons::glyph(Glyph::RotateLeft, col));
    rotateRightAction_->setIcon(mervin::icons::glyph(Glyph::RotateRight, col));
    printAction_->setIcon(mervin::icons::glyph(Glyph::Print, col));
    if (measureAction_)
        measureAction_->setIcon(mervin::icons::glyph(Glyph::Measure, col));
    if (formAction_)
        formAction_->setIcon(mervin::icons::glyph(Glyph::FillForm, col));
    if (commentAction_)
        commentAction_->setIcon(mervin::icons::glyph(Glyph::Comments, col));
    if (saveButton_)
        saveButton_->setIcon(mervin::icons::glyph(Glyph::Save, col));
    if (moreButton_)
        moreButton_->setIcon(mervin::icons::glyph(Glyph::Menu, col));

    // ── Menu icons ──────────────────────────────────────────────────────────
    // Every menu takes the same neutral toolbar ink set above, the Document
    // popover included.
    auto setGlyph = [&col](QAction *a, Glyph g) {
        if (a)
            a->setIcon(mervin::icons::glyph(g, col));
    };
    setGlyph(fitPageAction_, Glyph::FitPage);
    setGlyph(fitWidthAction_, Glyph::FitWidth);
    setGlyph(fullScreenAction_, Glyph::FullScreen);
    setGlyph(continuousAction_, Glyph::ContinuousScroll);
    setGlyph(singleAction_, Glyph::SinglePage);
    setGlyph(twoPageAction_, Glyph::TwoPageSpread);
    setGlyph(selectAllAction_, Glyph::SelectAll);
    setGlyph(highlightFormFieldsAction_, Glyph::HighlightFields);
    setGlyph(alwaysOnTopAction_, Glyph::AlwaysOnTop);
    setGlyph(settingsAction_, Glyph::Settings);
    setGlyph(keyboardShortcutsAction_, Glyph::Keyboard);
    setGlyph(aboutAction_, Glyph::About);
    if (outlineDock_)
        setGlyph(outlineDock_->toggleViewAction(), Glyph::Outline);
    if (thumbnailDock_)
        setGlyph(thumbnailDock_->toggleViewAction(), Glyph::Thumbnails);
    if (commentsDock_)
        setGlyph(commentsDock_->toggleViewAction(), Glyph::Comments);
    // The toolbar's comfort toggle picks moon/sun by the active document theme.
    syncComfortButton();

    if (documentButton_)
        documentButton_->setIcon(mervin::icons::glyph(Glyph::Document, col));
    if (documentMenu_) {
        // Fixed order: Rotate, Delete, Extract, Split, Merge, (sep), Security -
        // the separator is skipped. Same neutral ink as every other menu.
        static const Glyph docGlyphs[] = {
            Glyph::RotateRight, Glyph::Delete, Glyph::ExtractPages,
            Glyph::SplitPages, Glyph::MergePages, Glyph::Security,
        };
        constexpr int docGlyphCount = int(sizeof(docGlyphs) / sizeof(docGlyphs[0]));
        int gi = 0;
        for (QAction *a : documentMenu_->actions()) {
            if (a->isSeparator())
                continue;
            if (gi < docGlyphCount)
                a->setIcon(mervin::icons::glyph(docGlyphs[gi++], col));
        }
    }
}

void MainWindow::syncDocTabBar()
{
    if (!docTabBar_)
        return;
    QSignalBlocker blocker(docTabBar_);
    // Remove extra tabs or add missing ones to match tabs_->count().
    while (docTabBar_->count() > tabs_->count())
        docTabBar_->removeTab(docTabBar_->count() - 1);
    while (docTabBar_->count() < tabs_->count())
        docTabBar_->addTab(QString());
    // Sync titles and tooltips; the per-tab glyphs are (re)tinted in
    // updateTabGlyphs() so the active document's tab can carry the accent colour.
    for (int i = 0; i < tabs_->count(); ++i) {
        docTabBar_->setTabText(i, tabs_->tabText(i));
        docTabBar_->setTabToolTip(i, tabs_->tabToolTip(i));
    }
    if (tabs_->currentIndex() >= 0)
        docTabBar_->setCurrentIndex(tabs_->currentIndex());
    updateTabGlyphs();
}

void MainWindow::updateTabGlyphs()
{
    // Each open tab shows a document glyph (Segoe "Page", U+E8A5). It is drawn in
    // the neutral icon ink, except the tab of the document currently on screen,
    // whose glyph takes the app accent - the same blue used for selection and
    // focus elsewhere - so the active tab reads as active at a glance. While the
    // Recent view is up no document is "current", so every glyph stays neutral.
    if (!docTabBar_)
        return;
    const QColor neutral = mervin::Theme::iconInk(palette());
    const QColor accent = mervin::Theme::accentColor(settings_.accentColor, palette());
    const int active = recentActive_ ? -1 : docTabBar_->currentIndex();
    for (int i = 0; i < docTabBar_->count(); ++i)
        docTabBar_->setTabIcon(i, mervin::icons::glyph(mervin::icons::Glyph::Document,
                                            i == active ? accent : neutral));
}

void MainWindow::setUiEnabled(bool enabled)
{
    // Disable document actions and their shortcuts while no PDF or the Recent view is active.
    // setCommandBarMode separately styles the visible controls.
    const bool docEnabled = enabled && !recentActive_;
    for (QAction *a : {prevPageAction_, nextPageAction_, zoomInAction_, zoomOutAction_,
                       fitModeAction_, rotateLeftAction_, rotateRightAction_, printAction_})
        a->setEnabled(docEnabled);
    pageEdit_->setEnabled(docEnabled);
    zoomCombo_->setEnabled(docEnabled);
    // Save acts on the current document; dead with no document or on the Recent view.
    if (saveButton_)
        saveButton_->setEnabled(docEnabled);
    // Document tools (page operations + security) likewise need an open document.
    if (documentButton_)
        documentButton_->setEnabled(docEnabled);

    // Selection / find / OCR / close stay tied only to having a document open.
    // Find also focuses the Recent search field, so it remains live on Recent.
    for (QAction *a : {closeTabAction_, findAction_, findNextAction_, findPrevAction_,
                       copyAction_, selectAllAction_, ocrAction_, measureAction_})
        a->setEnabled(enabled);
    // Fill Forms is additionally gated on the document actually having fields.
    ViewerWidget *v = currentViewer();
    if (formAction_)
        formAction_->setEnabled(enabled && v && v->hasFormFields());
    // Highlight / Comment are available for any PDF document (any PDF can carry
    // annotations); disabled for non-PDF formats and when no document is open.
    const bool annot = enabled && v && v->hasAnnotationSupport();
    if (commentAction_)
        commentAction_->setEnabled(annot);
}

bool MainWindow::openFile(const QString &path, bool allowDuplicate, int atIndex, bool makeCurrent,
                          const QString &knownPassword)
{
    QFileInfo fi(path);
    QString canon = fi.canonicalFilePath();
    if (canon.isEmpty())
        canon = fi.absoluteFilePath();

    // Already open anywhere in this process? Focus it instead of duplicating -
    // unless the caller explicitly wants a second view ("Duplicate to new
    // window"), in which case fall through and open a fresh, independent tab.
    if (!allowDuplicate) {
        // A background open (the staged session restore) must not pull the view
        // onto an already-open document or raise its window: by the time the batch
        // reaches that document the user may be reading something else, and not
        // taking the view is the whole point of makeCurrent=false. Answer "already
        // open" and leave the UI alone.
        if (!makeCurrent) {
            if (indexOfPath(canon) >= 0 || (wm_ && wm_->isOpenAnywhere(canon)))
                return true;
        } else if (wm_) {
            if (wm_->focusExistingTab(canon))
                return true;
        } else if (focusTabIfOpen(canon)) {
            return true;
        }
    }

    auto *page = new TabPage(engine_);
    QString error;
    QString password = knownPassword;
    bool needsPassword = false;
    bool ok = page->open(path, password, &error, &needsPassword);
    while (!ok && needsPassword) {
        bool got = false;
        password = QInputDialog::getText(
            this, tr("Password Required"),
            tr("\"%1\" is password-protected. Enter the password:").arg(fi.fileName()),
            QLineEdit::Password, QString(), &got);
        if (!got) { // user cancelled
            delete page;
            return false;
        }
        needsPassword = false;
        ok = page->open(path, password, &error, &needsPassword);
    }
    if (!ok) {
        delete page;
        QMessageBox::warning(this, tr("Mervin PDF"),
                             tr("Could not open \"%1\".\n\n%2")
                                 .arg(QDir::toNativeSeparators(path), error));
        return false;
    }

    ViewerWidget *v = page->viewer();
    // The viewer's signals are wired to this window only while it is the current
    // tab (see wireCurrentViewer), so a tab moved to another window updates the
    // right toolbar.

    // A window holding no tabs is showing the Recent page, so its first document
    // must take the view whatever the caller asked for - QTabWidget would make it
    // current anyway, and the Recent page would then hide a loaded document.
    const bool takeView = makeCurrent || tabs_->count() == 0;
    if (takeView)
        recentActive_ = false; // opening a file exits the Recent view
    // Insert exactly like adoptTab does when a position is given; QTabWidget keeps
    // the same widget current across an insert before it, so a background insert
    // never moves the view off the document on screen.
    const int idx = (atIndex >= 0 && atIndex <= tabs_->count())
                        ? tabs_->insertTab(atIndex, page, page->tabTitle())
                        : tabs_->addTab(page, page->tabTitle());
    tabs_->setTabToolTip(idx, QDir::toNativeSeparators(page->path()));
    if (takeView)
        tabs_->setCurrentIndex(idx);
    syncDocTabBar();
    applySettingsToViewer(v); // default page mode / zoom / invert
    // Auto-entering Fill Forms for a document with fields is handled in
    // ViewerWidget::setDocument (so it fires on every open path - fresh, session
    // restore, save-reopen - not just this one), seeded from settings_.autoFormFill
    // via the TabPage constructor.

    // Record in the recent history; recordOpen() also restores this file's saved
    // view state (if any) synchronously from the in-process store via
    // applyViewState(). When duplicating, skip that restore: the stored state
    // would be applied to whichever window holds the file first (possibly the
    // original), tugging its view - the caller seeds the duplicate's view itself.
    if (wm_) {
        wm_->recordOpen(page->canonicalPath(), /*restoreViewState=*/!allowDuplicate,
                        page->viewer()->pageCount());
        wm_->updateSession();
    }
    return true;
}

QStringList MainWindow::askForPdfs()
{
    mervin::OpenPdfDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    if (!dialog.internetUrl().isEmpty())
        return {dialog.internetUrl()};
    return dialog.selectedFiles();
}

void MainWindow::onOpen()
{
    const QStringList paths = askForPdfs();
    if (paths.isEmpty())
        return;
    for (const QString &path : paths) {
        if (const auto url = mervin::urlopen::fromUserInput(path))
            openUrl(*url);
        else
            openFile(path); // openFile() adds it as a tab in this window
    }
}

void MainWindow::openUrl(const QUrl &url, bool inNewWindow)
{
    const QString fileName = mervin::urlopen::suggestedFileName(url);
    const QString cachedName = mervin::urlopen::cachedFileName(url);
    const QString downloadDir = mervin::urlopen::downloadDirectory();
    if (!QDir().mkpath(downloadDir)) {
        QMessageBox::warning(this, tr("Open from URL"),
                             tr("Could not create the download folder."));
        return;
    }

    const QString destination = QDir(downloadDir).filePath(cachedName);
    auto output = std::make_shared<QSaveFile>(destination);
    if (!output->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Open from URL"), output->errorString());
        return;
    }

    auto *progress = new QProgressDialog(tr("Downloading %1...").arg(fileName), tr("Cancel"),
                                         0, 0, this);
    progress->setWindowTitle(tr("Open from URL"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumWidth(420);
    progress->setMinimumDuration(0);
    progress->show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    auto *network = new QNetworkAccessManager(progress);
    const QNetworkRequest request = mervin::urlopen::makeRequest(url);
    QNetworkReply *reply = network->get(request);
    auto writeFailed = std::make_shared<bool>(false);
    connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
                if (total > 0 && total <= std::numeric_limits<int>::max()) {
                    progress->setRange(0, static_cast<int>(total));
                    progress->setValue(static_cast<int>(received));
                }
            });
    connect(reply, &QIODevice::readyRead, this, [reply, output, writeFailed] {
        const QByteArray chunk = reply->readAll();
        if (output->write(chunk) != chunk.size()) {
            *writeFailed = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, progress, output, writeFailed, destination, inNewWindow] {
                progress->hide(); // completion must not emit canceled() and abort the reply
                const auto networkError = reply->error();
                const QString error = reply->errorString();
                const bool saved = networkError == QNetworkReply::NoError && output->commit();
                reply->deleteLater();
                progress->deleteLater();
                if (!saved) {
                    output->cancelWriting();
                    if (*writeFailed || networkError != QNetworkReply::OperationCanceledError) {
                        QMessageBox::warning(this, tr("Open from URL"),
                                             (*writeFailed || networkError == QNetworkReply::NoError)
                                                 ? output->errorString()
                                                 : error);
                    }
                    return;
                }
                if (inNewWindow && wm_)
                    wm_->openPaths({destination}, QStringLiteral("new-window"));
                else
                    openFile(destination);
            });
}

void MainWindow::onOpenInNewWindow()
{
    const QStringList paths = askForPdfs();
    if (paths.isEmpty())
        return;

    QStringList localPaths;
    for (const QString &path : paths) {
        if (const auto url = mervin::urlopen::fromUserInput(path))
            openUrl(*url, /*inNewWindow=*/true);
        else
            localPaths.append(path);
    }
    if (localPaths.isEmpty())
        return;
    if (wm_)
        wm_->openPaths(localPaths, QStringLiteral("new-window"));
    else {
        for (const QString &path : localPaths)
            openFile(path); // no window manager (standalone): fall back to tabs
    }
}

void MainWindow::showAbout()
{
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::showShortcuts()
{
    // Mirrors the spec's baseline shortcut table. Menu rows carry no shortcut
    // hints (see hideShortcutHints), so this dialog is the only place the
    // bindings are written down - every one of them belongs here.
    static const char *rows[][2] = {
        {"Open", "Ctrl+O"},          {"Save edits", "Ctrl+S"},
        {"New window", "Ctrl+N"},    {"New tab", "Ctrl+T"},
        {"Close tab", "Ctrl+W"},
        {"Reopen closed tab", "Ctrl+Shift+T"},
        {"Cycle tabs", "Ctrl+Tab / Ctrl+Shift+Tab"},
        {"Find", "Ctrl+F"},
        {"Find next / previous", "F3 / Shift+F3"},
        {"Previous / next page", "Ctrl+Up / Ctrl+Down"},
        {"Zoom in / out", "Ctrl+= / Ctrl+-"},
        {"Fit page / width", "Ctrl+1 / Ctrl+2"},
        {"Toggle fit page / width", "Home"},
        {"Rotate left / right", "Ctrl+Shift+L / Ctrl+Shift+R"},
        {"Copy / Select all", "Ctrl+C / Ctrl+A"},
        {"Comment", "Ctrl+Shift+N"},
        {"OCR selection", "Ctrl+Shift+O"},
        {"Measure", "Ctrl+Shift+M"},
        {"Fill forms", "Ctrl+Shift+F"},
        {"Print", "Ctrl+P"},         {"Full screen", "F11"},
    };
    QString html = QStringLiteral("<table cellspacing='6'>");
    for (const auto &r : rows)
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                    .arg(QString::fromLatin1(r[0]), QString::fromLatin1(r[1]));
    html += QStringLiteral("</table>");

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Keyboard Shortcuts"));
    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(html, &dlg);
    label->setTextFormat(Qt::RichText);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::onCurrentTabChanged(int)
{
    // Clicking a tab exits the Recent view and returns to the document.
    if (tabCount() > 0)
        recentActive_ = false;
    updateForCurrentTab();
}

void MainWindow::closeTab(int index)
{
    closeTabAt(index, /*remember=*/true);
}

void MainWindow::closeTabAt(int index, bool remember)
{
    QWidget *w = tabs_->widget(index);
    if (!w)
        return;
    if (auto *t = qobject_cast<TabPage *>(w)) {
        if (!confirmClose(t))
            return;
        saveTabViewState(t); // remember where the user left off
        if (remember)
            rememberClosedTab(t, index);
    }
    tabs_->removeTab(index);
    syncDocTabBar();
    delete w; // drops the Document (engine base context still alive)
    if (wm_)
        wm_->updateSession();
}

void MainWindow::rememberClosedTab(TabPage *tab, int index)
{
    if (wm_ && tab)
        wm_->rememberClosedTab(tab->path(), tabPaths(), index);
}

void MainWindow::rememberOpenTabs()
{
    if (!wm_ || !tabs_)
        return;
    // Record original tab positions before removing any tabs. Record the active tab last so it reopens first.
    const int cur = tabs_->currentIndex();
    for (int i = 0; i < tabs_->count(); ++i)
        if (i != cur)
            rememberClosedTab(qobject_cast<TabPage *>(tabs_->widget(i)), i);
    if (cur >= 0)
        rememberClosedTab(qobject_cast<TabPage *>(tabs_->widget(cur)), cur);
}

void MainWindow::closeAllTabs()
{
    // Record the whole bar first (see rememberOpenTabs), then close without
    // re-recording. Closing the first tab repeatedly is otherwise the same path
    // as the per-tab close button, so each tab's view state is saved and the
    // empty window falls back to Recent.
    rememberOpenTabs();
    while (tabs_->count() > 0) {
        const int before = tabs_->count();
        closeTabAt(0, /*remember=*/false);
        if (tabs_->count() == before)
            break;
    }
}

void MainWindow::reopenClosedTab()
{
    if (!wm_)
        return;
    // Show a status message only for an empty history; open failures already report their own error.
    if (wm_->reopenClosedTab(this) == mervin::WindowManager::Reopen::NothingToReopen)
        statusBar()->showMessage(tr("No recently closed tab to reopen"), 3000);
}

void MainWindow::showRecentPanel()
{
    recentActive_ = true;
    if (stack_)
        stack_->setCurrentIndex(0);
    if (wm_) {
        recentPanel_->setEntries(wm_->recentEntries());
        wm_->refreshRecent();
    }
    statusInfo_->setText(recentPanel_->statusSummary());
    updateRecentButton();
    setCommandBarMode(true);
    if (findBar_) {
        findBar_->setMode(mervin::FindBar::Mode::RecentSearch);
        findBar_->activate(); // focus the search field
    }
}

void MainWindow::updateRecentButton()
{
    // Apply the Recent state to both tab controls and refresh their stylesheet-dependent appearance.
    const auto setRecentActive = [this](QWidget *w) {
        if (!w || w->property("recentActive").toBool() == recentActive_)
            return;
        w->setProperty("recentActive", recentActive_);
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    };
    setRecentActive(docTabBar_);
    setRecentActive(recentBtn_);
    // The active-tab accent glyph depends on the same recent/document state, so
    // refresh it here too (covers switching to Recent, where no tab is active).
    updateTabGlyphs();
}

void MainWindow::setCommandBarMode(bool recentActive)
{
    // Keep document controls visible but disabled on Recent; action shortcuts are disabled separately.
    if (docControls_)
        docControls_->setEnabled(!recentActive);
}

void MainWindow::updateStartPage()
{
    if (!stack_)
        return;
    const bool empty = tabCount() == 0;
    if (empty) {
        // No tabs - must show Recent.
        recentActive_ = true;
    }
    // If tabs exist and the user didn't explicitly click Recent, show the tab content.
    stack_->setCurrentIndex(recentActive_ ? 0 : 1);

    if (recentActive_) {
        if (wm_) {
            recentPanel_->setEntries(wm_->recentEntries());
            wm_->refreshRecent();
        }
        if (findBar_)
            findBar_->setMode(mervin::FindBar::Mode::RecentSearch);
    } else {
        if (contentSearch_)
            contentSearch_->cancel();
        if (findBar_)
            findBar_->setMode(mervin::FindBar::Mode::FindDocument);
    }
    updateRecentButton();
    setCommandBarMode(recentActive_);
}

void MainWindow::updateForCurrentTab()
{
    // Record the active document after each tab change, once the window has tabs.
    if (wm_ && tabCount() > 0)
        wm_->updateSession();

    updateStartPage();

    ViewerWidget *v = currentViewer();
    wireCurrentViewer(v);
    updateSidebars();
    refreshCommentsSidebar(); // reflect the current tab's annotations (if shown)
    setUiEnabled(v != nullptr);
    syncViewActions(v);

    if (!v) {
        setWindowTitle(tr("Mervin PDF"));
        // No document: show the recent listing summary while the Recent view is
        // active, otherwise leave the status bar empty.
        if (recentActive_ && recentPanel_)
            statusInfo_->setText(recentPanel_->statusSummary());
        else
            statusInfo_->clear();
        if (pageEdit_)
            pageEdit_->clear();
        if (pageCountLabel_)
            pageCountLabel_->setText(QStringLiteral(" / 0 "));
        return;
    }

    updatePageLabels(v->currentPage() + 1, v->pageCount());
    syncZoomCombo(v);

    // Search is per-tab: restore this tab's query, options and result count into
    // the shared find bar (without re-running the search). Skipped in Recent
    // mode, where the bar filters the recent list instead.
    if (findBar_ && !recentActive_)
        findBar_->restoreFindState(v->findQuery(), v->findCaseSensitive(), v->findWholeWord(),
                                   v->currentMatchNumber(), v->matchCount());

    TabPage *t = currentTab();
    setWindowTitle(tr("%1 - Mervin PDF").arg(t->tabTitle()));
    statusInfo_->setText(QDir::toNativeSeparators(t->path()));
    v->setFocus();
}

void MainWindow::updatePageLabels(int current, int total)
{
    if (pageEdit_) {
        QSignalBlocker blocker(pageEdit_);
        pageEdit_->setText(current > 0 ? QString::number(current) : QString());
    }
    if (pageCountLabel_)
        pageCountLabel_->setText(QStringLiteral(" / %1 ").arg(total));
}

void MainWindow::syncZoomCombo(ViewerWidget *viewer)
{
    if (!zoomCombo_ || !viewer)
        return;
    QSignalBlocker blocker(zoomCombo_);
    switch (viewer->zoomMode()) {
    case ViewerWidget::ZoomMode::FitPage:
        zoomCombo_->setCurrentText(tr("Fit Page"));
        break;
    case ViewerWidget::ZoomMode::FitWidth:
        zoomCombo_->setCurrentText(tr("Fit Width"));
        break;
    case ViewerWidget::ZoomMode::Custom:
        zoomCombo_->setCurrentText(QStringLiteral("%1%").arg(qRound(viewer->scale() * 100)));
        break;
    }
}

void MainWindow::onPageEditReturn()
{
    bool ok = false;
    const int page = pageEdit_->text().toInt(&ok);
    if (ok)
        if (auto *v = currentViewer())
            v->goToPage(page - 1);
}

void MainWindow::onZoomComboActivated()
{
    ViewerWidget *v = currentViewer();
    if (!v)
        return;
    const QString text = zoomCombo_->currentText().trimmed();
    if (text.compare(tr("Fit Page"), Qt::CaseInsensitive) == 0) {
        v->setZoomMode(ViewerWidget::ZoomMode::FitPage);
    } else if (text.compare(tr("Fit Width"), Qt::CaseInsensitive) == 0) {
        v->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    } else {
        QString num = text;
        num.remove(QLatin1Char('%'));
        bool ok = false;
        const double pct = num.toDouble(&ok);
        if (ok && pct > 0)
            v->setScale(pct / 100.0);
    }
    // Hand focus back to the document so the zoom box does not keep a caret after
    // the value is committed (mirrors the find bar returning focus on Enter/Esc).
    v->setFocus();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Zoom box: only allow editing (and thus a caret) while it actually holds
    // focus. See createDocControls for why this suppresses the stray blink.
    if (zoomCombo_ && watched == zoomCombo_->lineEdit()) {
        if (event->type() == QEvent::FocusIn) {
            zoomCombo_->lineEdit()->setReadOnly(false);
        } else if (event->type() == QEvent::FocusOut) {
            QLineEdit *le = zoomCombo_->lineEdit();
            le->deselect();
            le->setReadOnly(true);
        }
    }

    if (watched == docTabBar_ && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            const int idx = docTabBar_->tabAt(me->position().toPoint());
            if (idx >= 0) {
                closeTab(idx);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::toggleFullScreen(bool on)
{
    // The menu bar is hidden permanently (its entries live in the ⋯ toolbar
    // button), so only the window state changes here.
    if (on)
        showFullScreen();
    else
        showNormal();
}

void MainWindow::toggleAlwaysOnTop(bool on)
{
    setWindowFlag(Qt::WindowStaysOnTopHint, on);
    show(); // re-show is required after changing window flags
}

void MainWindow::openSettings()
{
    settings_ = mervin::Settings::load();
    SettingsDialog dlg(settings_, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString prevAccent = settings_.accentColor;
    const QString prevDocTheme = settings_.documentTheme;
    const QString prevScheme = settings_.colorScheme;
    const bool prevAutoUpdate = settings_.autoUpdate;
    settings_ = dlg.settings();
    settings_.save();
    // Keep the More-menu toggle in sync and push the highlight setting to every
    // open tab (it is window/app-wide, like the menu toggle does).
    if (highlightFormFieldsAction_) {
        QSignalBlocker block(highlightFormFieldsAction_);
        highlightFormFieldsAction_->setChecked(settings_.highlightFormFields);
    }
    const QColor annotColor(settings_.annotationColor);
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *tp = qobject_cast<TabPage *>(tabs_->widget(i))) {
            tp->viewer()->setHighlightFormFields(settings_.highlightFormFields);
            if (annotColor.isValid())
                tp->viewer()->setMarkupColor(annotColor); // new default for new marks/notes
        }
    // Apply global theme changes through WindowManager; its scheduled refresh reads the saved accent.
    if (settings_.colorScheme != prevScheme && wm_)
        wm_->setColorScheme(settings_.colorScheme);
    else if (settings_.accentColor != prevAccent)
        mervin::Theme::applyApp(); // rebuild the app-wide sheet for every window + dialog
    // Document theme is global: route a change through the WindowManager so every
    // window re-applies it (the broadcast also re-tints this window's tabs).
    if (settings_.documentTheme != prevDocTheme && wm_)
        wm_->setDocumentTheme(settings_.documentTheme);
    // Auto update too: the other windows' settings copies and the Updater (which
    // drops a downloaded update when switched off) follow the broadcast.
    if (settings_.autoUpdate != prevAutoUpdate && wm_)
        wm_->setAutoUpdate(settings_.autoUpdate);
    if (auto *v = currentViewer())
        applySettingsToViewer(v); // apply the new defaults to the current view
}

void MainWindow::applyZoom(ViewerWidget *viewer, const QString &zoom)
{
    if (zoom == QLatin1String("fit-width")) {
        viewer->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    } else if (zoom == QLatin1String("fit-page")) {
        viewer->setZoomMode(ViewerWidget::ZoomMode::FitPage);
    } else {
        bool ok = false;
        const double pct = zoom.toDouble(&ok);
        if (ok && pct > 0)
            viewer->setScale(pct / 100.0);
        else
            viewer->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    }
}

void MainWindow::applySettingsToViewer(ViewerWidget *viewer)
{
    if (!viewer)
        return;
    ViewLayout::Mode lm;
    lm.scroll = (settings_.pageMode == QLatin1String("single")) ? ViewLayout::Scroll::Single
                                                                : ViewLayout::Scroll::Continuous;
    lm.spread = settings_.twoPageSpread;
    viewer->setLayoutMode(lm);
    viewer->setPageTheme(computeDocumentPageTheme());
    viewer->setMeasureSnap(settings_.measurementSnap);
    viewer->setHighlightFormFields(settings_.highlightFormFields);
    viewer->setAutoFormFill(settings_.autoFormFill);
    applyZoom(viewer, settings_.defaultZoom);
}

mervin::ViewState MainWindow::captureViewState(ViewerWidget *viewer) const
{
    mervin::ViewState st;
    if (!viewer)
        return st;
    const ViewerWidget::ScrollAnchor anchor = viewer->scrollAnchor();
    st.page = anchor.page; // page under the viewport top-left - the resume anchor
    st.rotation = viewer->rotation();
    st.scale = viewer->scale();
    st.offsetX = anchor.fracX; // exact spot within that page
    st.offsetY = anchor.fracY;
    switch (viewer->zoomMode()) {
    case ViewerWidget::ZoomMode::FitPage:
        st.zoomMode = QStringLiteral("fit-page");
        break;
    case ViewerWidget::ZoomMode::Custom:
        st.zoomMode = QStringLiteral("custom");
        break;
    case ViewerWidget::ZoomMode::FitWidth:
        st.zoomMode = QStringLiteral("fit-width");
        break;
    }
    return st;
}

void MainWindow::applyViewStateToViewer(ViewerWidget *viewer, const mervin::ViewState &state)
{
    if (!viewer)
        return;
    viewer->setRotation(state.rotation);
    if (state.zoomMode == QLatin1String("fit-page"))
        viewer->setZoomMode(ViewerWidget::ZoomMode::FitPage);
    else if (state.zoomMode == QLatin1String("custom"))
        viewer->setScale(state.scale);
    else
        viewer->setZoomMode(ViewerWidget::ZoomMode::FitWidth);
    // Last, so it anchors within the final layout. Restores the exact scroll spot
    // (page + within-page fraction) and stays sticky across the open-time re-fit.
    viewer->restorePageScrollFraction(state.page, state.offsetX, state.offsetY);
}

bool MainWindow::applyViewState(const QString &canonicalPath, const mervin::ViewState &state)
{
    const int idx = indexOfPath(canonicalPath);
    if (idx < 0)
        return false;
    auto *t = qobject_cast<TabPage *>(tabs_->widget(idx));
    if (!t || !t->viewer())
        return false;
    applyViewStateToViewer(t->viewer(), state);
    return true;
}

void MainWindow::saveTabViewState(TabPage *tab)
{
    if (!wm_ || !tab || !tab->viewer())
        return;
    wm_->saveViewState(tab->canonicalPath(), captureViewState(tab->viewer()));
}

void MainWindow::saveAllViewStates()
{
    if (!wm_ || !tabs_)
        return;
    QList<QPair<QString, mervin::ViewState>> states;
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto *t = qobject_cast<TabPage *>(tabs_->widget(i)))
            states.append({t->canonicalPath(), captureViewState(t->viewer())});
    wm_->saveViewStates(states);
}

void MainWindow::wireCurrentViewer(ViewerWidget *v)
{
    // Connect only the current viewer, so a tab moved to another window stops
    // updating this window's toolbar and starts updating its new home's.
    for (const QMetaObject::Connection &c : viewerConns_)
        QObject::disconnect(c);
    viewerConns_.clear();
    if (!v)
        return;
    viewerConns_ << connect(v, &ViewerWidget::pageChanged, this,
                            [this](int current, int total) { updatePageLabels(current, total); });
    viewerConns_ << connect(v, &ViewerWidget::scaleChanged, this,
                            [this, v](double) { syncZoomCombo(v); });
    viewerConns_ << connect(v, &ViewerWidget::zoomModeChanged, this,
                            [this, v](ViewerWidget::ZoomMode) { syncZoomCombo(v); });
    // Track live layout changes as well as tab switches: newly applied defaults arrive after
    // updateForCurrentTab.
    viewerConns_ << connect(v, &ViewerWidget::layoutModeChanged, this,
                            [this, v](ViewLayout::Mode) { syncViewActions(v); });
    viewerConns_ << connect(v, &ViewerWidget::ocrRegionSelected, this,
                            &MainWindow::onOcrRegionSelected);
    // Measuring tool: handle calibration, keep the action's check + status bar in
    // sync with the viewer (e.g. when Esc exits measure mode).
    viewerConns_ << connect(v, &ViewerWidget::calibrationLineDrawn, this,
                            &MainWindow::onCalibrationLineDrawn);
    viewerConns_ << connect(v, &ViewerWidget::setScaleRequested, this,
                            &MainWindow::onSetScaleRequested);
    viewerConns_ << connect(v, &ViewerWidget::measureModeChanged, this, [this](bool on) {
        if (measureAction_ && measureAction_->isChecked() != on) {
            QSignalBlocker block(measureAction_);
            measureAction_->setChecked(on);
        }
        if (!on && statusInfo_)
            updateForCurrentTab(); // restore the path text in the status bar
    });
    // Switching to the standard pointer (Esc or the panel's cursor toggle) leaves
    // a stale measurement readout in the status bar; restore the document path.
    // (Disabling the tool entirely is handled by measureModeChanged above.)
    viewerConns_ << connect(v, &ViewerWidget::measureCursorActiveChanged, this, [this](bool active) {
        if (!active && statusInfo_) {
            if (TabPage *t = currentTab())
                statusInfo_->setText(QDir::toNativeSeparators(t->path()));
        }
    });
    viewerConns_ << connect(v, &ViewerWidget::measurementReadout, this, [this](const QString &t) {
        if (statusInfo_ && !t.isEmpty())
            statusInfo_->setText(t);
    });
    // Form filling: keep the Fill-Forms toggle in sync (e.g. Esc leaves the tool),
    // and refresh the Save button's enabled state when a field is filled.
    viewerConns_ << connect(v, &ViewerWidget::formModeChanged, this, [this](bool on) {
        if (formAction_ && formAction_->isChecked() != on) {
            QSignalBlocker block(formAction_);
            formAction_->setChecked(on);
        }
    });
    viewerConns_ << connect(v, &ViewerWidget::formEditsChanged, this,
                            [this] { updateForCurrentTab(); });
    // Annotations: keep the Highlight / Comment toggles in sync (e.g. Esc leaves
    // the tool), refresh the Save state when a mark is added/edited, and refresh
    // the comments list when the annotation set changes.
    viewerConns_ << connect(v, &ViewerWidget::commentToolEnabledChanged, this, [this](bool on) {
        if (commentAction_ && commentAction_->isChecked() != on) {
            QSignalBlocker block(commentAction_);
            commentAction_->setChecked(on);
        }
    });
    viewerConns_ << connect(v, &ViewerWidget::annotEditsChanged, this,
                            [this] { updateForCurrentTab(); });
    viewerConns_ << connect(v, &ViewerWidget::annotationsChanged, this,
                            [this] { refreshCommentsSidebar(); });
    // Persist the last-used markup style when the user picks it in the Comment
    // panel (mirrors highlightFormFieldsAction_ - settings_ is the authoritative
    // in-memory copy, so a later closeEvent save won't clobber it). The default
    // colour is no longer a panel control; it lives in Settings.
    if (TabPage *tab = currentTab()) {
        if (mervin::AnnotPanel *ap = tab->annotPanel()) {
            viewerConns_ << connect(ap, &mervin::AnnotPanel::highlightStyleChanged, this,
                                    [this](mervin::AnnotType t) {
                                        settings_.annotationStyle =
                                            t == mervin::AnnotType::Underline
                                                ? QStringLiteral("underline")
                                            : t == mervin::AnnotType::StrikeOut
                                                ? QStringLiteral("strikeout")
                                                : QStringLiteral("highlight");
                                        settings_.save();
                                    });
        }
    }
    viewerConns_ << connect(v, &ViewerWidget::contextMenuRequested, this,
                            [this](const QPoint &globalPos) {
                                QMenu menu(this);
                                // Copy appears only when text is selected.
                                ViewerWidget *vw = currentViewer();
                                if (vw && vw->hasSelection()) {
                                    menu.addAction(copyAction_);
                                    menu.addSeparator();
                                }
                                if (vw && vw->hasAnnotationSupport())
                                    menu.addAction(commentAction_);
                                menu.addAction(ocrAction_);
                                menu.addAction(measureAction_);
                                if (vw && vw->hasFormFields())
                                    menu.addAction(formAction_);
                                menu.addSeparator();
                                menu.addAction(rotateLeftAction_);
                                menu.addAction(rotateRightAction_);
                                // Four of these rows are the ☰ menu's own actions
                                // and are already hint-less; without this the two
                                // Rotate rows would be the only ones still
                                // carrying a "Ctrl+Shift+…" column.
                                hideShortcutHints(&menu);
                                menu.exec(globalPos);
                            });
    // Reflect this viewer's current measure state in the toolbar toggle.
    if (measureAction_) {
        QSignalBlocker block(measureAction_);
        measureAction_->setChecked(v->measureToolEnabled());
    }
    // Reflect this viewer's form-fill state (and seed its highlight setting).
    v->setHighlightFormFields(settings_.highlightFormFields);
    if (formAction_) {
        QSignalBlocker block(formAction_);
        formAction_->setChecked(v->formMode());
    }
    if (commentAction_) {
        QSignalBlocker block(commentAction_);
        commentAction_->setChecked(v->commentToolEnabled());
    }
    viewerConns_ << connect(v, &ViewerWidget::pageChanged, this, [this](int current, int) {
        if (thumbnailSidebar_)
            thumbnailSidebar_->setCurrentPage(current - 1); // current is 1-based
    });
    // Wire the global find bar to this viewer.
    if (findBar_) {
        viewerConns_ << connect(findBar_, &mervin::FindBar::searchChanged,
                                v, &ViewerWidget::startFind);
        viewerConns_ << connect(findBar_, &mervin::FindBar::findNext,
                                v, &ViewerWidget::findNext);
        viewerConns_ << connect(findBar_, &mervin::FindBar::findPrev,
                                v, &ViewerWidget::findPrev);
        viewerConns_ << connect(v, &ViewerWidget::findStatusChanged,
                                findBar_, &mervin::FindBar::setResultCount);
    }
}

void MainWindow::createSidebars()
{
    // Qt puts tabified dock switchers along the bottom by default. Keep the
    // navigation panels aligned with other app tabs by placing their tab strip
    // above the active sidebar instead.
    setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);

    // Sidebars stay in the left panel. Omitting DockWidgetMovable and
    // DockWidgetFloatable prevents moving them over the document or detaching
    // them, while the close button remains available.
    const QDockWidget::DockWidgetFeatures sidebarFeatures =
        QDockWidget::DockWidgetClosable;

    outlineSidebar_ = new mervin::OutlineSidebar(this);
    outlineDock_ = new QDockWidget(tr("Outline"), this);
    outlineDock_->setObjectName(QStringLiteral("outlineDock")); // for saveState/restoreState
    outlineDock_->setAllowedAreas(Qt::LeftDockWidgetArea);
    outlineDock_->setFeatures(sidebarFeatures);
    outlineDock_->setWidget(outlineSidebar_);
    addDockWidget(Qt::LeftDockWidgetArea, outlineDock_);
    outlineDock_->hide(); // collapsible; off by default
    connect(outlineSidebar_, &mervin::OutlineSidebar::pageSelected, this, [this](int p) {
        if (auto *v = currentViewer())
            v->goToPage(p);
    });

    thumbnailSidebar_ = new mervin::ThumbnailSidebar(engine_, this);
    thumbnailDock_ = new QDockWidget(tr("Thumbnails"), this);
    thumbnailDock_->setObjectName(QStringLiteral("thumbnailDock"));
    thumbnailDock_->setAllowedAreas(Qt::LeftDockWidgetArea);
    thumbnailDock_->setFeatures(sidebarFeatures);
    thumbnailDock_->setWidget(thumbnailSidebar_);
    addDockWidget(Qt::LeftDockWidgetArea, thumbnailDock_);
    thumbnailDock_->hide();
    tabifyDockWidget(thumbnailDock_, outlineDock_); // stack the two as tabs
    connect(thumbnailSidebar_, &mervin::ThumbnailSidebar::pageSelected, this, [this](int p) {
        if (auto *v = currentViewer())
            v->goToPage(p);
    });

    commentsSidebar_ = new mervin::CommentsSidebar(this);
    commentsDock_ = new QDockWidget(tr("Comments"), this);
    commentsDock_->setObjectName(QStringLiteral("commentsDock")); // for saveState/restoreState
    commentsDock_->setAllowedAreas(Qt::LeftDockWidgetArea);
    commentsDock_->setFeatures(sidebarFeatures);
    commentsDock_->setWidget(commentsSidebar_);
    addDockWidget(Qt::LeftDockWidgetArea, commentsDock_);
    commentsDock_->hide(); // collapsible; off by default
    tabifyDockWidget(outlineDock_, commentsDock_); // stack with outline/thumbnails
    connect(commentsSidebar_, &mervin::CommentsSidebar::annotationActivated, this,
            [this](int page, int id) {
                if (auto *v = currentViewer())
                    v->revealAnnotation(page, id);
            });
    // Populate the list the moment the dock is shown (it's kept empty while hidden
    // to avoid enumerating every page's annotations on open).
    connect(commentsDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible)
            refreshCommentsSidebar();
    });
}

void MainWindow::refreshCommentsSidebar()
{
    if (!commentsSidebar_ || !commentsDock_ || !commentsDock_->isVisible())
        return; // only enumerate when the panel is actually shown
    ViewerWidget *v = currentViewer();
    commentsSidebar_->setAnnotations(v ? v->allAnnotations() : std::vector<mervin::Annotation>{});
}

void MainWindow::updateSidebars()
{
    ViewerWidget *v = currentViewer();
    mervin::Document *doc = v ? v->document() : nullptr;
    if (doc != sidebarDoc_) { // only rebuild when the document actually changes
        sidebarDoc_ = doc;
        if (outlineSidebar_)
            outlineSidebar_->setOutline(doc ? doc->outline()
                                            : std::vector<mervin::OutlineItem>{});
        if (thumbnailSidebar_)
            thumbnailSidebar_->setDocument(doc);
    }
    if (v && thumbnailSidebar_)
        thumbnailSidebar_->setCurrentPage(v->currentPage());
}

void MainWindow::onOcrRegionSelected(int pageNo, const QRectF &pageRect)
{
    ViewerWidget *v = currentViewer();
    if (!v || !v->document())
        return;

    QStringList installed = mervin::TessdataManager::installedLanguages();
    if (installed.isEmpty()) {
        const auto choice = QMessageBox::question(
            this, tr("OCR Selection"),
            tr("No OCR language data is installed.\n\nOpen the language manager to add a "
               "language (e.g. English)?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice != QMessageBox::Yes)
            return;

        mervin::ManageLanguagesDialog manager(settings_.ocrDefaultLanguage, this);
        manager.exec();
        settings_.ocrDefaultLanguage = manager.defaultLanguage();
        settings_.save();
        installed = mervin::TessdataManager::installedLanguages();
        if (installed.isEmpty())
            return;
    }

    mervin::OcrService ocr(engine_);
    mervin::OcrPopup popup(installed, settings_.ocrDefaultLanguage, this);

    QThreadPool worker;
    worker.setMaxThreadCount(1);
    std::shared_ptr<std::atomic<bool>> canceled;
    const auto lifetime = v->document()->lifetime();
    const QString tessdata = mervin::TessdataManager::directory();
    auto recognize = [&](const QStringList &langs) {
        if (canceled)
            canceled->store(true);
        worker.clear();
        canceled = std::make_shared<std::atomic<bool>>(false);
        popup.setRawText(tr("Recognizing…"));
        worker.start([&, token = canceled, langs, lifetime, tessdata] {
            QString error;
            const QString text = ocr.recognize(lifetime, pageNo, pageRect, langs,
                                               tessdata, &error, token.get());
            QMetaObject::invokeMethod(&popup, [&, token, text, error] {
                if (!token->load())
                    popup.setRawText(error.isEmpty() ? text : tr("[OCR failed: %1]").arg(error));
            }, Qt::QueuedConnection);
        });
    };

    connect(&popup, &mervin::OcrPopup::recognizeRequested, this,
            [&](const QStringList &langs) { recognize(langs); });
    connect(&popup, &mervin::OcrPopup::manageLanguagesRequested, this,
            [this, &popup] {
                const QString selected = popup.selectedLanguages().value(0);
                const QString previousDefault = settings_.ocrDefaultLanguage;
                mervin::ManageLanguagesDialog manager(previousDefault, this);
                manager.exec();

                settings_.ocrDefaultLanguage = manager.defaultLanguage();
                settings_.save();
                const QStringList refreshed = mervin::TessdataManager::installedLanguages();
                const QString preferred = settings_.ocrDefaultLanguage != previousDefault
                    || !refreshed.contains(selected)
                    ? settings_.ocrDefaultLanguage
                    : selected;
                popup.refreshLanguages(refreshed, preferred);
            });

    recognize(popup.selectedLanguages());
    popup.exec();
    if (canceled)
        canceled->store(true);
    worker.clear();
    worker.waitForDone();
}

void MainWindow::toggleMeasure(bool on)
{
    auto *t = currentTab();
    if (!t) {
        if (measureAction_ && measureAction_->isChecked()) {
            QSignalBlocker block(measureAction_);
            measureAction_->setChecked(false);
        }
        return;
    }
    ViewerWidget *v = t->viewer();
    if (on)
        v->setOcrMode(false); // measure and OCR are mutually exclusive
    v->setMeasureMode(on);    // emits measureModeChanged -> panel show/hide + action sync
}

void MainWindow::toggleForms(bool on)
{
    auto *t = currentTab();
    if (!t) {
        if (formAction_ && formAction_->isChecked()) {
            QSignalBlocker block(formAction_);
            formAction_->setChecked(false);
        }
        return;
    }
    ViewerWidget *v = t->viewer();
    if (on) {
        v->setOcrMode(false);     // forms, measure and OCR are mutually exclusive
        v->setMeasureMode(false);
    }
    v->setFormMode(on); // emits formModeChanged -> action sync
}

void MainWindow::toggleComment(bool on)
{
    auto *t = currentTab();
    if (!t) {
        if (commentAction_ && commentAction_->isChecked()) {
            QSignalBlocker block(commentAction_);
            commentAction_->setChecked(false);
        }
        return;
    }
    // Opens/closes the Comment window; it docks beside the measure panel and both
    // can be open at once (only the single active gesture is exclusive).
    t->viewer()->setCommentToolEnabled(on);
}

void MainWindow::onCalibrationLineDrawn(int pageNo, double lengthPoints)
{
    auto *t = currentTab();
    if (!t)
        return;
    const mervin::MeasurePanel *panel = t->measurePanel();
    const mervin::MeasureUnit unit =
        panel ? panel->unit() : mervin::MeasureUnit::Millimeter;
    mervin::CalibrationDialog dlg(mervin::CalibrationDialog::Mode::Calibrate, lengthPoints, unit,
                                  this);
    if (dlg.exec() == QDialog::Accepted) {
        const mervin::MeasureScale s = dlg.result();
        if (s.valid())
            t->viewer()->setPageScaleOverride(pageNo, s);
    } else if (!t->viewer()->pageHasScale(pageNo)) {
        // Cancelled with still no usable scale: the drawn line is discarded and no
        // measurement is kept (a measurement without a scale isn't valid). Re-arm
        // calibration so the next drawn line prompts for the scale again.
        t->viewer()->beginCalibration();
    } else {
        // Cancelled a manual recalibration on an already-scaled page: just resume
        // measuring with the existing scale.
        t->viewer()->cancelCalibration();
    }
}

void MainWindow::onSetScaleRequested(int pageNo)
{
    auto *t = currentTab();
    if (!t)
        return;
    const mervin::MeasurePanel *panel = t->measurePanel();
    const mervin::MeasureUnit unit = panel ? panel->unit() : mervin::MeasureUnit::Millimeter;
    mervin::CalibrationDialog dlg(mervin::CalibrationDialog::Mode::SetScale, 0.0, unit, this);
    if (dlg.exec() == QDialog::Accepted) {
        const mervin::MeasureScale s = dlg.result();
        if (s.valid())
            t->viewer()->setPageScaleOverride(pageNo, s);
    }
    // Cancel is a no-op: the page keeps whatever scale it already had.
}

mervin::TabPage *MainWindow::releaseTab(int index)
{
    auto *t = qobject_cast<TabPage *>(tabs_->widget(index));
    if (!t)
        return nullptr;
    tabs_->removeTab(index); // does NOT delete the page; fires currentChanged
    syncDocTabBar();
    t->setParent(nullptr);   // detach from the old tab widget before re-homing
    updateForCurrentTab();   // re-wire to the new current tab / show start page
    if (wm_)
        wm_->updateSession();
    return t;
}

void MainWindow::adoptTab(mervin::TabPage *page, int atIndex)
{
    if (!page)
        return;
    const int idx = (atIndex >= 0 && atIndex <= tabs_->count())
                        ? tabs_->insertTab(atIndex, page, page->tabTitle())
                        : tabs_->addTab(page, page->tabTitle());
    tabs_->setTabToolTip(idx, QDir::toNativeSeparators(page->path()));
    tabs_->setCurrentIndex(idx); // fires currentChanged -> wires viewer here
    syncDocTabBar();
    updateForCurrentTab();
    if (wm_)
        wm_->updateSession();
}

void MainWindow::onTabDetachRequested(int index, const QPoint &globalPos)
{
    if (wm_)
        wm_->detachTab(this, index, globalPos);
}

void MainWindow::onTabMergeRequested(mervin::DetachableTabBar *source, int sourceIndex,
                                     int targetIndex)
{
    if (!source)
        return;
    auto *srcWin = qobject_cast<MainWindow *>(source->window());
    if (srcWin == this) {
        // Dropped back onto its own bar: a reorder. The insertion index counts
        // the dragged tab, so shift the destination when moving rightwards.
        int dest = (sourceIndex < targetIndex) ? targetIndex - 1 : targetIndex;
        dest = qBound(0, dest, tabs_->count() - 1);
        if (sourceIndex >= 0 && dest != sourceIndex) {
            tabs_->tabBar()->moveTab(sourceIndex, dest);
            syncDocTabBar(); // the QDrag path didn't move the visible bar - refresh it
        }
        return;
    }
    if (wm_)
        wm_->mergeTab(srcWin, sourceIndex, this, targetIndex);
}

void MainWindow::syncViewActions(ViewerWidget *viewer)
{
    const bool has = viewer != nullptr;
    // The document theme is a global radio group (always available); only the
    // per-document page-layout actions hinge on having an open viewer here.
    for (QAction *a : {continuousAction_, singleAction_, twoPageAction_})
        a->setEnabled(has);
    if (!has)
        return;

    // Mode actions use triggered() (not toggled()), so setChecked won't re-fire.
    // The two axes are set independently - the spread check never disturbs the
    // scroll radio group, which is the whole point of keeping them apart.
    const ViewLayout::Mode lm = viewer->layoutMode();
    if (lm.scroll == ViewLayout::Scroll::Single)
        singleAction_->setChecked(true);
    else
        continuousAction_->setChecked(true);
    twoPageAction_->setChecked(lm.spread);
}
