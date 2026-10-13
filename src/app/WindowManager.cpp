#include "app/WindowManager.h"

#include "app/Relaunch.h"
#include "config/Settings.h"
#include "ipc/Message.h"
#include "ipc/SingleInstanceServer.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/TabPage.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QScopedValueRollback>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QSet>
#include <QStyleHints>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin {

namespace {

QString canonicalOf(const QString &path)
{
    QFileInfo fi(path);
    const QString c = fi.canonicalFilePath();
    return c.isEmpty() ? fi.absoluteFilePath() : c;
}

// Restore without losing maximized state and activate. Windows uses foreground rights granted
// by the forwarding process; without a grant, fall back to taskbar flashing.
void surfaceWindow(MainWindow *w)
{
    if (!w)
        return;
    if (w->isMinimized())
        w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    w->show();
    w->raise();
    w->activateWindow();
#ifdef Q_OS_WIN
    // Fetch the HWND fresh each time: toggling Always on Top recreates the
    // native window, so a cached handle would go stale.
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
#endif
}

} // namespace

WindowManager::WindowManager()
    : engine_(std::make_unique<RenderEngine>())
    , recentFile_(RecentStore::defaultFile())
    , viewStateFile_(ViewStateStore::defaultFile())
{
    Settings s = Settings::load();
    unloadInactiveMinutes_ = s.unloadInactiveMinutes;
    closeToTray_ = s.closeToTray;
    colorScheme_ = s.colorScheme;
    documentTheme_ = s.documentTheme;
    applyColorSchemeToQt(colorScheme_);

    // Apply theme before showing windows. Scheme-change signals precede palette updates, so
    // subsequent rebuilds run next event-loop turn. Accent changes explicitly reapply the
    // theme.
    Theme::applyApp();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { scheduleThemeRefresh(); });
#endif
    // An OS accent change leaves the scheme alone, so the signal above misses it.
    // Windows and Qt's GTK theme report it as QEvent::ThemeChange. Plasma's
    // platform theme instead replaces the whole application palette, Nord or Cool
    // slate included. applyApp() pins the accent into the palette, so it runs
    // again on either event to read the new accent and restore its own palette.
    qApp->installEventFilter(this);

    // This process owns the recent history + view-state stores (single writer).
    recent_.setRetention(s.recentRetention);
    viewState_.setRetention(s.recentRetention);
    recent_.load(recentFile_);       // missing file -> empty store (normal)
    viewState_.load(viewStateFile_);
    keepMissingRecent_ = s.recentKeepMissing;
    prunePool_.setMaxThreadCount(1);
    tabScanPool_.setMaxThreadCount(1);

    activityClock_.start();
    trayMenu_ = std::make_unique<QMenu>();
    //: Tray icon menu item: show the windows hidden in the tray.
    trayMenu_->addAction(tr("Show Mervin"), this, &WindowManager::restoreFromTray);
    trayMenu_->addSeparator();
    //: Tray icon menu item: close every window and exit.
    trayMenu_->addAction(tr("Quit Mervin"), this, &WindowManager::quitAll);
    tray_ = new QSystemTrayIcon(icons::applicationIcon(), this);
    //: Tooltip of the tray icon. The product name.
    tray_->setToolTip(tr("Mervin PDF"));
    tray_->setContextMenu(trayMenu_.get());
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            restoreFromTray();
    });
    updateTrayAvailability();
    activityTimer_ = new QTimer(this);
    activityTimer_->setInterval(1000);
    connect(activityTimer_, &QTimer::timeout, this, [this] {
        checkDocumentActivity();
        // A busy window kept a tab the last check found gone. Check again rather
        // than act on that old answer: the file may be back, or its share away.
        // Only this tick retries. The other callers of checkDocumentActivity run
        // inside MainWindow handlers.
        if (std::exchange(removedTabsWaiting_, false))
            closeRemovedTabs();
    });
    activityTimer_->start();
}

