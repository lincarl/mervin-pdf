#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLockFile>
#include <QSettings>
#include <QSet>
#include <QTimer>

#include "app/Relaunch.h"
#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "dialogs/FirstRunDialog.h"
#include "i18n/UiLanguage.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "mervin_version.h"
#include "session/StartupPlan.h"
#include "ui/TabPage.h"
#include "ui/Theme.h"
#include "update/Updater.h"

#ifdef Q_OS_WIN
#  include "platform/PlatformIntegration.h"
#endif

#include <cstdio>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <cstdio>
#endif

using mervin::ipc::Message;
using mervin::ipc::MessageDecoder;
using mervin::ipc::SingleInstanceServer;

namespace {

// Non-flags are paths; unknown flags are ignored. --profile <dir> (or =<dir>) redirects
// persistent state and isolates single-instance/session handling. --quit-after-startup exits
// once startup completes, for process timing. --language <id> (or =<id>) shows the UI in that
// language for this run only, without the first-run window.
struct CliOptions
{
    QString profileDir;
    bool profileError = false; // --profile present but its directory missing
    bool quitAfterStartup = false;
    QString language;
    QStringList paths;
};

// The value of `--name <value>` or `--name=<value>` at args[i], advancing i past a separate
// value. Empty when the flag has no value.
QString flagValue(const QStringList &args, int &i, const QString &name)
{
    const QString &a = args.at(i);
    if (a.startsWith(name + QLatin1Char('=')))
        return a.mid(name.size() + 1);
    if (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1String("--")))
        return args.at(++i);
    return {};
}

CliOptions parseCli(const QStringList &args)
{
    CliOptions opt;
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("--profile") || a.startsWith(QLatin1String("--profile="))) {
            const QString value = flagValue(args, i, QStringLiteral("--profile"));
            // A malformed --profile must hard-fail (see main): silently
            // proceeding would read/write the REAL user state - the exact
            // thing the flag exists to prevent.
            if (value.isEmpty())
                opt.profileError = true;
            else
                opt.profileDir = value;
        } else if (a == QLatin1String("--quit-after-startup")) {
            opt.quitAfterStartup = true;
        } else if (a == QLatin1String("--language") || a.startsWith(QLatin1String("--language="))) {
            opt.language = flagValue(args, i, QStringLiteral("--language"));
        } else if (a.startsWith(QLatin1String("--"))) {
            continue;
        } else {
            opt.paths.append(a);
        }
    }
    return opt;
}

QStringList toAbsolute(const QStringList &paths)
{
    QStringList out;
    out.reserve(paths.size());
    for (const QString &p : paths)
        out.append(QFileInfo(p).absoluteFilePath());
    return out;
}

// Secondary launch: hand the files to the already-running primary instance and
// confirm receipt via its ack (a bare flush only proves our buffer drained, and
// on Windows can false-negative when the write already flushed). Returns true
// only once acked; false means the primary is unreachable and the caller should
// try to become the primary itself.
bool handOffToPrimary(const QStringList &paths, const QString &behavior)
{
    QLocalSocket sock;
    bool connected = false;
    // The primary holds the lock but may have only just acquired it and not be
    // accepting yet - retry briefly (~1s ceiling).
    for (int i = 0; i < 20 && !connected; ++i) {
        sock.connectToServer(mervin::ipc::hostPipeName());
        connected = sock.waitForConnected(50);
        if (!connected)
            sock.abort();
    }
    if (!connected)
        return false;

#ifdef Q_OS_WIN
    // Pass this launch's foreground rights to the primary before IPC delivery, allowing it to
    // restore/activate the target window. QLockFile supplies the primary PID.
    {
        qint64 pid = 0;
        QString host, appName;
        QLockFile lock(QDir(QDir::tempPath())
                           .filePath(mervin::ipc::hostPipeName() + QStringLiteral(".lock")));
        if (lock.getLockInfo(&pid, &host, &appName) && pid > 0)
            AllowSetForegroundWindow(static_cast<DWORD>(pid));
        else
            AllowSetForegroundWindow(ASFW_ANY);
    }
#endif

    sock.write(Message::open(paths, behavior).encode());
    sock.flush();

    MessageDecoder decoder;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1000) {
        const int remaining = static_cast<int>(1000 - timer.elapsed());
        if (remaining <= 0 || !sock.waitForReadyRead(remaining))
            break;
        bool overflow = false;
        const auto msgs = decoder.feed(sock.readAll(), &overflow);
        for (const Message &m : msgs)
            if (m.cmd == Message::Cmd::Ack)
                return true;
        if (overflow)
            break;
    }
    return false; // no ack - treat as unreachable and try to become primary
}

