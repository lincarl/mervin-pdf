#include "app/WindowManager.h"

#include "config/Settings.h"
#include "ipc/Message.h"
#include "ipc/SingleInstanceServer.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QCoreApplication>
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

// Bring a window to the user: restore it if minimized (preserving a maximized
// state, which showNormal() would drop) and lift it to the foreground. The Qt
// calls are the whole story on Linux; on Windows they are refused while this
// process is in the background - e.g. handling a file forwarded by a secondary
// launch - so a native fallback restores and takes the foreground with the
// rights that launch granted via AllowSetForegroundWindow (see main.cpp). When
// no grant exists the fallback degrades to the OS-sanctioned taskbar flash.
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
    colorScheme_ = s.colorScheme;
    documentTheme_ = s.documentTheme;
    applyColorSchemeToQt(colorScheme_);

    // Central app-wide theme: applied once now (before any window is shown) and
    // rebuilt whenever the effective light/dark scheme changes - including system
    // auto-switches, which only surface as a styleHints signal. The accent comes
    // from Settings, so changing it just calls Theme::applyApp() again. The signal
    // fires before Qt finishes updating the palette, so the rebuild is deferred to
    // the next event-loop turn (see scheduleThemeRefresh). The initial build below
    // is safe to run now: no window exists yet and the forced scheme has settled.
    Theme::applyApp();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { scheduleThemeRefresh(); });
#endif

    // This process owns the recent history + view-state stores (single writer).
    recent_.setRetention(s.recentRetention);
    viewState_.setRetention(s.recentRetention);
    recent_.load(recentFile_);       // missing file -> empty store (normal)
    viewState_.load(viewStateFile_);
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
    // Qt < 6.8 (e.g. Ubuntu 24.04's 6.4) has no QStyleHints::setColorScheme /
    // Qt::ColorScheme. The app then follows the platform palette (themed via
    // Theme::applyApp()); forcing light/dark from Settings is inert here. No
    // shipped artifact lands in this branch - every release build (Windows,
    // AppImage, .deb, .rpm) is Qt 6.8+ - it only covers source builds against an
    // older Qt. The preference is still saved and would apply against newer Qt.
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

    // Acknowledge receipt IMMEDIATELY, before opening anything. The open can pop
    // a modal (encrypted-PDF password prompt, or an error box) that runs a nested
    // event loop for an unbounded time; if we acked only after openPaths(), the
    // sender would time out, give up, and spawn a duplicate standalone process
    // (two concurrent writers to recent.json). Acking first also means we never
    // touch `socket` again, so a nested loop can't free it under us.
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

void WindowManager::saveViewState(const QString &canonicalPath, const ViewState &state)
{
    if (canonicalPath.isEmpty())
        return;
    viewState_.put(canonicalPath, state, QDateTime::currentMSecsSinceEpoch());
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
}

QStringList WindowManager::sessionPaths()
{
    session_.load();
    return session_.paths();
}

void WindowManager::updateSession()
{
    // While a staged batch is still in flight the open set is deliberately
    // incomplete, and the stored session already lists every document of it - so
    // writing now would drop the ones not yet opened. That mattered less when
    // restore ran inline before the event loop: the user could not quit halfway
    // through. They can now, and a truncated session is exactly what restore
    // exists to prevent. The final document of a batch is taken off the queue
    // before it opens, so the write that follows it sees the full state again.
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
    w->openFile(o.path, /*allowDuplicate=*/false, atIndex, o.makeCurrent);

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

WindowManager::~WindowManager()
{
    shuttingDown_ = true;
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
    connect(w, &QObject::destroyed, this, &WindowManager::onWindowDestroyed);
    windows_.append(w);
    active_ = w;
    w->show();
    return w;
}

void WindowManager::onWindowDestroyed(QObject *obj)
{
    // Pointer identity only - obj is mid-destruction, do not dereference it.
    auto *w = static_cast<MainWindow *>(obj);
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

void WindowManager::duplicateToNewWindow(const QString &path, const ViewState &state,
                                         const QPoint &globalPos)
{
    if (path.isEmpty())
        return;
    MainWindow *w = createWindow();
    if (w->openFile(path, /*allowDuplicate=*/true)) {
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
        source->close(); // its last tab moved away; don't leave an empty window
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
    const QList<MainWindow *> copy = windows_;
    if (copy.isEmpty()) {
        QCoreApplication::quit();
        return;
    }
    for (MainWindow *w : copy)
        w->close(); // WA_DeleteOnClose -> deleted; last one quits the process
}

void WindowManager::notifyActivated(MainWindow *w)
{
    if (windows_.contains(w))
        active_ = w;
}

} // namespace mervin
