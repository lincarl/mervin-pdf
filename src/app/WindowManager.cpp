#include "app/WindowManager.h"

#include "app/Relaunch.h"
#include "config/Settings.h"
#include "io/AppFiles.h"
#include "io/Background.h"
#include "ipc/Message.h"
#include "ipc/SingleInstanceServer.h"
#include "recent/PathKey.h"
#include "session/SessionEnd.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/CloseFlow.h"
#include "ui/TabPage.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "update/Updater.h"

#include <QCoreApplication>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QScopedValueRollback>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QSet>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mervin {

namespace {

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

WindowManager::WindowManager(bool ownsStores)
    : engine_(std::make_unique<RenderEngine>())
    , recentFile_(RecentStore::defaultFile())
    , viewStateFile_(ViewStateStore::defaultFile())
    , sessionFile_(SessionStore::defaultFile())
    , ownsStores_(ownsStores)
{
#ifdef Q_OS_WIN
    qApp->installNativeEventFilter(this);
#if QT_CONFIG(sessionmanager)
    connect(qApp, &QGuiApplication::commitDataRequest, this,
            [this](QSessionManager &) { prepareSessionEnd(); }, Qt::DirectConnection);
#endif
#endif
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
    keepFavouriteViewStates();       // before the load trims the view states
    viewState_.load(viewStateFile_);
    keepMissingRecent_ = s.recentKeepMissing;
    connect(&probe_, &FileProbe::checked, this, &WindowManager::onFileChecked);
    mountSaveTimer_ = new QTimer(this);
    mountSaveTimer_->setSingleShot(true);
    mountSaveTimer_->setInterval(kMountSaveMs);
    connect(mountSaveTimer_, &QTimer::timeout, this, &WindowManager::recordFoundMounts);

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
        // Tab changes that nothing else let go, such as after a menu closed with
        // Esc, run here, not in checkDocumentActivity, whose other callers run
        // inside MainWindow handlers.
        runTabChanges();
    });
    activityTimer_->start();
}

sessionend::Result WindowManager::prepareSessionEnd(int graceMs)
{
    const sessionend::Result result = sessionend::waitForRecovery(graceMs);
    if (!reopenPresses_.isEmpty()) {
        // No GUI events run during the wait. Invalidate even a probe whose
        // timeout or answer has not yet observed the pending request.
        reopenCheck_.reset();
        ++reopenAttempt_;
        reopenSessionPaused_ = true;
    }
    return result;
}

void WindowManager::finishSessionEnd(bool ended)
{
    sessionend::finishRequest(ended);
    // The native filter runs before Qt restores input after cancellation. Resume
    // queued dialogs next turn, once the platform processed that outcome.
    if (!ended) {
        postTabChanges();
        postSessionContinuations();
    }
}

bool WindowManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG")
        || eventType == QByteArrayLiteral("windows_dispatcher_MSG")) {
        const auto *event = static_cast<const MSG *>(message);
#if QT_CONFIG(sessionmanager)
        constexpr LPARAM logoffFlag = static_cast<LPARAM>(ENDSESSION_LOGOFF);
        if (event->message == WM_QUERYENDSESSION && (event->lParam & logoffFlag)
            && !forwardingSessionQuery_) {
            const auto windows = QGuiApplication::allWindows();
            const bool owned = std::any_of(windows.cbegin(), windows.cend(), [event](QWindow *window) {
                return window->handle() && reinterpret_cast<HWND>(window->winId()) == event->hwnd;
            });
            if (owned) {
                // Qt 6.12 blocks input and emits commitDataRequest, then flushes
                // every C stream for logoff. A locked writer can exceed our grace.
                // Forward through Qt with only that flush flag removed, preserving
                // its session state, callback and exact cancellation result.
                const QScopedValueRollback guard(forwardingSessionQuery_, true);
                *result = SendMessage(event->hwnd, event->message, event->wParam,
                                      event->lParam & ~logoffFlag);
                return true;
            }
        }
#endif
        if (event->message == WM_ENDSESSION)
            finishSessionEnd(event->wParam != 0);
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    return false; // Qt still handles the OS outcome and its normal quit signal.
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
    // A released button or a closed modal window can let a window change its tabs.
    // The windows are not asked here, as one can be in its destructor.
    if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::WindowUnblocked)
        postTabChanges();
    return QObject::eventFilter(watched, event);
}

void WindowManager::postTabChanges()
{
    // Posted, as the release or the closing window has not reached its widget yet.
    if (tabChangesPosted_)
        return;
    tabChangesPosted_ = true;
    QTimer::singleShot(0, this, [this] {
        tabChangesPosted_ = false;
        runTabChanges();
    });
}