void WindowManager::applyColorSchemeToQt(const QString &scheme)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    Qt::ColorScheme cs = Qt::ColorScheme::Unknown; // follow system
    if (scheme == QLatin1String("dark"))
        cs = Qt::ColorScheme::Dark;
    else if (scheme == QLatin1String("light"))
        cs = Qt::ColorScheme::Light;
    QGuiApplication::styleHints()->setColorScheme(cs);
#else
    // Older Qt source builds follow the platform palette; forced colour schemes require Qt
    // 6.8+. Preserve the preference for future upgrades.
    Q_UNUSED(scheme);
#endif
}

void WindowManager::setColorScheme(const QString &scheme)
{
    if (colorScheme_ == scheme)
        return;
    colorScheme_ = scheme;
    // Order matters: applyColorSchemeToQt fires QStyleHints::colorSchemeChanged
    // synchronously, which queues the deferred Theme::applyApp() BEFORE the
    // per-window re-setup that colorSchemeChanged queues below - so each window
    // re-reads a palette and sheet that have already settled.
    applyColorSchemeToQt(scheme);
    // ...except when Qt has nothing to change: switching to "system" while the OS
    // already matches the previously forced scheme emits no styleHints signal at
    // all. Ask for the rebuild explicitly; scheduleThemeRefresh coalesces, so this
    // costs nothing in the common case.
    scheduleThemeRefresh();

    Settings s = Settings::load();
    s.colorScheme = scheme;
    s.save();

    emit colorSchemeChanged(scheme);
}

void WindowManager::setDocumentTheme(const QString &theme)
{
    if (documentTheme_ == theme)
        return;
    documentTheme_ = theme;

    Settings s = Settings::load();
    s.documentTheme = theme;
    s.save();

    emit documentThemeChanged(theme);
}

void WindowManager::setAutoUpdate(bool on)
{
    Settings s = Settings::load();
    if (s.autoUpdate != on) {
        s.autoUpdate = on;
        s.save();
    }
    emit autoUpdateChanged(on);
}

bool WindowManager::eventFilter(QObject *watched, QEvent *event)
{
    // Both are coalesced. applyApp() raises no ThemeChange, and the palette
    // changes it makes itself are skipped, so neither can loop.
    const bool foreignPalette = event->type() == QEvent::ApplicationPaletteChange
                                && watched == qApp && !Theme::applying();
    if (event->type() == QEvent::ThemeChange || foreignPalette)
        scheduleThemeRefresh();
    return QObject::eventFilter(watched, event);
}

void WindowManager::scheduleThemeRefresh()
{
    if (themeRefreshPending_)
        return; // coalesce a burst of scheme/palette signals into one rebuild
    themeRefreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        themeRefreshPending_ = false;
        Theme::applyApp(); // palette has settled by now -> correct light/dark sheet
    });
}

void WindowManager::adoptInstanceServer(std::unique_ptr<ipc::SingleInstanceServer> server)
{
    instanceServer_ = std::move(server);
    if (!instanceServer_)
        return;
    connect(instanceServer_.get(), &ipc::SingleInstanceServer::messageReceived, this,
            &WindowManager::onInstanceMessage);
}

void WindowManager::onInstanceMessage(QLocalSocket *socket, const ipc::Message &msg)
{
    if (msg.cmd != ipc::Message::Cmd::Open)
        return; // tolerate any other command a future/older client might send

    // Acknowledge before opening: nested password/error dialogs can block indefinitely and
    // would make the sender launch another primary. Never access the socket afterward; nested
    // loops may destroy it.
    ipc::SingleInstanceServer::send(socket, ipc::Message::ack(QStringLiteral("open")));

    // Defer the (possibly modal) open to a fresh event-loop turn so it does not
    // run nested inside this socket-read handler. Empty paths means "bring the
    // app up" - openPaths surfaces a window; otherwise files open per behaviour.
    const QStringList paths = msg.paths;
    const QString behavior = msg.behavior;
    QMetaObject::invokeMethod(
        this, [this, paths, behavior] { openPaths(paths, behavior); }, Qt::QueuedConnection);
}

