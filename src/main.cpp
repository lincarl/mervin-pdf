#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QLinearGradient>
#include <QLocalSocket>
#include <QLockFile>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>

#include "app/WindowManager.h"
#include "config/ConfigPaths.h"
#include "config/Settings.h"
#include "ipc/Message.h"
#include "ipc/PipeName.h"
#include "ipc/SingleInstanceServer.h"
#include "ui/ThemeTokens.h"
#include "mervin_version.h"
#include "session/StartupPlan.h"
#include "update/UpdateChecker.h"

#ifdef Q_OS_WIN
#  include "platform/PlatformIntegration.h"
#  include <QAbstractButton>
#  include <QMessageBox>
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

// Build the "P" app icon: blue gradient rounded-square, white bold P. Returns a
// QIcon with 16/32/48/256 px sizes.
QIcon makeAppIcon()
{
    auto draw = [](int size) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);

        QLinearGradient grad(0, 0, size, size); // 135° gradient
        grad.setColorAt(0.0, mervin::theme::brand().gradientStart);
        grad.setColorAt(1.0, mervin::theme::brand().gradientEnd);
        p.setBrush(grad);
        p.setPen(Qt::NoPen);
        const int radius = qMax(2, size / 6);
        p.drawRoundedRect(pm.rect(), radius, radius);

        p.setPen(mervin::theme::brand().onBrand);
        QFont f;
        // Cross-platform fallbacks: Segoe on Windows, common sans elsewhere.
        f.setFamilies({QStringLiteral("Segoe UI Variable"), QStringLiteral("Segoe UI"),
                       QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
        f.setStyleHint(QFont::SansSerif);
        f.setPixelSize(qMax(8, size * 55 / 100));
        f.setBold(true);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("P"));
        return pm;
    };
    QIcon icon;
    for (int sz : {16, 32, 48, 256})
        icon.addPixmap(draw(sz));
    return icon;
}

// Command-line options. Non-flag arguments are file paths; unknown flags are
// tolerated/ignored for forward/backward compatibility. Known flags:
//   --profile <dir> (or --profile=<dir>)
//       Keep ALL persisted state (settings, recent files, session, view state,
//       tessdata) in <dir> instead of the user's config dir, and form a
//       separate single-instance group - a dev/test launch never hands its
//       files to, nor restores the session of, the installed app.
//   --quit-after-startup
//       Exit as soon as startup completes and the event loop goes idle.
//       Used by tst_perf_startup to measure startup time.
struct CliOptions
{
    QString profileDir;
    bool profileError = false; // --profile present but its directory missing
    bool quitAfterStartup = false;
    QStringList paths;
};