void WindowManager::postSessionContinuations()
{
    if (sessionContinuationsPosted_)
        return;
    sessionContinuationsPosted_ = true;
    QTimer::singleShot(0, this, [this] {
        sessionContinuationsPosted_ = false;
        if (isClosing())
            return;
        const QPointer<WindowManager> self(this);
        if (std::exchange(stagedSessionPaused_, false))
            postStagedStep();
        openLateWaiting();
        if (!self || isClosing())
            return;
        if (std::exchange(reopenSessionPaused_, false))
            serveReopen();
        if (!self || isClosing() || sessionOpens_.isEmpty())
            return;
        // One accepted batch per turn. Callbacks may start another session request.
        continueOpenPaths(sessionOpens_.takeFirst());
        if (self && !sessionOpens_.isEmpty())
            postSessionContinuations();
    });
}

void WindowManager::runTabChanges()
{
    // A change can close a window.
    const QList<QPointer<MainWindow>> windows(windows_.cbegin(), windows_.cend());
    for (const QPointer<MainWindow> &w : windows)
        if (w)
            w->runTabChanges();
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
    connect(instanceServer_.get(), &ipc::SingleInstanceServer::opensReceived, this,
            &WindowManager::takeInstanceOpens);
    // Launches a primary acknowledged and did not open before it quit go first. This copy
    // holds its own lock now, as its own carry file requires. A primary that ran in another
    // folder left its launches there. Those files are taken only while no live process
    // holds that folder's lock.
    for (const QString &file : instanceServer_->carriedFiles()) {
        const QList<ipc::Message> carried = relaunch::takeCarried(file);
        instanceOpens_ += carried;
        for (qsizetype i = 0; i < carried.size(); ++i)
            QMetaObject::invokeMethod(this, &WindowManager::openNextInstanceOpen,
                                      Qt::QueuedConnection);
    }
    // Launches acknowledged during startup wait in the server. They open once the
    // event loop runs, after the first window and the startup batches are queued.
    QMetaObject::invokeMethod(this, &WindowManager::takeInstanceOpens, Qt::QueuedConnection);
}

void WindowManager::takeInstanceOpens()
{
    if (!instanceServer_)
        return;
    // The server thread has acknowledged these already. Each open runs on a fresh
    // event-loop turn, as earlier ones can run nested password or error dialogs.
    const QList<ipc::Message> opens = instanceServer_->takeOpens();
    instanceOpens_ += opens;
    for (qsizetype i = 0; i < opens.size(); ++i)
        QMetaObject::invokeMethod(this, &WindowManager::openNextInstanceOpen,
                                  Qt::QueuedConnection);
}

void WindowManager::openNextInstanceOpen()
{
    // A quit, or an update that asks about unsaved edits, holds them, as its windows
    // would close them again. If it becomes final, endHandoffs hands them to the next
    // copy, and if it is cancelled, resumeInstanceOpens opens them here.
    if (shuttingDown_ || sessionFinal_ || quitting_ || holdingOpens_
        || instanceOpens_.isEmpty())
        return;
    // Empty paths means "bring the app up" - openPaths surfaces a window; otherwise
    // files open per behaviour.
    const ipc::Message open = instanceOpens_.takeFirst();
    openPaths(open.paths, open.behavior);
}

void WindowManager::resumeInstanceOpens()
{
    for (qsizetype i = 0; i < instanceOpens_.size(); ++i)
        QMetaObject::invokeMethod(this, &WindowManager::openNextInstanceOpen,
                                  Qt::QueuedConnection);
}

void WindowManager::endHandoffs()
{
    if (!instanceServer_)
        return;
    instanceServer_->stopListening();
    // No launch gets an acknowledgement from now on. The launches acknowledged before
    // and not opened yet are this process's to keep, and the next copy opens them.
    const QList<ipc::Message> left =
        std::exchange(instanceOpens_, {}) + instanceServer_->takeOpens();
    for (const ipc::Message &open : left)
        relaunch::carry(open);
}

void WindowManager::writeStore(const QString &file, const QByteArray &data) const
{
    if (ownsStores_)
        appfiles::write(file, data);
}