void WindowManager::recordOpen(const QString &canonicalPath, bool restoreViewState, int pageCount)
{
    if (canonicalPath.isEmpty())
        return;
    // A file that is not there now, such as a crash recovery tab opened lazily or a
    // tab focused again after its share went away, keeps the mount it was opened
    // from. Recording now would store the file system below the empty folder.
    const QString mount = QFileInfo::exists(canonicalPath) ? RecentStore::mountOf(canonicalPath)
                                                           : recordedMount(canonicalPath);
    // The session keeps it too, for when Recent no longer lists the file.
    session_.setMount(canonicalPath, mount);
    if (recent_.add(canonicalPath, QDateTime::currentMSecsSinceEpoch(), pageCount, mount)) {
        recent_.save(recentFile_);
        emit recentListChanged(recent_.entries());
    }
    if (restoreViewState)
        applyViewStateIfStored(canonicalPath);
}

QString WindowManager::recordedMount(const QString &canonicalPath) const
{
    const QString mount = session_.mount(canonicalPath);
    return mount.isEmpty() ? recent_.recordedMount(canonicalPath) : mount;
}

void WindowManager::applyViewStateIfStored(const QString &canonicalPath)
{
    const std::optional<ViewState> st = viewState_.get(canonicalPath);
    if (!st)
        return;
    for (MainWindow *w : std::as_const(windows_))
        if (w->applyViewState(canonicalPath, *st))
            break; // applied to the one tab holding this file
}

void WindowManager::saveViewState(const QString &path, const ViewState &state)
{
    saveViewStates({{path, state}});
}

void WindowManager::saveViewStates(const QList<QPair<QString, ViewState>> &states)
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &[path, state] : states)
        if (!path.isEmpty())
            viewState_.put(path, state, now);
    if (!states.isEmpty())
        viewState_.save(viewStateFile_);
}

void WindowManager::removeRecent(const QString &canonicalPath)
{
    if (canonicalPath.isEmpty())
        return;
    const bool removedRecent = recent_.remove(canonicalPath);
    const bool removedState = viewState_.remove(canonicalPath); // drop its resume state too
    if (removedRecent) {
        recent_.save(recentFile_);
        emit recentListChanged(recent_.entries());
    }
    if (removedState)
        viewState_.save(viewStateFile_);
}

void WindowManager::clearMissingRecent(const QStringList &paths)
{
    const QStringList removed = recent_.removeMissingFiles(paths);
    if (removed.isEmpty())
        return;

    recent_.save(recentFile_);
    emit recentListChanged(recent_.entries());

    bool removedState = false;
    for (const QString &path : removed)
        removedState = viewState_.remove(path) || removedState;
    if (removedState)
        viewState_.save(viewStateFile_);
}

void WindowManager::setFavorite(const QString &canonicalPath, bool favorite)
{
    if (canonicalPath.isEmpty())
        return;
    if (recent_.setFavorite(canonicalPath, favorite)) {
        recent_.save(recentFile_);
        emit recentListChanged(recent_.entries());
    }
}

void WindowManager::refreshRecent()
{
    emit recentListChanged(recent_.entries());
    pruneMissingRecent();
}

void WindowManager::setAnnotationDefaults(const QString &color, const QString &author)
{
    emit annotationDefaultsChanged(color, author);
}

void WindowManager::applyRecentSettings(int visibleCount, int retention, bool keepMissing)
{
    const int recentBefore = recent_.count();
    const int statesBefore = viewState_.count();
    recent_.setRetention(retention);
    viewState_.setRetention(retention);
    // setRetention trims in memory only; persist now rather than on the next open.
    if (recent_.count() != recentBefore) {
        recent_.save(recentFile_);
        emit recentListChanged(recent_.entries());
    }
    if (viewState_.count() != statesBefore)
        viewState_.save(viewStateFile_);
    keepMissingRecent_ = keepMissing;
    emit recentVisibleCountChanged(visibleCount);
    pruneMissingRecent();
}

