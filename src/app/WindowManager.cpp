#include "app/WindowManager.h"

#include "app/Relaunch.h"
#include "config/Settings.h"
#include "ipc/Message.h"
#include "ipc/SingleInstanceServer.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QScopedValueRollback>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QSet>
#include <QStyleHints>
#include <QTimer>

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
    connect(activityTimer_, &QTimer::timeout, this, &WindowManager::checkDocumentActivity);
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
    if (recent_.add(canonicalPath, QDateTime::currentMSecsSinceEpoch(), pageCount)) {
        recent_.save(recentFile_);
        emit recentListChanged(recent_.entries());
    }
    if (restoreViewState)
        applyViewStateIfStored(canonicalPath);
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
    QStringList paths;
    paths.reserve(recent_.count());
    for (const RecentEntry &e : recent_.entries())
        paths << e.path;
    prunePool_.start([this, paths] {
        QStringList removed;
        for (const QString &path : paths) {
            if (pruneCancel_)
                return;
            if (RecentStore::isRemovedFromDisk(path))
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

QStringList WindowManager::sessionPaths()
{
    session_.load();
    return session_.paths();
}

void WindowManager::updateSession()
{
    // Do not persist an incomplete restore: pending entries would disappear from the saved
    // session. The final entry leaves the queue before opening, so its following save sees the
    // complete set.
    if (!stagedQueue_.isEmpty())
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
    session_.setPaths(all);

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
    pruneCancel_ = true; // prunePool_ waits for the scan; make it stop early
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
    if (active_ == w)
        active_ = windows_.isEmpty() ? nullptr : windows_.last();
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
        surfaceWindow(activeOrNewWindow());
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
    TabPage *page = source->releaseTab(index);
    if (!page)
        return;
    MainWindow *w = createWindow();
    w->adoptTab(page, -1);
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
    TabPage *page = source->releaseTab(sourceIndex);
    if (!page)
        return;
    target->adoptTab(page, targetIndex);
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

bool WindowManager::canCloseToTray() const
{
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
        for (MainWindow *window : std::as_const(windows_))
            if (window->isInTray())
                window->restoreFromTray();
        tray_->hide();
    }
}

void WindowManager::restoreFromTray()
{
    bool restored = false;
    for (MainWindow *window : std::as_const(windows_)) {
        if (window->isInTray()) {
            window->restoreFromTray();
            restored = true;
        }
    }
    if (!restored)
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
    if (windows_.contains(w))
        active_ = w;
}

} // namespace mervin