void WindowManager::recordOpen(const QString &canonicalPath, bool restoreViewState, int pageCount)
{
    if (canonicalPath.isEmpty())
        return;
    // The mount recorded for the file, until the check below finds the file and
    // the mount it is on now. A file that is not there, such as a crash recovery
    // tab opened lazily or a tab focused again after its share went away, keeps
    // it, since recording then would store the file system below the empty folder.
    const QString mount = recordedMount(canonicalPath);
    // The session keeps it too, for when Recent no longer lists the file.
    session_.setMount(canonicalPath, mount);
    if (recent_.add(canonicalPath, QDateTime::currentMSecsSinceEpoch(), pageCount, mount)) {
        writeStore(recentFile_, recent_.toJson());
        emit recentListChanged(recent_.entries());
    }
    mountWanted_.insert(FileProbe::keyOf({canonicalPath, mount}));
    probe_.check({{canonicalPath, mount}});
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
        writeStore(viewStateFile_, viewState_.toJson());
}

void WindowManager::removeRecent(const QString &canonicalPath)
{
    if (canonicalPath.isEmpty())
        return;
    const bool removedRecent = recent_.remove(canonicalPath);
    const bool removedState = viewState_.remove(canonicalPath); // drop its resume state too
    if (removedRecent) {
        keepFavouriteViewStates();
        writeStore(recentFile_, recent_.toJson());
        emit recentListChanged(recent_.entries());
    }
    if (removedState)
        writeStore(viewStateFile_, viewState_.toJson());
}

void WindowManager::clearMissingRecent(const QStringList &paths)
{
    // Checked again, so a file back in the meantime stays.
    QList<FileCheck> files;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        const FileCheck file{path, recent_.recordedMount(path)};
        clearWanted_.insert(FileProbe::keyOf(file));
        files.append(file);
    }
    probe_.check(files);
}

void WindowManager::dropRecent(const QString &path)
{
    if (recentDue_.isEmpty())
        QTimer::singleShot(0, this, &WindowManager::dropFoundRecent);
    recentDue_.append(path);
}

void WindowManager::dropFoundRecent()
{
    const QStringList removed = recent_.removeEntries(std::exchange(recentDue_, {}));
    if (removed.isEmpty())
        return;

    keepFavouriteViewStates();
    writeStore(recentFile_, recent_.toJson());
    emit recentListChanged(recent_.entries());

    bool removedState = false;
    for (const QString &path : removed)
        removedState = viewState_.remove(path) || removedState;
    if (removedState)
        writeStore(viewStateFile_, viewState_.toJson());
}

void WindowManager::recordMount(const QString &path, const QString &mount)
{
    mountsDue_.insert(path, mount);
    // The probe answers one file at a time, so saving on the next turn of the
    // event loop would save, and rebuild every Recent page, nearly once per answer.
    // The timer is not restarted, so a slow pass still saves every kMountSaveMs.
    if (!mountSaveTimer_->isActive())
        mountSaveTimer_->start();
}

void WindowManager::recordFoundMounts()
{
    mountSaveTimer_->stop();
    if (!recent_.setMissingMounts(std::exchange(mountsDue_, {})))
        return;
    writeStore(recentFile_, recent_.toJson());
    // The panels ask for checks with the mount they were given.
    emit recentListChanged(recent_.entries());
}

void WindowManager::setFavorite(const QString &canonicalPath, bool favorite)
{
    if (canonicalPath.isEmpty())
        return;
    if (recent_.setFavorite(canonicalPath, favorite)) {
        keepFavouriteViewStates();
        writeStore(recentFile_, recent_.toJson());
        emit recentListChanged(recent_.entries());
    }
}

void WindowManager::keepFavouriteViewStates()
{
    QStringList favourites;
    for (const RecentEntry &e : recent_.entries())
        if (e.favorite)
            favourites.append(e.path);
    viewState_.setKept(favourites);
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
        writeStore(recentFile_, recent_.toJson());
        emit recentListChanged(recent_.entries());
    }
    if (viewState_.count() != statesBefore)
        writeStore(viewStateFile_, viewState_.toJson());
    keepMissingRecent_ = keepMissing;
    emit recentVisibleCountChanged(visibleCount);
    pruneMissingRecent();
}

void WindowManager::pruneMissingRecent()
{
    if (keepMissingRecent_ || recent_.count() == 0)
        return;
    // Each path with the mount recorded for it.
    QList<FileCheck> files;
    files.reserve(recent_.count());
    for (const RecentEntry &e : recent_.entries()) {
        const FileCheck file{e.path, e.mount};
        pruneWanted_.insert(FileProbe::keyOf(file));
        files.append(file);
    }
    probe_.check(files);
}