void WindowManager::pruneMissingRecent()
{
    if (keepMissingRecent_ || recent_.count() == 0)
        return;
    if (pruneRunning_) {
        prunePending_ = true;
        return;
    }
    pruneRunning_ = true;
    // Each path with the mount recorded when it was opened.
    QList<std::pair<QString, QString>> paths;
    paths.reserve(recent_.count());
    for (const RecentEntry &e : recent_.entries())
        paths.append({e.path, e.mount});
    prunePool_.start([this, paths] {
        QStringList removed;
        for (const auto &[path, mount] : paths) {
            if (scanCancel_)
                return;
            if (RecentStore::isRemovedFromDisk(path, mount))
                removed << path;
        }
        QMetaObject::invokeMethod(
            this,
            [this, removed] {
                pruneRunning_ = false;
                // clearMissingRecent re-checks each path, so a file restored in the
                // meantime stays; it also drops view state and tells every window.
                if (!keepMissingRecent_ && !removed.isEmpty())
                    clearMissingRecent(removed);
                if (std::exchange(prunePending_, false))
                    pruneMissingRecent();
            },
            Qt::QueuedConnection);
    });
}

void WindowManager::closeRemovedTabs()
{
    if (shuttingDown_ || quitting_)
        return;
    if (tabScanRunning_) {
        tabScanPending_ = true;
        return;
    }
    // Each unloaded tab's file, once per canonical path, with the mount it was
    // opened from. Tabs with unsaved edits reload from their recovery copy.
    struct File
    {
        QString path;
        QString canonicalPath;
        QString mount;
    };
    QList<File> files;
    QSet<QString> listed;
    for (MainWindow *w : std::as_const(windows_)) {
        if (w->isInTray())
            continue;
        for (const TabPage *tab : w->unloadedTabs()) {
            if (tab->hasUnsavedEdits() || listed.contains(tab->canonicalPath()))
                continue;
            listed.insert(tab->canonicalPath());
            files.append({tab->path(), tab->canonicalPath(), recordedMount(tab->canonicalPath())});
        }
    }
    if (files.isEmpty())
        return;
    tabScanRunning_ = true;
    tabScanPool_.start([this, files] {
        QStringList removed;
        for (const File &f : files) {
            if (scanCancel_)
                return;
            // The rule session restore uses (SessionStore::prepareRestore): gone only
            // from a folder that is still there. A missing folder can also be a
            // volume away, such as another USB stick under the same drive letter.
            const QFileInfo file(f.canonicalPath);
            if (!QFileInfo::exists(f.path) && file.absoluteDir().exists()
                && RecentStore::isRemovedFromDisk(f.canonicalPath, f.mount))
                removed << f.canonicalPath;
        }
        QMetaObject::invokeMethod(
            this,
            [this, removed] {
                tabScanRunning_ = false;
                closeFoundRemovedTabs(removed);
                if (std::exchange(tabScanPending_, false))
                    closeRemovedTabs();
            },
            Qt::QueuedConnection);
    });
}

void WindowManager::closeFoundRemovedTabs(const QStringList &canonicalPaths)
{
    if (shuttingDown_ || quitting_)
        return;
    for (const QString &path : canonicalPaths)
        for (MainWindow *w : std::as_const(windows_)) // closing a tab keeps its window
            if (!w->closeRemovedTabs(path))
                removedTabsWaiting_ = true;
}

QStringList WindowManager::sessionPathsToRestore()
{
    session_.load();
    // Each file is judged by the file system it was on when opened, as the session
    // recorded it. A session saved before sessions recorded mounts has them in Recent.
    return session_.prepareRestore(QDate::currentDate(), {}, [this](const QString &path) {
        return recent_.recordedMount(path);
    });
}