CliOptions parseCli(const QStringList &args)
{
    CliOptions opt;
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("--profile") || a.startsWith(QLatin1String("--profile="))) {
            QString value;
            if (a.startsWith(QLatin1String("--profile=")))
                value = a.mid(QStringLiteral("--profile=").size());
            else if (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1String("--")))
                value = args.at(++i);
            // A malformed --profile must hard-fail (see main): silently
            // proceeding would read/write the REAL user state - the exact
            // thing the flag exists to prevent.
            if (value.isEmpty())
                opt.profileError = true;
            else
                opt.profileDir = value;
        } else if (a == QLatin1String("--quit-after-startup")) {
            opt.quitAfterStartup = true;
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
    // The primary is a background process, so Windows would refuse the
    // SetForegroundWindow it issues when surfacing the target window (at most
    // the taskbar button flashes, and a minimized window stays minimized). This
    // freshly launched sender still holds foreground rights - pass them to the
    // primary before handing over, so its activation succeeds. The primary's
    // PID sits in the single-instance lock file (QLockFile records its holder).
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

#ifdef Q_OS_WIN
// First launch only on Windows: offer to make Mervin the default PDF viewer.
// The whole check is one-time - on the FIRST run we look at whether Mervin is
// already the default and prompt only if it is not; on every later run we skip
// both the registry read and the prompt, so normal startup is never slowed.
//
// Windows 10/11 forbids silently taking over a file association, so "Yes"
// registers Mervin as a candidate .pdf handler and opens Settings -> Default
// Apps for the user to confirm. Run BEFORE any window is created: MainWindow
// loads its own Settings at construction and rewrites the whole file on close,
// so the flag must already be persisted by the time the first window exists.
void maybePromptSetDefaultPdfApp()
{
    mervin::Settings s = mervin::Settings::load();
    if (s.promptedSetDefaultApp)
        return; // One-time check already done - don't touch the registry again.

    // Record that the one-time check has now happened (whatever its outcome) so
    // subsequent launches skip both the registry read and the prompt entirely.
    s.promptedSetDefaultApp = true;
    s.save();

    // Nothing to offer if Mervin is already the user's default PDF viewer.
    if (mervin::PlatformIntegration::isDefaultPdfHandler())
        return;

    QMessageBox box;
    box.setWindowTitle(QStringLiteral("Mervin PDF"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("Make Mervin PDF your default PDF viewer?"));
    box.setInformativeText(
        QStringLiteral("Mervin will then open PDF files when you double-click them. "
                       "You can change this any time from Settings."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    box.button(QMessageBox::No)->setText(QStringLiteral("Not Now"));

    if (box.exec() != QMessageBox::Yes)
        return;

    if (mervin::PlatformIntegration::registerPdfHandlerAndPromptDefault())
        return;

    // The sandboxed Snap (and any environment where the association can't be
    // changed from inside the app) can't do this automatically - guide the user
    // to finish it once from the desktop's settings. The Snap-exported .desktop
    // advertises MimeType=application/pdf, so Mervin shows up there as a choice.
    QMessageBox info;
    info.setWindowTitle(QStringLiteral("Mervin PDF"));
    info.setIcon(QMessageBox::Information);
    info.setText(QStringLiteral("Finish setting Mervin PDF as your default viewer"));
    info.setInformativeText(
        QStringLiteral("Open your system's Settings → Default Applications (or "
                       "right-click a PDF → Open With) and choose Mervin PDF for "
                       "PDF files."));
    info.exec();
}
#endif

#ifdef Q_OS_WIN
// Mervin ships as a Windows GUI-subsystem app, so launching it from Explorer, the
// Start menu, the installer, or a PDF double-click never pops up a console window.
// The cost of that is a GUI app started from a terminal would normally swallow its
// stdout/stderr - so when there IS a parent console (a dev shell), attach to it and
// reopen the std streams. qDebug()/stderr then shows up in that terminal exactly
// like the old console-subsystem build, with no stray window for end users.
// AttachConsole fails harmlessly when there is no parent console (the Explorer /
// Start-menu case), in which case we leave the streams alone and do nothing.
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
int runUi(QApplication &app, const CliOptions &cli, const QStringList &paths,
          std::unique_ptr<SingleInstanceServer> server)
{
    app.setQuitOnLastWindowClosed(false); // WindowManager drives process exit
    mervin::WindowManager wm;
    if (server)
        wm.adoptInstanceServer(std::move(server));

    // Windows-only first-run prompt (once ever). Before windows exist - see the
    // note above. Skipped for --profile runs: a fresh profile would re-trigger
    // it, and a dev/test instance must not touch file-type registration.
#ifdef Q_OS_WIN
    if (mervin::ConfigPaths::overrideDir().isEmpty())
        maybePromptSetDefaultPdfApp();
#endif

    // Create the window BEFORE reading any document, so the event loop's first
    // turn paints it. Every open below is then staged onto later turns (see
    // WindowManager::openStaged): a cold-cache open is mostly disk reads and the
    // virus scanner's first pass on the file, and doing a whole session inline
    // used to freeze the process with nothing on screen at all.
    wm.createWindow();

    // Crash recovery / session restore (M11): reopen the documents that were
    // open last time (still-existing files only). On by default.
    QStringList restore;
    if (mervin::Settings::load().restoreSession) {
        for (const QString &p : wm.sessionPaths())
            if (QFileInfo::exists(p))
                restore.append(p);
    }

    // Command-line files first, then the session with its previously-active
    // document ahead of the rest - see planStartupOpens for the ordering rules. A
    // file that is BOTH on the command line and in the session deliberately stays
    // in the restore list: it opens once (as the command-line entry, which takes
    // the view) and its restore entry becomes a no-op that still holds its saved
    // tab position, so double-clicking a file from the last session no longer
    // migrates its tab to the end of the bar.
    const QList<mervin::StagedOpen> batch =
        mervin::planStartupOpens(paths, restore, wm.sessionActivePath());

    // --quit-after-startup: quit once the whole batch is open, NOT at the first
    // idle turn. tst_perf_startup times this process as plain runtime, and
    // quitting at the first turn would silently stop measuring the documents.
    std::function<void()> onDone;
    if (cli.quitAfterStartup)
        onDone = [] { QCoreApplication::quit(); };
    wm.openStaged(batch, restore, onDone);

    // Optional, opt-in background update check (off by default). Primary instance
    // only - secondary launches hand off and exit before reaching here. Delayed so
    // it never slows first paint; the checker fails silently on any error and
    // deletes itself when done.
    if (mervin::Settings::load().checkUpdatesOnStartup) {
        QTimer::singleShot(2500, qApp,
                           [] { (new mervin::UpdateChecker(qApp))->checkOnStartup(); });
    }

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
    QApplication::setWindowIcon(makeAppIcon());

    const CliOptions cli = parseCli(QApplication::arguments());
    if (cli.profileError) {
        std::fprintf(stderr, "MervinPDF: --profile requires a directory "
                             "(--profile <dir> or --profile=<dir>)\n");
        return 2;
    }
    if (!cli.profileDir.isEmpty()) {
        // Redirect ALL persisted state before anything resolves a path or the
        // single-instance name. The update checker's QSettings normally live in
        // the registry / org paths - point them into the profile too, so a
        // profile run is fully self-contained.
        mervin::ConfigPaths::setOverrideDir(cli.profileDir);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           mervin::ConfigPaths::configDir());
    }

    const QStringList paths = toAbsolute(cli.paths);
    const QString behavior = mervin::Settings::load().openBehavior;

    // Single-instance: the first launch owns the pipe and becomes the primary UI
    // process; later launches hand their files to it and exit. There is no
    // separate background or tray process - when the primary's last window
    // closes, the process exits and nothing is left running.
    auto server = std::make_unique<SingleInstanceServer>();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (server->start())
            return runUi(app, cli, paths, std::move(server)); // we are the primary
        if (handOffToPrimary(paths, behavior))
            return 0; // delivered to the running instance
        // The primary released the pipe between our failed start and the connect
        // (it was shutting down) - loop once to try taking over ourselves.
    }

    // Could not coordinate (extremely unlikely). Open standalone so the user
    // still gets their file; this launch simply isn't the single-instance owner.
    return runUi(app, cli, paths, nullptr);
}