void WindowManager::onFileChecked(const FileCheck &file, const FileFacts &facts)
{
    const QString key = FileProbe::keyOf(file);
    const QString &path = file.path;
    if (mountWanted_.remove(key) && facts.exists) {
        if (recent_.setMount(path, facts.mount)) {
            writeStore(recentFile_, recent_.toJson());
            // The panels ask for checks with the mount they were given.
            emit recentListChanged(recent_.entries());
        }
        if (session_.mount(path) != facts.mount) {
            session_.setMount(path, facts.mount);
            updateSession();
        }
    } else if (facts.exists && file.mount.isEmpty() && !facts.mount.isEmpty()) {
        // An entry an older version wrote has no mount. Any check that finds its
        // file there, such as for the Recent page, records the one it is on.
        recordMount(path, facts.mount);
    }
    if (pruneWanted_.remove(key)) {
        if (!keepMissingRecent_ && facts.removed())
            dropRecent(path);
        // The pass has its last answer, so the mounts it found need not wait.
        if (pruneWanted_.isEmpty() && !mountsDue_.isEmpty())
            recordFoundMounts();
    }
    if (clearWanted_.remove(key) && !facts.exists)
        dropRecent(path);
    if (restoreLate_.remove(key))
        restoreLate(path, facts);
    if (reopenCheck_ && FileProbe::keyOf(*reopenCheck_) == key)
        serveReopen();
    if (restorePending_ && restoreWaiting_.remove(key) && restoreWaiting_.isEmpty())
        finishRestore();
}

void WindowManager::closeRemovedTabs()
{
    if (shuttingDown_ || quitting_)
        return;
    if (tabScanRunning_) {
        tabScanPending_ = true;
        return;
    }
    // Each unloaded tab's file, once per canonical path, with the mount recorded
    // for it. Tabs with unsaved edits reload from their recovery copy.
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
            if (tab->freshOpen() || tab->hasUnsavedEdits() || listed.contains(tab->canonicalPath()))
                continue;
            listed.insert(tab->canonicalPath());
            files.append({tab->path(), tab->canonicalPath(), recordedMount(tab->canonicalPath())});
        }
    }
    if (files.isEmpty())
        return;
    tabScanRunning_ = true;
    const QPointer<QObject> self(this);
    background::start([self, cancel = scanCancel_, files] {
        for (const File &f : files) {
            if (*cancel)
                return;
            // The rule session restore uses (SessionStore::prepareRestore): gone only
            // from a folder that is still there. A missing folder can also be a
            // volume away, such as another USB stick under the same drive letter.
            const QFileInfo file(f.canonicalPath);
            if (!QFileInfo::exists(f.path) && file.absoluteDir().exists()
                && RecentStore::isRemovedFromDisk(f.canonicalPath, f.mount)) {
                // Close it now. Waiting for the other files, which can stall on a
                // network path, would leave time for this one to come back.
                background::post(self, [self, path = f.canonicalPath] {
                    static_cast<WindowManager *>(self.data())->closeFoundRemovedTabs({path});
                });
            }
        }
        background::post(self, [self] {
            auto *manager = static_cast<WindowManager *>(self.data());
            manager->tabScanRunning_ = false;
            if (std::exchange(manager->tabScanPending_, false))
                manager->closeRemovedTabs();
        });
    });
}

void WindowManager::closeFoundRemovedTabs(const QStringList &canonicalPaths)
{
    if (shuttingDown_ || quitting_)
        return;
    const QPointer<WindowManager> self(this);
    for (MainWindow *w : std::as_const(windows_)) { // closing a tab keeps its window
        for (const QString &path : canonicalPaths) {
            if (w->closeRemovedTabs(path))
                continue;
            // The window has to keep the tab for now. Check again once its
            // earlier changes have run and it can change its tabs, rather than
            // act on this answer, as the file may be back by then.
            w->whenTabsCanChange([self] {
                if (self)
                    self->closeRemovedTabs();
            });
            break;
        }
    }
}

void WindowManager::restoreSession(std::function<void(const QStringList &)> done)
{
    session_.load(sessionFile_);
    // Each file is judged by the file system it was on when opened, as the session
    // recorded it. A session saved before sessions recorded mounts has them in Recent.
    const QList<FileCheck> files =
        session_.filesToCheck(QDate::currentDate(), [this](const QString &path) {
            return recent_.recordedMount(path);
        });
    restoreDone_ = std::move(done);
    restorePending_ = true;
    restoreWindow_ = activeOrNewWindow();
    restoreOrder_.clear();
    restoreChecks_.clear();
    restoreWaiting_.clear();
    for (const FileCheck &file : files) {
        restoreOrder_.append(file.path);
        restoreChecks_.insert(normalizePathKey(file.path), file);
        restoreWaiting_.insert(FileProbe::keyOf(file));
    }
    if (files.isEmpty()) {
        QTimer::singleShot(0, this, &WindowManager::finishRestore);
        return;
    }
    // Ahead of the Recent page's checks: the window waits for these.
    probe_.check(files, 0, /*first=*/true);
    QTimer::singleShot(kRestoreWaitMs, this, &WindowManager::finishRestore);
}