void WindowManager::updateSession()
{
    // Do not persist an incomplete restore: pending entries would disappear from the saved
    // session. The final entry leaves the queue before opening, so its following save sees the
    // complete set. Nor a tab halfway between windows, which would lose its recorded mount.
    if (!stagedQueue_.isEmpty() || movingTab_)
        return;

    // Union of every live window's open documents, in first-seen order.
    QStringList all;
    QSet<QString> seen;
    for (MainWindow *w : std::as_const(windows_))
        for (const QString &p : w->tabPaths())
            if (!p.isEmpty() && !seen.contains(p)) {
                seen.insert(p);
                all.append(p);
            }
    session_.setPaths(all); // a kept document open again leaves the kept list

    // Which document is on screen, so the next start can open that one first
    // instead of making the user wait behind the rest of the session. The active
    // window's current tab wins; with no active window (or one showing no
    // document) fall back to the first window that has one.
    QString activePath;
    if (active_ && windows_.contains(active_))
        activePath = active_->currentTabPath();
    if (activePath.isEmpty()) {
        for (MainWindow *w : std::as_const(windows_)) {
            activePath = w->currentTabPath();
            if (!activePath.isEmpty())
                break;
        }
    }
    session_.setActivePath(all.contains(activePath) ? activePath : QString());
    session_.save();
}

void WindowManager::openStaged(const QList<StagedOpen> &batch, const QStringList &savedOrder,
                               std::function<void()> onDone)
{
    stagedQueue_ = batch;
    stagedOrder_ = savedOrder;
    stagedDone_ = std::move(onDone);
    // Pin the window the batch belongs to. Re-resolving it per turn would follow
    // active_, and the whole point of staging is that the UI is live while the
    // batch drains - so Ctrl+N, "Open in new window", a detached tab or an
    // Explorer open with the "new window" behaviour would all make the fresh
    // window active and divert the rest of the session into it.
    stagedWindow_ = activeOrNewWindow();
    QTimer::singleShot(0, this, &WindowManager::stagedStep);
}

void WindowManager::stagedStep()
{
    // The window this batch belongs to can be closed (or the process asked to
    // quit) while the batch is still trickling in. Stop rather than resurrect a
    // window to open into: the session file still lists those documents, so the
    // next start restores them.
    if (shuttingDown_ || !stagedWindow_ || !windows_.contains(stagedWindow_))
        stagedQueue_.clear();

    if (stagedQueue_.isEmpty()) {
        // Background restored tabs stay unloaded. Wait for any selected tab's asynchronous
        // opening before startup timing or a caller's completion callback can finish.
        if (stagedWindow_ && stagedWindow_->hasLoadingDocuments()) {
            QTimer::singleShot(10, this, &WindowManager::stagedStep);
            return;
        }
        stagedWindow_ = nullptr;
        stagedOrder_.clear();
        // Move the callback out before running it: it may start another batch.
        const std::function<void()> done = std::move(stagedDone_);
        stagedDone_ = {};
        if (done)
            done();
        return;
    }

    const StagedOpen o = stagedQueue_.takeFirst();
    MainWindow *w = stagedWindow_;
    // The tab index comes from the live tab bar, not from the plan: an open that
    // does not produce a tab (a corrupt file, a cancelled password prompt, a
    // document already open because it was also named on the command line) then
    // leaves the remaining documents in their saved order instead of shifting
    // every one of them.
    const int atIndex = insertIndexForSaved(w->tabPaths(), stagedOrder_, o.savedIndex);
    w->openFile(o.path, o.allowDuplicate, atIndex, o.makeCurrent, {}, o.lazy);

    // One document per event-loop turn: Qt gets to paint and handle input between
    // documents, so the window stays live through a slow cold-cache batch.
    QTimer::singleShot(0, this, &WindowManager::stagedStep);
}

void WindowManager::rememberClosedTab(const QString &path, const QStringList &siblings, int index)
{
    if (path.isEmpty())
        return;
    ClosedTab t;
    t.path = path;
    t.canonicalPath = canonicalOf(path);
    t.siblings = siblings;
    t.index = qMax(0, index);
    closedTabs_.push(t);
}

void WindowManager::pruneClosedTabs()
{
    closedTabs_.prune([this](const ClosedTab &t) {
        // Open again (from Recent, from Explorer, or by an earlier press): there
        // is nothing left to restore. Gone from disk: nothing to restore it from.
        return !isOpenAnywhere(t.canonicalPath) && QFileInfo::exists(t.path);
    });
}