// Install the UI language before any window exists: windows set their text once, when they
// are built. The first start, before a language is stored, shows the first-run window, which
// on Windows also offers to make Mervin the default PDF viewer; returns whether the user asked
// for that. Launches that arrive while the window is open are acknowledged and their files
// added to `paths`, so they open with the first window instead of starting a second process.
bool startUiLanguage(const CliOptions &cli, SingleInstanceServer *server, QStringList &paths)
{
    namespace i18n = mervin::i18n;
    const QStringList available = i18n::availableLanguages();
    if (!cli.language.isEmpty()) {
        if (i18n::normalized(cli.language, available).isEmpty())
            std::fprintf(stderr, "MervinPDF: unknown --language '%s' (available: %s)\n",
                         qPrintable(cli.language), qPrintable(available.join(QStringLiteral(", "))));
        i18n::apply(cli.language);
        i18n::setOneRunOverride(true);
        return false;
    }
    mervin::Settings settings = mervin::Settings::load();
    // Timing runs never stop for input, and a build without translations has nothing to offer.
    if (!settings.uiLanguage.isEmpty() || cli.quitAfterStartup || available.size() < 2) {
        i18n::apply(settings.uiLanguage);
        return false;
    }

    i18n::apply(i18n::suggestedLanguage());
    // WindowManager applies these too, but this window comes first.
    mervin::WindowManager::applyColorSchemeToQt(settings.colorScheme);
    mervin::Theme::applyApp();

    bool offerDefaultApp = false;
#ifdef Q_OS_WIN
    // A --profile run (dev and test) must not touch the machine's file-type registration.
    const bool realProfile = mervin::ConfigPaths::overrideDir().isEmpty();
    offerDefaultApp = realProfile && !settings.promptedSetDefaultApp
        && !mervin::PlatformIntegration::isDefaultPdfHandler();
#endif
    mervin::FirstRunDialog dialog(offerDefaultApp);
    QMetaObject::Connection gate;
    if (server) {
        gate = QObject::connect(
            server, &SingleInstanceServer::messageReceived, &dialog,
            [&paths, &dialog](QLocalSocket *socket, const Message &msg) {
                if (msg.cmd != Message::Cmd::Open)
                    return;
                SingleInstanceServer::send(socket, Message::ack(QStringLiteral("open")));
                paths.append(msg.paths);
                dialog.raise();
                dialog.activateWindow();
            });
    }
    // Closing the window keeps the language it shows.
    const bool accepted = dialog.exec() == QDialog::Accepted;
    QObject::disconnect(gate);

    settings.uiLanguage = dialog.language();
#ifdef Q_OS_WIN
    if (realProfile)
        settings.promptedSetDefaultApp = true; // asked once, whatever the answer
#endif
    settings.save();
    return accepted && dialog.makeDefaultApp();
}

#ifdef Q_OS_WIN
// GUI-subsystem builds create no console. Attach to an existing parent console and reopen
// streams for terminal diagnostics; Explorer launches have no parent and leave streams
// untouched.
void attachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
}
#endif