void WindowManager::finishRestore()
{
    if (!restorePending_)
        return;
    restorePending_ = false;
    // Documents opened meanwhile, such as command-line files, keep their mounts:
    // prepareRestore forgets those of documents it does not restore or keep.
    QHash<QString, QString> openMounts;
    for (const QString &path : openTabPaths())
        openMounts.insert(path, session_.mount(path));
    const QStringList restore =
        session_.prepareRestore(QDate::currentDate(), [this](const QString &path) {
            const FileCheck file = restoreChecks_.value(normalizePathKey(path), {path, {}});
            return restoreWaiting_.contains(FileProbe::keyOf(file)) ? std::nullopt
                                                                     : probe_.facts(file);
        });
    for (auto it = openMounts.cbegin(); it != openMounts.cend(); ++it)
        if (!it.value().isEmpty())
            session_.setMount(it.key(), it.value());
    // Kept in the session for want of an answer. One that answers still opens.
    restoreLate_ = std::exchange(restoreWaiting_, {});
    if (const std::function<void(const QStringList &)> done = std::exchange(restoreDone_, {}))
        done(restore);
}

void WindowManager::restoreLate(const QString &path, const FileFacts &facts)
{
    if (!facts.exists) {
        // Removed while its volume was away, so there is nothing to restore. Away
        // still, it stays kept for a later start.
        if (facts.goneFromFolder()) {
            session_.forgetUnavailable(path);
            updateSession();
        }
        return;
    }
    // Restored tabs are placed against the live bar, so a late one waits until the
    // session's own tabs are in.
    if (sessionend::active() || sessionHeld_ || !stagedQueue_.isEmpty()) {
        if (!lateWaiting_.contains(path))
            lateWaiting_.append(path);
        return;
    }
    MainWindow *w = restoreWindow_.data();
    if (shuttingDown_ || quitting_ || !w || !windows_.contains(w) || w->isInTray()
        || isOpenAnywhere(path))
        return;
    const int at = insertIndexForSaved(w->tabPaths(), restoreOrder_, restoreOrder_.indexOf(path));
    w->openFile(path, /*allowDuplicate=*/false, at, /*makeCurrent=*/false, {}, /*lazy=*/true);
}

void WindowManager::holdSession(bool hold)
{
    sessionHeld_ = hold;
    if (!hold && stagedQueue_.isEmpty()) {
        // Nothing else would save it, so record what the restore decided and the
        // documents opened while it was held. A batch still opening saves the
        // complete set with its last document.
        updateSession();
        openLateWaiting();
    }
}

void WindowManager::openLateWaiting()
{
    for (const QString &path : std::exchange(lateWaiting_, {})) {
        FileFacts there;
        there.exists = true;
        restoreLate(path, there);
    }
}

void WindowManager::updateSession()
{
    // Do not persist an incomplete restore: pending entries would disappear from the saved
    // session. That holds while the restore waits for its checks, and while its batch opens:
    // the final entry leaves the queue before opening, so its following save sees the
    // complete set. Nor a tab halfway between windows, which would lose its recorded mount.
    if (sessionHeld_ || restorePending_ || !stagedQueue_.isEmpty() || movingTab_)
        return;
    // Quitting has saved the final session, and windows closing since then take
    // their tabs along without a close. With no window left there is nothing to
    // record: a late check answer must not save an empty or partial session.
    if (sessionFinal_ || shuttingDown_ || windows_.isEmpty())
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
    writeStore(sessionFile_, session_.toJson());
}

void WindowManager::openStaged(const QList<StagedOpen> &batch, const QStringList &savedOrder,
                               std::function<void()> onDone, MainWindow *into)
{
    stagedQueue_ = batch;
    stagedOrder_ = savedOrder;
    stagedDone_ = std::move(onDone);
    // Pin the window the batch belongs to. Re-resolving it per turn would follow
    // active_, and the whole point of staging is that the UI is live while the
    // batch drains - so Ctrl+N, "Open in new window", a detached tab or an
    // Explorer open with the "new window" behaviour would all make the fresh
    // window active and divert the rest of the session into it.
    stagedWindow_ = into && windows_.contains(into) ? into : activeOrNewWindow();
    postStagedStep();
}