WindowManager::Reopen WindowManager::reopenClosedTab(MainWindow *into)
{
    pruneClosedTabs();
    while (!closedTabs_.isEmpty()) {
        const ClosedTab t = closedTabs_.pop();
        // Re-checked after the pop as well: the prune above ran before this loop,
        // and a password prompt inside openFile() below runs a nested event loop
        // in which the world can change.
        if (isOpenAnywhere(t.canonicalPath) || !QFileInfo::exists(t.path))
            continue;

        MainWindow *w = (into && windows_.contains(into)) ? into : activeOrNewWindow();
        // Where it belongs in the bar as it stands NOW: after whichever of the tabs
        // it was closed alongside are already back, and before everything else.
        // Re-derived rather than replayed (see ClosedTab), which is what makes
        // undoing a whole window's worth of closes rebuild the original order
        // however many presses in, and in whatever order the entries come back.
        const int at = insertIndexForSaved(w->tabPaths(), t.siblings, t.index);
        const bool ok = w->openFile(t.path, /*allowDuplicate=*/false, at,
                                    /*makeCurrent=*/true);
        if (ok) {
            surfaceWindow(w);
            return Reopen::Reopened;
        }
        // Consumed either way. A document that cannot be opened - a cancelled
        // password prompt, a file that went corrupt - must not be handed back on
        // every press from here on. The press was still spent on a real entry,
        // so this is not the same as an empty history.
        return Reopen::Failed;
    }
    return Reopen::NothingToReopen;
}

bool WindowManager::isOpenAnywhere(const QString &canonicalPath) const
{
    if (canonicalPath.isEmpty())
        return false;
    for (MainWindow *w : windows_)
        if (w->tabPaths().contains(canonicalPath))
            return true;
    return false;
}

QStringList WindowManager::openTabPaths() const
{
    QStringList paths;
    for (MainWindow *w : windows_)
        paths << w->tabPaths();
    return paths;
}

WindowManager::~WindowManager()
{
    shuttingDown_ = true;
    if (activityTimer_)
        activityTimer_->stop();
    if (tray_)
        tray_->hide();
    scanCancel_ = true; // the scan pools wait for their scans; make them stop early
    // Stop the render workers first, then free any windows still alive (their
    // Documents), then let engine_ drop the MuPDF context (member destruction,
    // after this body) - Documents are all gone by then.
    if (engine_)
        engine_->shutdown();
    const QList<MainWindow *> copy = windows_;
    windows_.clear();
    for (MainWindow *w : copy)
        delete w;
}

MainWindow *WindowManager::createWindow()
{
    auto *w = new MainWindow(engine_.get(), this);
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(w, &QObject::destroyed, this, [this, w] { onWindowDestroyed(w); });
    windows_.append(w);
    active_ = w;
    w->show();
    return w;
}

void WindowManager::onWindowDestroyed(MainWindow *w)
{
    // Retain pointer identity; the MainWindow subobject has already been destroyed.
    windows_.removeOne(w);
    if (active_ == w) {
        // A window still on screen, else the one hidden in the tray last, else the
        // newest. An empty window closed for real must not hand over to an older
        // tray window just because it was created later.
        active_ = nullptr;
        for (MainWindow *window : std::as_const(windows_))
            if (window->isVisible() && !window->isInTray())
                active_ = window;
        if (!active_) {
            const QList<MainWindow *> hidden = trayWindows();
            active_ = !hidden.isEmpty() ? hidden.last()
                      : windows_.isEmpty() ? nullptr : windows_.last();
        }
    }
    if (stagedWindow_ == w)
        stagedWindow_ = nullptr; // stagedStep then abandons the rest of the batch

    if (windows_.isEmpty() && !shuttingDown_) {
        // Last window gone: stop workers before this window's Documents are
        // freed (children are deleted right after this destroyed() handler),
        // then quit the process. Nothing is left running afterwards.
        if (engine_)
            engine_->shutdown();
        QCoreApplication::quit();
    }
}