// Primary instance: own the renderer + windows, accept opens from later
// launches (if given the instance server), restore the previous session, and
// open the initial files.
int runUi(QApplication &app, const CliOptions &cli, const QStringList &cliPaths,
          std::unique_ptr<SingleInstanceServer> server)
{
    app.setQuitOnLastWindowClosed(false); // WindowManager drives process exit
    QStringList paths = cliPaths;
    const bool setDefaultApp = startUiLanguage(cli, server.get(), paths);
    mervin::WindowManager wm;
    if (server)
        wm.adoptInstanceServer(std::move(server));

    // Create the window BEFORE reading any document, so the event loop's first
    // turn paints it. Every open below is then staged onto later turns (see
    // WindowManager::openStaged): a cold-cache open is mostly disk reads and the
    // virus scanner's first pass on the file, and doing a whole session inline
    // used to freeze the process with nothing on screen at all.
    MainWindow *firstWindow = wm.createWindow();
    // Windows' Default Apps page opens over the first window.
    if (setDefaultApp)
        mervin::FirstRunDialog::setAsDefaultPdfApp(firstWindow);

    // Crash recovery / session restore (M11): reopen the documents that were
    // open last time (still-existing files only). On by default.
    QStringList restore;
    if (mervin::Settings::load().restoreSession) {
        for (const QString &p : wm.sessionPaths())
            if (QFileInfo::exists(p))
                restore.append(p);
    }

    // Recover unsaved work even if the original moved or ordinary session restore is off.
    // Passwords are deliberately absent from these manifests and are requested on selection.
    const QStringList recovery = mervin::TabPage::recoverablePaths();
    for (const QString &path : recovery)
        if (!restore.contains(path))
            restore.append(path);

    // Keep command-line/session duplicates in the restore list: planStartupOpens opens them
    // once while reserving their saved tab positions.
    QList<mervin::StagedOpen> batch =
        mervin::planStartupOpens(paths, restore, wm.sessionActivePath());
    for (auto &open : batch)
        if (recovery.contains(open.path))
            open.lazy = true;
    QSet<QString> recovered;
    for (const QString &path : recovery) {
        if (recovered.contains(path))
            batch.append({path, static_cast<int>(restore.indexOf(path)), false, true, true});
        recovered.insert(path);
    }

    // --quit-after-startup waits for all tab shells and the selected document load.
    // tst_perf_startup times this process as plain runtime, and
    // quitting at the first turn would silently stop measuring the documents.
    std::function<void()> onDone;
    if (cli.quitAfterStartup)
        onDone = [] { QCoreApplication::quit(); };
    wm.openStaged(batch, restore, onDone);

    // Start asynchronous update handling after first paint in the primary process; skip startup
    // timing runs. Settings changes cancel background work through WindowManager.
    mervin::Updater updater;
    updater.setCloseWindowsHandler([&wm] { return wm.closeAllForQuit(); });
    QObject::connect(&wm, &mervin::WindowManager::autoUpdateChanged, &updater,
                     &mervin::Updater::onAutoUpdateChanged);
    if (!cli.quitAfterStartup)
        QTimer::singleShot(2500, &updater, &mervin::Updater::onStartup);

    return app.exec();
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    attachParentConsole(); // keep qDebug visible when run from a terminal (see above)
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral(MERVIN_APP_NAME));
    QApplication::setOrganizationName(QStringLiteral(MERVIN_ORG_NAME));
    QApplication::setApplicationVersion(QStringLiteral(MERVIN_VERSION_STRING));
    QApplication::setWindowIcon(mervin::icons::applicationIcon());
#ifdef Q_OS_LINUX
    // Wayland shells find the installed launcher icon through this desktop ID.
    QApplication::setDesktopFileName(QStringLiteral("mervin-pdf"));
#endif

    const CliOptions cli = parseCli(QApplication::arguments());
    if (cli.profileError) {
        std::fprintf(stderr, "MervinPDF: --profile requires a directory "
                             "(--profile <dir> or --profile=<dir>)\n");
        return 2;
    }
    if (!cli.profileDir.isEmpty()) {
        // Redirect ALL persisted state before anything resolves a path or the
        // single-instance name. The updater's QSettings normally live in
        // the registry / org paths - point them into the profile too, so a
        // profile run is fully self-contained.
        mervin::ConfigPaths::setOverrideDir(cli.profileDir);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           mervin::ConfigPaths::configDir());
    }

    const QStringList paths = toAbsolute(cli.paths);
    const QString behavior = mervin::Settings::load().openBehavior;

    // A restart asked for while running (a new UI language) starts the new copy here,
    // after runUi has released the single-instance pipe.
    const auto finish = [](int code) {
        if (mervin::relaunch::requested() && !mervin::relaunch::start())
            std::fprintf(stderr, "MervinPDF: could not restart\n");
        return code;
    };

    // Single-instance: the first launch owns the pipe and becomes the primary UI
    // process; later launches hand their files to it and exit. A primary hidden in the
    // tray remains available for those opens. Explicit Quit ends that same process.
    auto server = std::make_unique<SingleInstanceServer>();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (server->start())
            return finish(runUi(app, cli, paths, std::move(server))); // we are the primary
        if (handOffToPrimary(paths, behavior))
            return 0; // delivered to the running instance
        // The primary released the pipe between our failed start and the connect
        // (it was shutting down) - loop once to try taking over ourselves.
    }

    // Could not coordinate (extremely unlikely). Open standalone so the user
    // still gets their file; this launch simply isn't the single-instance owner.
    return finish(runUi(app, cli, paths, nullptr));
}