void WindowManager::postStagedStep()
{
    if (stagedStepPosted_)
        return;
    stagedStepPosted_ = true;
    QTimer::singleShot(0, this, [this] {
        stagedStepPosted_ = false;
        stagedStep();
    });
}

void WindowManager::stagedStep()
{
    // Keep both documents and completion callbacks until cancellation. Successful
    // session end leaves them untouched for the next process's startup restore.
    if (sessionend::active()) {
        stagedSessionPaused_ = true;
        return;
    }
    stagedSessionPaused_ = false;
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
        const QPointer<WindowManager> self(this);
        if (done)
            done();
        if (self && !isClosing() && stagedQueue_.isEmpty() && !sessionHeld_)
            openLateWaiting();
        return;
    }

    const StagedOpen o = stagedQueue_.takeFirst();
    MainWindow *w = stagedWindow_;
    // The tab index comes from the live tab bar, not from the plan: an open that
    // focuses an existing tab (for example a document also named on the command
    // line) then
    // leaves the remaining documents in their saved order instead of shifting
    // every one of them.
    const int atIndex = insertIndexForSaved(w->tabPaths(), stagedOrder_, o.savedIndex);
    w->openFile(o.path, o.allowDuplicate, atIndex, o.makeCurrent, {}, o.lazy);

    // One document per event-loop turn: Qt gets to paint and handle input between
    // documents, so the window stays live through a slow cold-cache batch.
    postStagedStep();
}

void WindowManager::rememberClosedTab(const QString &path, const QString &canonicalPath,
                                      const QStringList &siblings, int index)
{
    if (path.isEmpty())
        return;
    ClosedTab t;
    t.path = path;
    t.canonicalPath = canonicalPath.isEmpty() ? path : canonicalPath;
    t.siblings = siblings;
    t.index = qMax(0, index);
    closedTabs_.push(t);
}

void WindowManager::reopenClosedTab(MainWindow *into, std::function<void(Reopen)> done)
{
    reopenPresses_.append({into, std::move(done)});
    if (reopenPresses_.size() == 1)
        serveReopen();
}

void WindowManager::serveReopen()
{
    const QPointer<WindowManager> self(this);
    while (self && !reopenPresses_.isEmpty()) {
        if (sessionend::active()) {
            // The old timeout must not consume the press after cancellation.
            // Restart its probe then, preserving both history and the callback.
            reopenCheck_.reset();
            ++reopenAttempt_;
            reopenSessionPaused_ = true;
            return;
        }
        reopenSessionPaused_ = false;
        if (shuttingDown_ || quitting_) {
            reopenCheck_.reset();
            for (const ReopenPress &press : std::exchange(reopenPresses_, {}))
                if (press.done)
                    press.done(Reopen::Failed);
            return;
        }
        // Open again (from Recent, from Explorer, or by an earlier press): there is
        // nothing left to restore.
        closedTabs_.prune([this](const ClosedTab &t) { return !isOpenAnywhere(t.canonicalPath); });
        if (closedTabs_.isEmpty()) {
            reopenCheck_.reset();
            for (const ReopenPress &press : std::exchange(reopenPresses_, {}))
                if (press.done)
                    press.done(Reopen::NothingToReopen);
            return;
        }
        // The file itself, not a link to it, so its own volume answers.
        const ClosedTab &top = closedTabs_.top();
        const FileCheck check{top.canonicalPath, recordedMount(top.canonicalPath)};
        if (!reopenCheck_ || FileProbe::keyOf(*reopenCheck_) != FileProbe::keyOf(check)) {
            // Ask afresh for every press, since the file may have gone or come back.
            reopenCheck_ = check;
            const quint64 attempt = ++reopenAttempt_;
            probe_.check({check}, 0, /*first=*/true);
            QTimer::singleShot(kReopenWaitMs, this, [this, attempt] {
                if (attempt == reopenAttempt_ && reopenCheck_) {
                    reopenTimedOut_ = attempt;
                    serveReopen();
                }
            });
            return; // onFileChecked or the timer calls back
        }
        const bool timedOut = reopenTimedOut_ == reopenAttempt_;
        reopenCheck_.reset();
        ++reopenAttempt_;
        if (timedOut) {
            // Its share has not answered yet. The entry stays for the next press,
            // when the answer has usually arrived, and older entries are not tried.
            const ReopenPress press = reopenPresses_.takeFirst();
            if (press.done)
                press.done(Reopen::Failed);
            continue;
        }
        const ClosedTab t = closedTabs_.pop();
        // Gone from disk: nothing to restore it from.
        const std::optional<FileFacts> facts = probe_.facts(check);
        if (!facts || !facts->exists)
            continue;

        // The press stays first while it opens, so presses made meanwhile wait.
        const ReopenPress press = reopenPresses_.first();
        // Never into a window that went to the tray or closed meanwhile, and never
        // into a new one: the press belonged to a window on screen.
        MainWindow *w = press.into && windows_.contains(press.into) && !press.into->isInTray()
                            ? press.into.data()
                            : nullptr;
        if (!w && active_ && windows_.contains(active_) && !active_->isInTray())
            w = active_;
        if (!w) {
            closedTabs_.push(t);
            reopenPresses_.removeFirst();
            if (press.done)
                press.done(Reopen::Failed);
            continue;
        }
        // Its place is in the bar as it stands NOW, right after the last tab in the
        // bar that sat before it when it was closed, wherever that tab sits now, or
        // first when none is back. Re-derived rather than replayed (see ClosedTab),
        // which is what makes undoing a whole window's worth of closes rebuild the
        // original order however many presses in, and in whatever order the entries
        // come back.
        const int at = insertIndexForSaved(w->tabPaths(), t.siblings, t.index);
        const bool ok = w->openFile(t.path, /*allowDuplicate=*/false, at,
                                    /*makeCurrent=*/true);
        if (ok)
            surfaceWindow(w);
        // Consumed either way. A document that cannot be opened - a cancelled
        // password prompt, a file that went corrupt - must not be handed back on
        // every press from here on. The press was still spent on a real entry,
        // so this is not the same as an empty history.
        reopenPresses_.removeFirst();
        if (press.done)
            press.done(ok ? Reopen::Reopened : Reopen::Failed);
    }
}