MainWindow *WindowManager::activeOrNewWindow()
{
    if (active_ && windows_.contains(active_))
        return active_;
    if (!windows_.isEmpty())
        return windows_.last();
    return createWindow();
}

MainWindow *WindowManager::emptyWindow() const
{
    for (MainWindow *w : windows_)
        if (w->tabCount() == 0)
            return w;
    return nullptr;
}

bool WindowManager::focusExistingTab(const QString &canonicalPath)
{
    if (canonicalPath.isEmpty())
        return false;
    for (MainWindow *w : std::as_const(windows_)) {
        if (w->focusTabIfOpen(canonicalPath)) {
            surfaceWindow(w);
            active_ = w;
            // Re-opening an already-open file (e.g. from Explorer) still bumps
            // its recency, but there is no view state to restore - it's live.
            recordOpen(canonicalPath, /*restoreViewState=*/false);
            return true;
        }
    }
    return false;
}

void WindowManager::openPaths(const QStringList &paths, const QString &behavior)
{
    if (paths.isEmpty()) {
        // A launch without files brings the app up, like a tray click: every window
        // hidden in the tray returns, not only the active one.
        restoreFromTray();
        return;
    }

    const bool newWin = (behavior == QLatin1String("new-window"));
    MainWindow *batchWindow = nullptr;

    for (const QString &path : paths) {
        const QString canon = canonicalOf(path);
        if (focusExistingTab(canon))
            continue; // already open somewhere - focused, not duplicated

        MainWindow *target;
        if (newWin) {
            if (!batchWindow) {
                // Reuse a document-less window if one exists rather than
                // leaving an empty stray window behind.
                batchWindow = emptyWindow();
                if (!batchWindow)
                    batchWindow = createWindow();
            }
            target = batchWindow;
        } else {
            target = activeOrNewWindow();
        }
        target->openFile(path);
    }

    MainWindow *focusW = batchWindow ? batchWindow : active_;
    if (focusW)
        surfaceWindow(focusW);
}

void WindowManager::detachTab(MainWindow *source, int index, const QPoint &globalPos)
{
    if (!source || source->tabCount() <= 1)
        return; // dragging the only tab out would just leave an empty window
    MainWindow *w = nullptr;
    {
        const QScopedValueRollback<bool> moving(movingTab_, true);
        TabPage *page = source->releaseTab(index);
        if (!page)
            return;
        w = createWindow();
        w->adoptTab(page, -1);
    }
    updateSession();
    w->move(globalPos); // drop position becomes the new window's top-left
    w->raise();
    w->activateWindow();
}

void WindowManager::duplicateToNewWindow(const QString &path, const QString &password,
                                         const ViewState &state, const QPoint &globalPos)
{
    if (path.isEmpty())
        return;
    MainWindow *w = createWindow();
    if (w->openFile(path, /*allowDuplicate=*/true, /*atIndex=*/-1, /*makeCurrent=*/true,
                    password)) {
        w->applyViewState(canonicalOf(path), state); // mirror the source view
        w->move(globalPos);                           // offset from the source window
    }
    w->raise();
    w->activateWindow();
}

void WindowManager::mergeTab(MainWindow *source, int sourceIndex, MainWindow *target,
                             int targetIndex)
{
    if (!source || !target || source == target)
        return; // same-window reorder is handled in MainWindow
    {
        const QScopedValueRollback<bool> moving(movingTab_, true);
        TabPage *page = source->releaseTab(sourceIndex);
        if (!page)
            return;
        target->adoptTab(page, targetIndex);
    }
    updateSession();
    target->raise();
    target->activateWindow();
    active_ = target;
    if (source->tabCount() == 0)
        source->closeForQuit(); // its last tab moved away; don't retain an empty tray window
}

void WindowManager::newWindow()
{
    // Reuse an existing empty window rather than proliferating blank windows.
    MainWindow *w = emptyWindow();
    if (!w)
        w = createWindow();
    surfaceWindow(w);
}