bool WindowManager::isOpenAnywhere(const QString &canonicalPath, const TabPage *except) const
{
    if (canonicalPath.isEmpty())
        return false;
    const QString key = normalizePathKey(canonicalPath);
    for (MainWindow *w : windows_)
        for (const TabPage *tab : w->findChildren<TabPage *>())
            if (tab != except && normalizePathKey(tab->canonicalPath()) == key)
                return true;
    return false;
}

bool WindowManager::hasLoadingDocuments() const
{
    return std::any_of(windows_.cbegin(), windows_.cend(),
                       [](const MainWindow *window) { return window->hasLoadingDocuments(); });
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
#ifdef Q_OS_WIN
    if (qApp)
        qApp->removeNativeEventFilter(this);
#endif
    shuttingDown_ = true;
    endHandoffs();
    if (activityTimer_)
        activityTimer_->stop();
    if (tray_)
        tray_->hide();
    *scanCancel_ = true; // nothing waits for a running scan; let it stop early
    // Stop the render workers, then free any windows still alive. Nothing waits for
    // either: their Documents close on background threads, which wait for any work
    // in them. Each holder of a context cloned from the engine's keeps that context
    // alive (see RenderEngine::baseContext()), so engine_ can go after this body.
    if (engine_)
        engine_->shutdown();
    const QList<MainWindow *> copy = windows_;
    windows_.clear();
    for (MainWindow *w : copy)
        delete w;
    // The next copy reads the settings, Recent and the session once it gets the
    // single-instance lock, which instanceServer_ holds until after this body. A
    // tab's recovery copy is removed once its document has closed on a background
    // thread, so those closes come first, and the updater's state, as a pending
    // update it put back, comes last, all within the same time.
    // Windows already received its bounded answer. A stalled close, profile or
    // updater must not add the ordinary five-second exit budget to logoff.
    if (sessionend::active())
        return;
    // Mounts found within the last kMountSaveMs are saved with the rest. The
    // windows are gone, so nothing is announced.
    if (recent_.setMissingMounts(std::exchange(mountsDue_, {})))
        writeStore(recentFile_, recent_.toJson());
    QElapsedTimer exitWait;
    exitWait.start();
    const auto left = [&exitWait] {
        return std::max(0, appfiles::kExitWaitMs - static_cast<int>(exitWait.elapsed()));
    };
    Document::waitForCloses(appfiles::kExitWaitMs);
    appfiles::flush(left());
    Updater::waitForDisk(left());
    // The launches carried go to the next primary while this process still holds the
    // lock, which instanceServer_ lets go of after this body. If they cannot be written,
    // relaunch keeps them for the next copy's command line.
    if (instanceServer_ && !relaunch::writeCarried(instanceServer_->carriedFile()))
        std::fprintf(stderr, "MervinPDF: could not write the files opened during the quit "
                             "to the carry file\n");
    // Writes still in flight could land after the next copy has read the files, or over
    // its own writes. The lock then stays until this process has ended, and the next copy
    // waits for that, as it waits during this exit wait.
    if (instanceServer_ && !appfiles::flush(0)) {
        std::fprintf(stderr, "MervinPDF: keeping the single-instance lock until this "
                             "process has ended\n");
        instanceServer_->keepLockUntilExit();
    }
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
        // Last window gone: stop the render workers, then quit the process. This
        // window's Documents, deleted right after this destroyed() handler, close
        // on background threads.
        endHandoffs();
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
    const QString key = normalizePathKey(canonicalPath);
    for (MainWindow *w : std::as_const(windows_)) {
        if (w->focusTabIfOpen(canonicalPath)) {
            surfaceWindow(w);
            active_ = w;
            // Re-opening an already-open file (e.g. from Explorer) still bumps
            // its recency, but there is no view state to restore - it's live.
            for (const TabPage *tab : w->findChildren<TabPage *>())
                if (!tab->freshOpen() && normalizePathKey(tab->canonicalPath()) == key) {
                    recordOpen(tab->canonicalPath(), /*restoreViewState=*/false);
                    break;
                }
            return true;
        }
    }
    return false;
}

void WindowManager::openPaths(const QStringList &paths, const QString &behavior)
{
    continueOpenPaths({paths, behavior, {}});
}

void WindowManager::continueOpenPaths(SessionOpen batch)
{
    if (shuttingDown_ || sessionFinal_)
        return;
    if (sessionend::active()) {
        sessionOpens_.append(std::move(batch));
        return;
    }
    if (batch.paths.isEmpty()) {
        // A launch without files brings back every window hidden in the tray.
        restoreFromTray();
        return;
    }

    const bool newWin = (batch.behavior == QLatin1String("new-window"));
    const QPointer<WindowManager> self(this);
    while (!batch.paths.isEmpty()) {
        if (isClosing()) {
            if (sessionend::active())
                sessionOpens_.prepend(std::move(batch));
            return;
        }
        const QString path = batch.paths.first();
        const QString canon = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const bool focused = focusExistingTab(canon);
        if (!self)
            return;
        if (focused) {
            batch.paths.removeFirst();
            continue;
        }
        if (isClosing())
            continue; // retain this path before creating or selecting a target

        QPointer<MainWindow> target;
        if (newWin) {
            if (!batch.window || !windows_.contains(batch.window)) {
                // Reuse a document-less window before creating another one.
                batch.window = emptyWindow();
                if (!batch.window)
                    batch.window = createWindow();
            }
            target = batch.window;
        } else {
            target = activeOrNewWindow();
        }
        if (!self)
            return;
        if (isClosing())
            continue;
        if (target)
            target->openFile(path);
        if (!self)
            return;
        batch.paths.removeFirst();
    }

    MainWindow *focusW = batch.window ? batch.window.data() : active_;
    if (!isClosing() && focusW)
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
    if (!w->openFile(path, /*allowDuplicate=*/true, /*atIndex=*/-1, /*makeCurrent=*/true,
                     password, false, state, /*closeOnFailure=*/true)) {
        // A closing process refused the new shell. Keep no empty window.
        // Background failures follow the policy carried by the duplicate tab.
        if (w->tabCount() == 0)
            w->closeForQuit();
        return;
    }
    w->move(globalPos);                           // offset from the source window
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

void WindowManager::closeAllForQuit()
{
    CloseFlow::requestQuit(this);
}

void WindowManager::restart()
{
    CloseFlow::requestQuit(this, true);
}

bool WindowManager::tryCloseAllForUpdate()
{
    return CloseFlow::tryCloseForUpdate(this);
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
    if (shuttingDown_ || quitting_ || checkingActivity_ || sessionend::active())
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
        // Unloaded documents close on background threads, and what MuPDF cached
        // for one goes at the first trim after it has closed.
        cacheTrimmed_ = Document::closing() == 0;
    }
}

void WindowManager::notifySuspensionFailure(const QString &message)
{
    //: Title of a tray notification: an inactive document could not be unloaded from memory.
    notifyInTray(tr("Document remains loaded"), message);
}

void WindowManager::notifyInTray(const QString &title, const QString &message)
{
    if (tray_ && tray_->isVisible())
        tray_->showMessage(title, message, QSystemTrayIcon::Warning);
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