void WindowManager::quitAll()
{
    closeAllForQuit();
}

bool WindowManager::closeAllForQuit()
{
    if (quitting_)
        return false;
    QScopedValueRollback<bool> quitGuard(quitting_, true);
    const QList<MainWindow *> copy = windows_;
    for (MainWindow *window : copy)
        if (!window->prepareToClose())
            return false;
    updateSession();
    for (MainWindow *window : copy)
        window->closeForQuit();
    if (copy.isEmpty())
        QCoreApplication::quit();
    return true;
}

bool WindowManager::restart()
{
    if (!closeAllForQuit())
        return false;
    relaunch::request();
    return true;
}

bool WindowManager::canCloseToTray(const MainWindow *window) const
{
    // An empty window kept in the tray would come back with the others on the next
    // restore, in front of the window that holds the documents.
    if (window->tabCount() == 0 && windows_.size() > 1)
        return false;
    return !quitting_ && !shuttingDown_ && closeToTray_ && tray_ && tray_->isVisible()
        && QSystemTrayIcon::isSystemTrayAvailable() && !qApp->isSavingSession();
}

void WindowManager::updateTrayAvailability()
{
    const bool available = closeToTray_ && QSystemTrayIcon::isSystemTrayAvailable();
    if (available) {
        if (!tray_->isVisible())
            tray_->show();
    } else {
        // Never strand a window if its desktop tray disappears or the setting is disabled.
        for (MainWindow *window : trayWindows())
            window->restoreFromTray();
        tray_->hide();
    }
}

QList<MainWindow *> WindowManager::trayWindows() const
{
    QList<MainWindow *> hidden;
    for (MainWindow *window : windows_)
        if (window->isInTray())
            hidden.append(window);
    std::sort(hidden.begin(), hidden.end(), [](const MainWindow *a, const MainWindow *b) {
        return a->trayOrder() < b->trayOrder();
    });
    return hidden;
}

void WindowManager::restoreFromTray()
{
    // Each restore raises its window, so the window hidden last, the one in use when
    // the app went to the tray, ends up in front.
    const QList<MainWindow *> hidden = trayWindows();
    for (MainWindow *window : hidden)
        window->restoreFromTray();
    if (!hidden.isEmpty()) {
        // Activation is reported later. Record the front window now, so the second
        // call of a tray double-click surfaces this window again, not another one.
        active_ = hidden.last();
    }
    surfaceWindow(activeOrNewWindow());
}

void WindowManager::applyMemorySettings(int minutes, bool closeToTray)
{
    unloadInactiveMinutes_ = minutes;
    closeToTray_ = closeToTray;
    emit memorySettingsChanged(minutes, closeToTray);
    updateTrayAvailability();
    checkDocumentActivity();
}

void WindowManager::checkDocumentActivity()
{
    if (shuttingDown_ || quitting_ || checkingActivity_)
        return;
    QScopedValueRollback<bool> guard(checkingActivity_, true);
    updateTrayAvailability();
    bool resident = false;
    const qint64 now = activityClock_.elapsed();
    for (MainWindow *window : std::as_const(windows_)) {
        window->updateDocumentActivity(now, unloadInactiveMinutes_);
        resident = window->hasResidentDocuments() || resident;
    }
    if (resident)
        cacheTrimmed_ = false;
    else if (!cacheTrimmed_) {
        engine_->trimCache();
        cacheTrimmed_ = true;
    }
}

void WindowManager::notifySuspensionFailure(const QString &message)
{
    if (tray_ && tray_->isVisible())
        //: Title of a tray notification: an inactive document could not be unloaded from memory.
        tray_->showMessage(tr("Document remains loaded"), message, QSystemTrayIcon::Warning);
}

void WindowManager::notifyActivated(MainWindow *w)
{
    if (!windows_.contains(w))
        return;
    active_ = w;
    // Returning to Mervin is when a file deleted meanwhile, such as a mail
    // attachment's temporary copy, should take its unloaded tab with it.
    closeRemovedTabs();
}

} // namespace mervin
