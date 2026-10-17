# Mervin PDF - Design and architecture

This document summarizes the current implementation for contributors. It focuses on
the components and data flows needed to understand the application. User-facing
behavior is described in [spec.md](spec.md).

## Technology

Mervin is a C++20 desktop application built with CMake.

| Component | Responsibility |
| --- | --- |
| Qt 6 Core, GUI, Widgets, Network, and PrintSupport | Native UI, printing, networking, and local IPC |
| Qt Linguist tools (lupdate, lrelease) | UI translation catalogs |
| MuPDF | Document parsing, rendering, text extraction, forms, annotations, and OCR |
| qpdf | Page operations, encryption, permissions, and measurement PDF output |
| toml++ | Settings serialization |
| Tesseract data through MuPDF | Local OCR language models |

MuPDF is built from source and linked statically. Windows also statically links
qpdf, toml++ and their vcpkg library dependencies using `x64-windows-static-md`.
Qt, its plugins and the Microsoft runtime remain dynamic. Linux uses its
distribution's qpdf and toml++ packages without changing their linking
configuration. MuPDF's archives bundle their own copies of libjpeg, FreeType, zlib
and Brotli, libraries that qpdf and Qt also use. On Windows, MuPDF's build prefixes
its JPEG symbols (`FZ_HIDE_INTERNAL_JPEG`). On Linux, `--exclude-libs` keeps every
MuPDF archive symbol out of the executable's dynamic symbol table, so the shared
system libraries and Qt's plugins keep using the system copies. The application is
licensed under AGPL-3.0; dependency notices are maintained in
[THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## Source layout

The build produces the static libraries `mervin_core`, `mervin_viewer`, and
`mervin_app`, plus the `MervinPDF` executable. Widget tests link the same
implementations as the application.

```text
src/
  main.cpp       application startup and single-instance handoff
  app/           process-level window and store ownership
  config/        paths and TOML settings
  dialogs/       application dialogs
  extract/       extract-plan model
  i18n/          UI language list, OS language matching, and catalog loading
  ipc/           process lock, local socket, launch handoff, and message framing
  merge/         merge-plan model
  net/           safe URL request and download helpers
  ocr/           language-model discovery and validation
  platform/      Windows and Linux integration
  print/         print ranges, sheet placement, raster plans, and the preview
  recent/        recent-file and per-file view-state stores, volume checks
  render/        MuPDF documents, rendering, text, tools, forms, and annotations
  security/      qpdf page, security, and measurement-output services
  session/       open-session and closed-tab state
  ui/            main window, viewer, tabs, panels, sidebars, theme, and icons
  update/        release checks, per-package downloads, and installing updates
```

`mervin_core` owns the document and service code that can be tested without Qt
Widgets. The executable owns windows, dialogs, and the coordination between user
actions and core services.

## Runtime ownership

The principal ownership tree is:

```text
main() / runUi()
├── QApplication
└── WindowManager
    ├── RenderEngine
    ├── recent, view-state, session, and closed-tab stores
    └── MainWindow(s)
        ├── outline, thumbnail, and comments sidebars
        └── TabPage(s)
            ├── Document
            ├── ViewerWidget
            │   ├── TextIndex, layout, and caches
            │   └── FormModel, AnnotModel, measurement, and OCR state
            └── tool panels
```

`WindowManager` is the process-level coordinator. It owns the shared render engine,
tracks every window, routes file-open requests, prevents duplicate tabs for the same
canonical path during normal opens, moves tabs between windows, and is the sole
writer of the shared recent, view-state, and session files. Queued jobs hold a
shared lifetime gate: document destruction clears its pointer under the gate lock,
waiting only for active document access. Rasterization of an independent display
list can finish afterward. A document, a render worker or a search can outlive the
engine (see Rendering and document access).

The explicit Duplicate to new window command bypasses normal path deduplication and
creates a second independent view.

### Tab order across nested event loops

A tab's position changes whenever a tab in front of it arrives or goes, and tabs
arrive at any time. Session restore, late restore, Ctrl+Shift+T and other instances
open them from timers and answers. Code that keeps a tab across a nested event loop,
such as a question, a menu, a password prompt or a tab drag, therefore keeps the tab
itself, not its position, and finds its position again afterwards. The close
questions, the tab menu and the resume password prompt hold a `QPointer<TabPage>`,
and a drag out of the bar follows the page that `MainWindow` stores in each tab's
data of the visible bar (`DetachableTabBar`). A page's right-click menu closes when
another document takes the view, since its rows act on the document on screen and
its Save Page As on a page of the document clicked. Closing a window includes tabs
that arrived while its close waits. Close all tabs closes the tabs it recorded and
leaves newcomers. A single-tab close follows its tab across moves, while a window
close and Close all tabs skip tabs moved out. Quit includes newly opened windows.
Close rechecks edits before approval, so changing a tab after Don't save asks again.

A move within the bar is `QTabBar`'s own and follows the pressed position, so a tab
that arrives while the button is held can make it move the tab now at that position.
This can change tab order. New-file password prompts belong to a placeholder tab
that was already inserted, so they retain that tab's identity.

Removing a tab later, from a background answer, waits until the window can change
its tabs (`MainWindow::canChangeTabs`). That means no modal dialog or popup in the
application, no document operation or unload in the window, no mouse button held and
no tab drag. A modal dialog in one window therefore holds back changes in the others
too. `MainWindow::whenTabsCanChange` queues such a change and never runs it inside
the call. The window runs its queue in order on the next turn of the event loop,
after a mouse button release, after a modal window closes, when a document operation
ends (`MainWindow::OperationScope`) and on the one-second activity tick. A change
finds its tab and window again when it runs. Once a quit or restart closes a window,
it runs no more changes, and those still queued go with it. `MainWindow::openFile`
inserts its placeholder synchronously. Accepted picker choices and completed
downloads enter through `queueOpenPaths`; active session requests prevent admission.
A tab's operation that has ended shows its questions and errors through this
queue too, in the window that holds the tab by then (see Page and security
operations).

### Disk access off the UI thread

A file call on a drive or network share that stopped answering blocks its thread
until the system gives up, which can take minutes or never happen on a hard NFS
mount, and nothing can cancel it. The UI thread therefore hands disk work to
`background::start` (`io/Background`) and never waits for it. Results come back
through `background::post`, which drops them if the object that asked is gone.
Nothing joins these threads. Ordinary close requests first wait asynchronously
for user operations through `CloseFlow`. Once closing is allowed, a window or the
application can leave stuck work behind, and the process ends without running
static destructors while any is left (`main.cpp`). A stuck call keeps its thread, so
work is bounded by its caller, one task per drive or per user action, never one per
file. `background::SerialQueue` runs a caller's jobs one at a time on such a task,
so one that is stuck holds up only the jobs behind it, on one thread.

Work that reads a document follows the same rule. The render workers, the find
search, OCR recognition and a tab's asynchronous load all run on background tasks,
and closing a tab, the OCR popup or a window never waits for them. Destroying a
`Document` waits for the work in it and closes its file, so a closing tab, and a tab
unloaded when its window goes to the tray or after a time inactive, hands its
document to `Document::closeInBackground`. Each close takes its own task: closing is
the user's action, one tab or window at a time. A tab's load runs in the tab's job
slot (`TabPage::Job`). A tab that stops waiting for its load, or closes, lets go of
it through `abandon()`, which never waits. A load still running closes what it opened
itself, on its own thread, and a document the load already read closes in the
background. Either way the load ends only after that close, and a file such as the
recovery copy of a tab that closed goes after it. A document on its way between
threads is an `OwnedDocument`, which closes in the background wherever it is dropped,
so a result that nobody takes never closes on the UI thread. Code that removes or
replaces the file next closes the document itself first. A recovery copy the document
has open is removed only after it is closed, as Windows cannot remove an open file.
Ordinary cleanup queues the manifest removal immediately. Recovery removals respect
the session outcome gate described below, preserving committed copies after
successful session end. Ordinary quitting waits for those removals within the time
it gives the app's own files (see Persistence).
MuPDF's Tesseract runs one recognition at a time in a process, so a canceled OCR
still finishing makes the next one wait its turn on its background thread. At
ordinary exit, `main.cpp` flushes only standard output and error before leaving with stuck work
behind, as flushing every C stream would wait for one that a stuck read of a document
holds. Successful session end skips this flush and the ordinary exit waits.

A tab follows and stops the job it started through a `job::State` (`io/JobContext`),
which the job makes current on its thread with a `job::Scope`. The code that reads and
writes document files reports each step to the current scope: `AtomicPdfWriter`,
`DocumentOutput::replace`, the qpdf sources in `PageOps`, `QpdfService` and
`MeasureExport`, and `RenderEngine::open`. A read names its file, a write its folder
and a commit the file it replaces, and the writers tick for each piece written, so the
tab can tell what it waits for and that a long write still answers. qpdf reads most of
a source only while it writes the output, so the qpdf sources (`security/QpdfSource`)
report their file again whenever a read follows a write, and `AtomicPdfWriter` reports
its folder again whenever a write follows a read (`job::switchTo`). A thread blocked
in a file call cannot be stopped, so a stopped job gives up at its next step and
starts no later commit. A writer asks `job::beginCommit()` right before
`QSaveFile::commit()`, which syncs the temporary file to disk and renames it. A stop
before that makes it discard its temporary file, and a stop after it answers
Committing and lets the commit finish. `job::DestinationLock` orders the writers of
one file in the process. A writer takes it before it opens its temporary file and
keeps it until the commit returned, so a newer write waits behind a stopped writer
that is still stuck instead of being replaced by it later. The key is `fileKey()`
(`recent/PathKey.h`), the canonical file, or the canonical folder and the name of a
new file, normalized by `normalizePathKey`. The writer computes it on its own thread,
and a writer that waits checks every 20 ms whether its job stopped, then gives up.
Outside a scope nothing is reported to a job and every step goes ahead, and two
writers of one file still take turns, also when one of them runs on the GUI thread.
Test hooks hold a step as a stuck disk would, make one fail, and count the steps
reported on the GUI thread. They act inside and outside a scope.

`FileProbe` (`io/FileProbe`) checks files for the UI: whether each exists, its size,
the mount it is on, and for a missing file whether its volume is attached and its
folder is still there (`FileFacts`). Each drive, share or mount gets one thread at a
time, which checks its files in order, so a share that stopped answering holds up
only its own files and never collects more threads. On Windows the volume is the
drive or share in the path. Elsewhere it is the deepest mount point above the path
(`mounts::answeringFolder`), and below an automount map or GVFS, the server's or
share's own folder. Every file is checked, and within one run of a thread, files in
the same folder with the same recorded mount share a volume check that found the
volume away, since an offline share can stall each check. An attached answer is not
reused, as a volume that went away meanwhile would then have files forgotten as
removed. A check is the file with the mount recorded for
it, which the volume check depends on. One already waiting is not queued twice, an
answer younger than the caller's limit is announced again rather than checked again,
and checks that a decision waits for, such as session restore's, go ahead of those
already waiting on their volume. `WindowManager` owns one probe for every window, so
each file is checked once whichever list shows it.

Before installing a PDF, `RenderEngine::open` reads the complete source in 64 KiB
chunks into a MuPDF buffer. It checks cancellation before and after each read and
ticks progress for each chunk. A stream error fails the open rather than accepting
partial bytes. `fz_open_document_with_buffer` gives the document its own reference
to those bytes, and the opener closes the source stream. Later page renders, forms,
outline, measurements, text selection and pointer lookups use the loaded bytes,
including after Stop waiting while the original drive still does not answer. The
existing document lifetime owns the buffer, and unloading releases it.

Each loaded document therefore keeps roughly its file size in additional memory,
and its first page waits until the complete read finishes. Allocation failure fails
the open, without a file-backed fallback that would restore lazy disk reads. Other
self-contained formats use the same buffer. Formats that need their containing
directory or a named file keep their existing path opener for compatibility.
Document save uses a full rewrite, so it does not need the original source stream.
Fresh opens and resumed tabs both preload on their worker.

Each `TabPage` groups one tab with its viewer, document when loaded, and retained
state when unloaded. Tab detachment and merging reparent this tab object rather
than reopening the file.

#### Disk access still on the UI thread

These still touch the disk on the UI thread and are planned work:

- Viewer queries still parse and inspect document data on the UI thread and can
  wait for another thread's document access lock. Loaded PDFs now use an owning
  memory buffer, so these calls do not read the original file. Moving every
  viewer query to background threads remains separate work. Directory-dependent
  non-PDF formats retain their path-based lookup of relative resources.
- Reads at startup before the first window, and the system file dialogs, which
  run on the UI thread by design.
- Opening a folder or a link with the system's handler (`QDesktopServices::openUrl`),
  which on Windows waits for `ShellExecute`.
- Open from URL creates its cache folder and writes and commits its download on
  the UI thread. Opening the completed local PDF uses the background loader.
- Two small updater steps: telling how this copy was installed when the updater
  starts (a registry read on Windows, a check that `$APPIMAGE` is a file on Linux),
  and starting the Windows installer, whose working folder is the download's folder.

### Opening a new tab

Every normal open creates or focuses a tab before returning. The initial key is the
absolute clean path, normalized for Windows case and separators without disk access.
The tab shows Loading and starts its worker even if it is not selected. The worker
resolves links, reads the source stamp and complete document bytes, loads embedded
measurements and caches whether it has form fields. A failed open stays in its tab
with a plain reason and Retry. The reader's own message remains the fallback for an
unclassified failure. Fresh failures are exempt from removed-file cleanup.
The shell claims its initial path in memory before reading, and its resolved path
before installation. An output reserved by a page tool keeps a reason and Retry
until that writer finishes or releases its reservation. A denied private result
closes in the background, and Duplicate follows its failure policy below.

The worker admits a fresh load's resolved canonical path before reading the file
and retains that admission until installation transfers it to the tab. Retry also
checks the known path before starting another read. The result waits in the tab's
job slot until its current window's tab-change queue can accept it. Reading has
finished at that point, so the placeholder returns to Loading and the slow-drive
clock stops. A load-generation check rejects old attempts, and a moved tab joins
its new owner's queue. If its previous owner is destroyed with a queued load,
password prompt or removal, the surviving tab queues that action in its new owner.
Before installation, the owner compares the resolved canonical
key against other tabs. It removes an ordinary duplicate newcomer without recording
history and focuses the existing tab only if the newcomer was still being shown.
The first accepted identity wins when aliases complete together. Independent
recovery copies keep their edited tabs even when their canonical source matches,
and use the original identity cached in their manifests. Successful opens
refresh the title and tooltip and record Recent with the page count. The session
contains placeholders too. Focusing a pending or failed placeholder does not record
an open in Recent. A failed fresh open keeps its reason and generation when selected
or moved, until Retry or an accepted password starts another attempt.

Stop waiting closes an ordinary fresh tab through the queue, without recording a
view state or adding it to Reopen Closed Tab. A result already stored closes in the
background, and a late result never installs. An initial open that adopted unsaved
recovery edits instead keeps its original path, snapshot and Retry, using the same
Stop behavior as a resume. Its recovery view and tools take precedence over fresh
viewer defaults and older stored view state.

All open/resume password prompts use Enter the password for the document, run
window-modal through the current owner's queue, and appear only for its visible,
selected tab. Cancel leaves an ordinary fresh failure with Retry. Duplicate to new
window carries its source view into the pending tab, including an unloaded source's
saved view. A failed Duplicate, including a canceled password prompt, removes only
that duplicate tab and closes its owning window if it is empty. Other tabs that
arrived meanwhile remain open.

## Startup and single-instance behavior

The first process acquires a per-user `QLockFile` and starts a `QLocalServer`
(`ipc::SingleInstanceServer`). `ipc::instancePaths()` builds both names. On Linux
and other Unix systems the folder for the lock file and the socket comes from what
is on disk, not from the environment, so that a launch from the desktop session and
one from ssh, cron or `sudo -u`, with or without `$XDG_RUNTIME_DIR`, take the same
lock. It is `/run/user/<uid>`, as systemd-logind makes it, if that is this user's
own 0700 folder and not a link. That folder is per user and /tmp cleaners do not
reach it. The group name hashes the uid instead of `$USER`. The socket is an
absolute path, and both files have the sticky bit, which keeps them from the
age-based cleanup that the XDG base directory spec allows in a runtime folder.
Without such a folder, or where the socket path is longer than Qt takes there, the
lock goes with the files it guards. It is in `instance-<host>` in the profile folder
that holds `config.toml`, `recent.json`, `viewstate.json` and `session.json`
(`ConfigPaths::configDir()`), for the default profile as for a `--profile` folder.
Launches that use the same profile folder take the same lock, whatever their
`$XDG_CACHE_HOME`, `$HOME` or `$TMPDIR`, and launches with different profile folders
share none of those files. Mervin makes the instance folder with mode 0700 and
leaves the profile folder's own mode as it is. One that already exists is used only
if it is this user's own 0700 folder and not a link. The host part keeps machines
that share a profile folder apart, as a lock that another machine holds never goes
stale. Qt needs a socket path to fit into the 108 bytes of `sockaddr_un` with two
bytes to spare on Linux, one for the terminating zero and one for abstract names, so
the longest path it takes is 106 bytes. Where the profile folder makes the path
longer, or where listening there fails because its file system holds no sockets, as
sshfs, vboxsf and some CIFS mounts, the lock and the carry file stay in the instance
folder and the socket goes to `mervin-pdf-<uid>` in /tmp, which is fixed, unlike
`$TMPDIR`. Anyone can create names in /tmp, so Mervin uses that folder only if it
created it or it already is this user's folder, not a link, with mode 0700. The
primary records where it listens in `<group>.endpoint` in the instance folder
(`ipc::recordEndpoint()`), so a launch that reaches the profile folder through
another path, as through a link, still finds it. If no folder takes the socket, or
listening fails in /tmp as well, the primary keeps the lock and listens nowhere, so
later launches open their files standalone, as beside a primary that does not
answer. Without a usable instance folder there is no lock, and each launch runs on
its own. Two launches of one profile that spell its folder differently, as a
relative and an absolute `--profile` path, still form different groups, as the group
name hashes the path as given. On Windows the lock stays in the per-user %TEMP%,
where released copies keep it too, so that an older and a newer copy still meet
after an upgrade. Two launches there with different values of `%TEMP%` take
different locks. The endpoint is a named pipe. A Store copy and each `--profile`
directory form groups of their own.

Subsequent launches send newline-delimited JSON containing file paths and open
behavior, wait for an acknowledgement, and exit. A launch first checks that the
process at the other end runs as the same user and sends nothing otherwise. Linux
compares the uid from `SO_PEERCRED`, and Windows compares the user SID of the pipe
server's process token, as the pipe namespace is shared by the whole machine. The
server runs its socket work on its own thread. It acknowledges each Open as soon as
it decodes the frame and keeps the Open until the GUI thread takes it, so a primary
that is still starting, or whose GUI thread waits for a profile folder or a dialog,
answers within milliseconds. The first-run window adds the files of waiting Opens to
those of the first window. `WindowManager` then takes them and opens each on a later
event-loop turn, as an earlier Open can show a nested password or error dialog.

`ipc::claimOrHandOff()` makes the launch decision. `start()` tells a lock held by a
live process apart from a lock that cannot be made. While a live process
holds the lock without answering, a launch tries to start and to hand off again for
up to `ipc::kLaunchWaitMs` (10 s, the five-second exit wait plus a margin). An Open
that a primary has taken over a connection without acknowledging it is never sent
again. A launch that gives up closes its connection, and a server that reads an Open
only after that drops it, so a primary that was stopped does not show the files of a
launch that opened them itself. On Linux the frame waits in the socket and the
server sees the end of the connection behind it. Qt's Windows pipes have no buffer,
so the launch's write is still pending when it closes, and closing cancels it. The
gap left is a launch whose deadline passes between the server's check and its
acknowledgement, which is a matter of microseconds. Once a quit is final, the
primary closes its endpoint and keeps the lock until its exit wait is over, so a
launch during that wait becomes the next primary and reads the last writes. If
writes of its own are still in flight when that wait ends, as on a profile folder
that stopped answering, the process never releases the lock
(`SingleInstanceServer::keepLockUntilExit()`). The next copy then waits while the
process lives, and takes the lock as stale once it has ended. QLockFile counts a
lock as stale when the process it names has ended or runs another program. On
Windows the open handle also keeps another process from deleting the file until
then. That copy can be the one `relaunch::startIfDue()` starts at once on Windows.
After the exit wait the process only tears down the application and gives
background work 200 ms before it ends, so the copy normally waits well under
`ipc::kLaunchWaitMs`. A write stuck in a request the system cannot abort, as on a
FUSE or network folder that stopped answering, keeps the process from ending until
the request returns. A launch then waits `ipc::kLaunchWaitMs` and opens its files
standalone. On Windows the copy `startIfDue()` started does the same, and the
carried launches stay in the carry file for the next primary within
`relaunch::kCarriedMaxAgeMs`. On Linux that copy starts only after the process has
ended. While
Quit or a language restart is in progress, or an update asks about unsaved edits
before it closes the windows, `WindowManager` holds the Opens that arrive. If that
is cancelled, they open then. Closing the last window does not hold them, so an Open
that arrives during its prompt opens in that window. Opens the server acknowledged
before a quit became final and that did not open go to `relaunch::carry()`. Before
the process lets go of the lock, `~WindowManager` writes them to `<group>.carried`
next to the lock, as UTF-8 JSON with mode 0600 (`relaunch::writeCarried()`), after
any a previous primary left there. Whichever copy next becomes the primary reads and
removes that file when it takes over its server, under its lock, and opens those
files. It also takes the carry files in the group's other folders whose lock no
live process holds, as a primary that ran in another folder leaves them (see
below). That is the copy `relaunch::startIfDue()` starts once the process has quit,
which is also the copy of a language restart, the copy an update starts, or a copy
the user starts first. No path goes on a command line or into the environment,
unless the carry file cannot be written. The launches then stay with `relaunch`,
and the copy that `startIfDue()` starts gets their files as arguments. They are
absolute paths, so the copy cannot read one as an option. On Windows QProcess builds
the command line itself and no cmd runs. On Linux the helper shell that waits for
this process gets them as positional parameters and passes them on with a quoted
`"$@"`, so it never parses them. A copy an update started before that write failed
cannot get them, so they are logged instead. A file older than 10 minutes
(`relaunch::kCarriedMaxAgeMs`), as one left by a copy that crashed, is removed
unread, as is one dated more than a minute ahead (`relaunch::kCarriedMaxAheadMs`),
which allows for a clock set back a little between the two copies. The age bound
leaves time for an installer between the two copies. At session end nothing is
written or started, so those files are lost with the session. On Linux `start()`
connects to the socket before it removes it, so a lock file that something deleted
while its primary runs does not make the next launch a second primary. The server
closes its socket before it releases the lock. QLockFile cannot tell whether a lock
file without content is stale, as a process leaves one that ended between making and
writing it, so `start()` removes such a file once it is 2 s old and tries again. On
Unix it does the same for any lock without a live holder. QLockFile does not let it
remove a file that a live process holds.

If a live process holds the lock but no primary takes the Open in that time, as
when the lock holder is stopped, the launch opens its files in a standalone process.
It shows no first-run window, restores no session, adopts no recovery copies, writes
none of `recent.json`, `viewstate.json` and `session.json`, and has no updater, as
those belong to the lock holder. Its windows still save settings, such as their
geometry. If the lock or endpoint cannot be made and nothing answers, nothing owns
that state, so the launch runs the full UI without a server. There is then no
single-instance coordination. Each later launch does the same, and those copies
share the stores, last writer wins.

While nothing holds this group's lock, `claimOrHandOff()` first looks for a primary
elsewhere. One of this version can listen in another of the folders above, as when
`/run/user/<uid>` appeared with a desktop login after it started from ssh.
`ipc::otherInstancePaths()` lists the group's paths in those folders that already
are this user's private folders, and creates nothing. A copy that holds the lock in
one of them and does not answer there is starting or in its exit wait. The launch
waits for it as for a holder of this group's lock, within the same
`ipc::kLaunchWaitMs`, and hands off once it answers. On Linux, copies built before
these locations moved listen in the temp folder, under a group name hashed from
`$USERNAME` or `$USER` (`ipc::legacyEndpoint()`). That is upgrade support, to be
removed once such copies are no longer expected. Each is offered the Open in the
same protocol, after the same check that the process runs as this user. The lock
check (`ipc::lockHeldByLiveProcess()`) only reads the lock file, so it never makes a
lock another launch could see. Only this user's processes write locks in these
folders, so it counts a lock as held only while the process the file names lives and
this user can signal it. A lock does not count if its process ID now belongs to
another user, root or the kernel, if it comes from another machine ID or an earlier
boot, or, on Linux, if that ID now runs another program of this user. A process of
this user whose program cannot be read still counts, as one whose last thread waits
in a stuck file system call after the rest of it ended. A file without content
counts as held for 2 s while its maker writes it. A copy of this version that holds
the lock gets the launch with the whole wait, and the other folders and an older
copy are not asked. If one of them takes the Open, the launch exits. If it takes the
connection but does not answer within the wait, it keeps the Open, so the launch
opens the files in a standalone process, as beside a primary of this version that
does not answer. An older copy, which has no rule to drop such an Open, shows the
files as well once it runs again. If nothing answers, the process is another user's,
or it closes the connection without an acknowledgement, as a quitting copy does, the
launch goes on as above. The reverse case cannot be fixed in the older code. An
older copy launched while a newer one runs starts a second primary until one of them
exits. Windows kept its pipe and lock names, so older and newer copies find each
other there in both directions.

Session restoration creates the tab list but opens only the selected document. Other
restored tabs load when selected.

The first window is shown before any document is checked, and command-line files
create their placeholders at once. Staged batches finish when their shells are queued,
so a command-line read that stopped answering cannot delay session restore. The
development-only `--quit-after-startup` option separately waits for requested loads
after both shell batches are queued, so startup timing still measures usable documents.
The session's batch places each restored tab right after the last tab in the bar that
the session saved before it, or first when there is none (`insertIndexForSaved`).
Saved tabs therefore keep their relative order, and a command-line file that the
session also lists stays where its own batch opened it.
`WindowManager::restoreSession` has `FileProbe` check each saved path
(`SessionStore::filesToCheck`) and waits, with the window live, until every check has
answered or `kRestoreWaitMs` (1.5 s) has passed. `SessionStore::prepareRestore` then
decides from the answers, without touching the disk. A file that is there is
restored. A missing file whose folder is still there on an attached volume was
deleted, moved or renamed and is dropped. Any other missing file gets no tab. Its
volume is away, as for a detached drive, an offline network share or an unmounted
volume, or its folder is missing, which can be a share mounted where the volume
check does not look. A file whose check has not answered in time counts as away
too. The file stays in `session.json` under `unavailable`, next to `open`, with the
date it was first found missing. If its check answers later in the same run, a file
that is there opens as an unloaded tab, placed the same way once the session's other
tabs are in, without taking the view unless the window shows no document yet. One
gone from its folder is dropped. The session is not saved from the start until its
documents are queued (`WindowManager::holdSession`), so nothing the restore has not
decided on yet can be lost. A document the user opens during the wait keeps the
view, as a command-line file does, and a command-line file with a recovery copy
opens from that copy.
`WindowManager::updateSession` writes the kept entries back without checking the
disk. An entry leaves the list once its file is open in a tab. A later start that
finds the file reopens it after the other restored tabs. Entries expire 30 days
after that date, without a disk check. With session restore off, the saved session
is not loaded and nothing is kept. Restored tabs take the canonical paths the
session stores, so creating them does not touch the disk either.

`WindowManager` owns the system tray lifecycle. With Close to tray enabled, a window
close hides that window without destroying its tabs. Tray actions restore windows
or explicitly quit, including the usual unsaved-edit checks. Disabling Close to tray
or having no available system tray preserves normal window closing. The process
exits when its final window is actually closed. A file-open handoff can bring a
hidden window back without creating a second copy of its document.

On Windows, `commitDataRequest` uses a direct connection to
`WindowManager::prepareSessionEnd`. `sessionend::RecoveryWrite` counts scheduled
loaded Save and edited unload snapshots independently of their tab or GUI job slot.
Admission and the initial count share one mutex, so a worker created before its
first document Hold is included. The worker releases protection when its recovery
manifest commits, or when failed or canceled work ends. Ordinary unsaved tabs,
copies, exports, printing, original replacement and reopening do not start a grace
period by themselves.

The callback closes snapshot admission and polls memory state without processing
GUI events, showing dialogs or accessing files. It uses a 3900 ms monotonic budget,
leaving 100 ms within the four-second limit for returning. This bounds Mervin's
wait, but an OS scheduling delay can still postpone the callback's return. The
callback allows session end when snapshots settle or the deadline expires. Edits
without a committed recovery manifest can be lost. Repeated queries reuse the
answer. The single-instance server keeps acknowledging launches on its own thread
meanwhile. Their files wait with the other accepted opens and open after a
cancellation, and are lost if the session ends. A native WM_ENDSESSION filter resets
admission after cancellation and retains it after successful end.
Queued tab changes and dialogs wait for that outcome too. Cancellation posts their
resumption for the next GUI turn, after Qt has restored Windows input handling.
The successful session path skips the normal five-second close/write budget,
restart and background-settlement delay.

Completed Security and Merge results, retained Security choices and document-load
reactions enter the tab-change queue before checking final shutdown. Accepted
Security and Merge requests preserve any already captured destination consent
through pending session end. Accepted document passwords, Open picker choices,
completed URL downloads and output-open confirmations also wait in that queue.
Cancellation delivers eligible continuations in their surviving owner without
replacing the approved destination snapshot or repeating downloads. Load
continuations reject stale generations. Successful session end keeps them suppressed until
their owners are destroyed. These operations add no recovery grace.

Startup batches retain both pending documents and their completion callbacks.
Confirmed late restores, Reopen Closed presses and accepted IPC path batches also
wait for the session outcome. Cancellation resumes this manager-owned work on a
coalesced GUI turn, retaining path order and each batch's window behavior. Reopen
starts a fresh file check so an expired pre-cancellation timer cannot consume its
press. Closed restore targets stay closed. These continuations add no recovery
grace and remain suppressed after successful session end.

Qt 6.12 normally flushes every buffered C output stream after its logoff callback.
A writer holding a stream lock can extend that wait beyond our deadline. The native
filter forwards logoff queries for existing Qt windows back through Qt with only
`ENDSESSION_LOGOFF` removed. It returns Qt's exact result and preserves the other
flags, input suspension, process-wide callback and normal `WM_ENDSESSION` handling.
This avoids the global flush without replacing Qt's session manager. Native
helper windows and non-logoff queries retain their normal handling.

Before a worker deletes a committed pre-adoption recovery, it atomically rearms
its protection. If a session request already started, the worker keeps the copy
and waits for the OS outcome. Cancellation permits deletion. Successful end keeps
the discoverable copy for startup. `appfiles::removeRecovery` gates adopted
recovery removals on its writer thread against an already active OS request.
Superseded Unload cleanup uses the same `sessionend::beginRecoveryRemoval` gate
after the old document closes and its retained readers finish. It removes the PDF
before the manifest, and keeps the manifest if PDF removal fails. Neither path
adds snapshot protection. Cleanup admitted before the query remains ordinary
cleanup, so obsolete copies do not add grace after a newer recovery or the original
is durable. File calls occur outside the registry mutex. Ordinary appfiles jobs
keep their existing behavior.

The development-only `--profile <directory>` option redirects application files and
Qt settings, and gives the process a separate single-instance identity. Tests and
manual checks use it to avoid touching normal user state.

## UI languages

The UI text is written in English. Every other language has a Qt Linguist catalog,
`i18n/mervin_<id>.ts`, named by locale ID (`sv`, `zh_CN`, `zh_TW`) or the explicit
Montenegrin app ID `cnr`. In
`CMakeLists.txt`, `qt_add_translations` compiles each catalog into `mervin_core`
as `:/i18n/mervin_<id>.qm`, so every package and test carries them without install
rules. The build embeds a separate standard-widget catalog (`qtbase_<id>.qm`).
It uses Qt's catalog where available and reviewed `i18n/qt/` supplements for missing
languages or widget contexts. Configure stops when neither exists. The loader
installs Qt's catalog, its supplement, then the app catalog, so app translations
take precedence while upstream internal diagnostics remain available. `lrelease`
runs with `-nounfinished`, so a message not yet marked finished shows in English
rather than as a draft. `mervin_en.ts` holds only the English plural forms of `%n`
strings. [TRANSLATING.md](TRANSLATING.md) covers updating catalogs and adding a
language.

`i18n::availableLanguages()` lists the compiled catalogs and `i18n::apply()`
installs one. `main()` applies the language before it builds any window, because
widgets set their text once, when they are built. It takes `--language <id>` for
that run, otherwise the `ui_language` setting; an unknown ID shows English. While
`ui_language` is empty, the first-run window (`FirstRunDialog`) asks for the
language before the first main window. As the only window open, it follows a
change at once through `LanguageChange`, and it opens the files of launches that the
server acknowledges meanwhile with the first main window.

The first-run window also offers **Download OCR**, checked by default. Continue
saves the display language before scheduling one background download attempt for
each matching OCR model. This first-run choice takes effect without a restart.
Closing the first-run window keeps the display language but skips the downloads.

The other windows never retranslate. When Settings picks a different language, OK or
Apply saves it and `WindowManager::restart()` closes every window as Quit does, so
unsaved documents still prompt and cancelling one cancels the restart. `relaunch`
then starts the new copy from `main()` once the single-instance server is gone. That
copy also opens any launches carried (see above). On Linux it waits until this
process has exited. From an AppImage it starts the AppImage file, and a `--profile`
run keeps its profile.

`i18n::suggestedLanguage()` pre-selects the first-run language. It walks
`QLocale::uiLanguages()` in order and takes the first catalog with the same
language and script, preferring the same territory, so zh-HK picks the zh_TW
catalog and de-AT a de one. The first tag of a language decides its script. Qt
ends a Taiwan list with a bare "zh" (zh-Hant-TW, zh-TW, zh-Hant, zh) and turns the
"zh" in Debian's and Ubuntu's `LANGUAGE=zh_TW:zh` into zh-Hans-CN. Both mean
Simplified Chinese, so they are skipped after a Traditional tag. Nothing matching
gives English.

Only text is translated. Mervin never calls `QLocale::setDefault`, so `QLocale()`
stays the OS regional format whatever the UI language. Recent and Settings format
their dates and file sizes with it. The annotation card shows its date as
`yyyy-MM-dd` in every language. Measurement values, zoom and page ranges keep
their fixed, locale-independent format, and unit symbols, settings keys, log
output and text written into PDFs stay as they are. `apply()` also sets the layout
direction from the language. For Simplified Chinese, Traditional Chinese, Japanese
and Korean, it adds an installed font for that language as the fallback for Han text and the relevant
kana or Hangul script. Otherwise Han text can fall back to a Japanese font with
different glyph shapes. These languages and Arabic and Thai also use an installed
primary font for their script, preventing standard
controls from sizing taller text with Latin-only font metrics. Font size and
weight remain unchanged; switching back restores the original font families.

## Inactive documents and memory

`unload_inactive_minutes` defaults to 30 and accepts whole minutes up to 10080. Zero
means Never and disables both timed unloading and unloading when closing to the
tray. `close_to_tray` defaults to true and controls window hiding separately.
Activity tracking keeps the selected document in a visible, non-minimized window
loaded even without keyboard focus. Background tabs, documents hidden by Recent,
and documents in minimized or hidden windows become eligible after the timeout.

Suspension preserves the tab identity and view state while releasing its document,
rendered pages, extracted text, geometry, and editing models. It cancels outstanding
render and search work so old results cannot repopulate a suspended viewer or replace
the resumed document's state. Closing to the tray requests suspension immediately
unless the interval is Never. Selecting a suspended tab reloads it asynchronously.
A tab busy with an operation (see Page and security operations) keeps its document.
Closing a tab abandons its housekeeping load without waiting. Choosing Save in the
close question writes the retained recovery copy on a worker without loading the
document. Explicit Save uses the same path for an unloaded edited tab. Stop or
failure cancels the close and retains its edits. Operations that need a loaded
document remain unavailable until its load finishes.

An unloaded or loading tab shows a placeholder on the page canvas, with its own canvas
tokens for text and buttons in both themes. A load shows "Loading" at once. The tab
polls what its job reports every 250 ms, and after 2 s without progress
(`TabPage::kSlowMs`) the placeholder names the document and the folder the load reads,
"Opening plan.pdf from /nas/docs. The drive is not answering.", with Stop waiting. The
folder is the recovery folder when the load reads the recovery copy. The folder wraps
after each separator that ends a folder name, also on Windows, but a line never ends
on the separators a path starts with or on the ellipsis of a shortened part. A name or
folder with a piece still too wide for a line is shortened in the middle, each only as
far as it needs. Both are bidirectional isolates, so a path keeps its order in a
right-to-left language. Stop waiting never waits. The tab shows "Document is
unloaded." with Retry, keeps its recovery copy, and selecting it does not load it
again until Retry. The read goes on until the drive answers, because a thread in a
file call cannot be stopped, and then closes what it opened. Retry while it still
reads takes it back instead of starting another read, so a tab keeps at most one read
waiting for the drive. A document that a load had already read when the user stopped
closes in the background. A read that Retry starts while the stopped load still closes
what it read waits for that close.

A clean document releases immediately. An edited document or unfinished measurement
schedules a worker that writes a local recovery snapshot with its forms, annotations,
measurements, and manual scales. The activity tick returns after scheduling, while
the tab remains resident and busy. The worker captures the live PDF under a
stop-aware `Document::Hold`, then releases document access before embedding
measurements and committing the version-1 manifest. The UI adopts the completed
recovery files and closes the original document in the background. The tab retains
its logical path and dirty state, and the snapshot preserves PDF encryption.
Passwords remain in memory, never in the recovery metadata or settings.

Selection and the first document reservation share the job mutex. Selecting before
that reservation cancels unload and restores the live tab immediately. Selecting
afterward lets the worker finish, keeps the same live document, and discards the
new recovery files. Stop during a reservation keeps the existing locked notice
until the drive answers. Closing silently stops this housekeeping job and never
waits on the UI thread. Snapshot failure leaves the live document available and
reports through the tab's current window. Reload failure keeps the tab and its
saved recovery state for retry.

`WindowManager::closeRemovedTabs` closes unloaded tabs without unsaved edits whose
file is gone, by the rule session restore uses to drop a saved document. The file
must be missing from a folder that is still there, on a volume that is attached
(`RecentStore::isRemovedFromDisk`, judged by the mount the session or Recent recorded
for the document). A missing folder can be another volume under the same drive
letter, so it keeps the tab, as it keeps a session document. Tabs with unsaved edits
reload from their recovery snapshot and stay. The check runs on window activation and
after a failed asynchronous reload, on its own worker thread, since network paths can
stall. Each file found gone is reported at once, not after the other files. The window then removes the tab on the UI thread without a prompt, a
view-state write or a reopen history entry, and without touching the disk. While the
window cannot change its tabs (see Tab order across nested event loops), or older tab
changes still wait in its queue, it keeps the tab and queues the whole check again
behind them. A close therefore always follows a fresh check, which also spares a tab whose file came back meanwhile. Windows in the tray keep their tabs until shown. Loaded tabs stay open,
because they still show their document. Removing a tab never leaves the Recent
screen, and moving a tab between windows saves the session once, after the move, so
the document keeps its recorded mount.

The shared MuPDF store and allocator can retain memory after individual objects are
released, so process resident memory is not the sum of the live tab caches.

## Rendering and document access

`RenderEngine` owns one base MuPDF context and a small worker pool. Each worker uses
a cloned context, which is MuPDF's supported multithreaded pattern. Requests are
processed newest-first so pages that just became visible take priority. The workers
run on `background::start` tasks and deliver results through `background::post`.
`shutdown()` stops them without joining, so a worker stuck reading a document from a
share that stopped answering cannot hold up a closing window or the end of the
process. Workers, documents, text indexes and OCR services can therefore outlive the
engine. Each keeps the base context alive and drops its own clone first: MuPDF frees
the base context's memory before the parts it shares with its clones, so the base
must be the last of them dropped. `RenderEngine::opener()` clones a context on the
UI thread for an open that runs later on another thread.

A single MuPDF document handle is not safe for concurrent access. `Document`
therefore provides an access mutex used while loading pages, parsing content,
extracting text, reading geometry, or changing PDF objects. Workers build display
lists while holding that mutex, then rasterize the independent lists in parallel.

Work on other threads locks the document's lifetime gate (`DocumentLifetime`) before
the access mutex, never after it. A background job that uses a document a tab shows
takes `Document::Hold`, which locks only the gate, and then calls `Document`'s
methods, which each take the access mutex. The hold waits for the gate in 20 ms
steps and gives up when its job stops or the document has closed. The gate is not
recursive, so the holding thread releases the hold before it reads the document
through a `TextIndex` or OCR, destroys it or waits for it to close. Render workers
wait for the gate in the same steps and drop a request canceled meanwhile, so a
held document does not keep the two to four workers from other tabs once its tab
has canceled its requests. The find search and OCR keep plain waits, as each runs
on a queue of its own.

Queries on the screen thread never wait for a job. While a hold holds a document or
waits for it, the document's queries, `TextIndex` page reads and
`RenderEngine::renderPageImage` return their empty value on the screen thread, log a
warning naming the call and count a refusal (`Document::testing::refusals()`). Other
threads are answered as before. `TextIndex` and `FormModel` do not cache a refused
page, so its text, links and form fields are read on a later call. A screen-thread
query counts itself before it checks for a hold, and a hold counts itself before it
checks for queries, so a query that found no hold ends before the hold takes the
gate. A query made inside another query of the same document, such as
`withPdfDocument` inside `savePdfTo`, is let through, since the outer one keeps the
hold waiting. Nothing changes while no job holds a document.

Each render request carries the requesting viewer's identifier, view epoch, and
token. A viewer discards results that belong to an older zoom, rotation, or view.
This avoids cross-window cancellation while preventing stale images from replacing
current ones. Replacing requests, changing the view, and destroying viewers also
cancel obsolete jobs before further parsing, rasterization, or color processing.

`ViewerWidget` is a `QAbstractScrollArea` responsible for layout, visible-page render
requests, cache use, coordinate conversion, selection, links, and tool overlays.
`ViewerDocumentTools.cpp` implements form and annotation editing;
`ViewerMeasurements.cpp` implements measurement interactions and saved-state comparison.
`ViewLayout` handles continuous or single-page scrolling and the independent spread
setting. Visible pages and current rows use binary search over ordered rows. A page-image cache limits memory use; high zoom levels render visible tiles
and use a preview layer while fresh pixels arrive.

Zoom runs from 8% to 10000%. MuPDF's rasteriser clamps every edge to `kMaxDevicePixel`
(2^20) device pixels from the device origin, and a tile is placed in the page's whole
raster, so a raster longer than that would give blank tiles beyond it and move long
shapes in the tiles before it. `ViewerWidget::maximumScale` therefore lowers the top of
the zoom range for a document whose largest page would pass the limit at the current
device pixel ratio, and every zoom, fit and restored view is clamped to it. The zoom
box shows the clamped zoom. The cap applies to the whole document, because one zoom
lays out every page and maps page points to the screen for selection, annotations,
forms and measurements. It only binds for pages longer than about 3.7 m at 100%
display scaling, or 1.2 m at 300%, so an A0 page keeps 10000% up to 300%. The cap never
goes below 8%. A page too large to render even at 8%, about 4.6 km at 100%, does not
count toward it, so a malformed giant page cannot shrink the rest of the document.
`RenderEngine` refuses a raster beyond the limit instead of returning a blank image, so
such a page shows plain paper, as any page whose render fails does. A move to a screen
with another device pixel ratio (`QEvent::DevicePixelRatioChange`) re-renders at the
new ratio and clamps the zoom again, in every zoom mode. When that changes the zoom, it
keeps the page point in the middle of the view, as any other zoom does.

All interactive geometry uses a consistent unrotated page-point space with a
top-left origin. The viewer converts between that space, screen coordinates, and PDF
user space for rotated pages and non-zero page origins.

## Text, search, and navigation

`TextIndex` extracts structured text per document and keeps glyph rectangles for
selection, hit testing, copying, and find highlights. The in-document matcher adds
case-sensitive and whole-word behavior on top of the extracted text. `DocumentSearch`
owns a separate serial worker and text cache so changing queries leaves the UI responsive.
Nothing waits for the worker: a newer query or closing the tab cancels the running
search, and one stuck reading a page ends once the read returns.

Recent-file content search runs on its own worker and opens files independently. It
returns the first matching page and a short snippet for each file. The Recent page's
All scope lists file-name matches at once and scans only the remaining files. Generation tokens are checked when queued results reach the UI.
Searches run on one `background::start` task per window at a time, with a MuPDF
context of its own that shares the process's MuPDF locks. The task takes the newest
query waiting and ends when none is, so no thread is kept between searches. Nothing
joins it. A newer search or destroying the service stops the running one at its next
file or page. One stuck in a file on a share that stopped answering keeps only the
newest query waiting, so stuck searches never collect threads. Files are scanned in
order, so later files wait while one does not answer.

PDF links, search state, and viewer state remain tab-local. The window-owned sidebars
are rebound to the active tab. Per-file view state is written when tabs or windows
close and restored on the next open.

## Document tools

### Measurement

`Document` reads rectilinear `/VP` and `/Measure` metadata and extracts vector paths
for snapping. `MeasureModel` stores page-local manual and calibrated scale
overrides. `ViewerWidget` owns measurement geometry and handles creation and editing;
`MeasurePanel` exposes the controls.

Editable measurements are serialized into a private PDF catalog stream so Mervin can
restore them. `MeasureExport` uses qpdf to embed that data or to emit flattened PDF
content streams for portable export and printing.

### OCR

`OcrService` renders only the selected page rectangle at 300 DPI and feeds the image
to MuPDF's OCR device. The selected Tesseract language code and the per-user tessdata
directory are passed explicitly; there is no automatic language-selection stage.

`TessdataManager` locates installed `.traineddata` files. The language manager reads
the official `tessdata_best` catalog from GitHub, validates downloaded model files
before they replace anything, and stores them in the writable per-user tessdata
directory. That directory is in the
profile, so listing, saving and removing models runs on one background thread for
the folder, in the order asked (`listModels`, `saveModel`, `removeModel`), and
Settings, the language manager and the OCR selection take the answers as they come.
An OCR selection goes on only for the newest one, only while its document is still on
screen, and only if the languages arrive within 5 s, so a profile that stopped answering
does not bring up OCR windows later. The OCR dialog displays
and edits the result, while the caller retains the selected page rectangle so a
language change can submit the recognition again. OCR captures a display list under the
document lock, then recognizes on a private context in a serial background worker.
Closing the popup or changing languages invalidates results and signals Tesseract
cancellation without waiting for the worker.

Installers include the OCR engine but no language models. When enabled in the
first-run window, `OcrProvisioner` downloads the official `tessdata_best` files for
the selected display language and the OS's primary display language. The fixed
`OcrLanguageMapping` table matches language and script, deduplicates model codes,
and skips unsupported languages without a fallback download. Bokmål and Nynorsk
both use `nor`; Montenegrin and Romansh have no matching model. Simplified Chinese
uses `chi_sim.traineddata`, and Traditional Chinese (`zh_TW`, including OS matches
for Hong Kong and Macao) uses `chi_tra.traineddata`. Explicit Hans or Hant scripts
take precedence over territory. The selected language's model becomes the initial
OCR default, or the OS model when only that one matches. Canceling, as when the app
quits, also stops a finished download whose file the folder's thread has not started
to install.

Saving `ui_language` consumes the first-run offer before any network request.
Downloads fail silently, stop after 30 seconds without data, and are cancelled
on application exit; a model already received is still installed. The folder and
the staging files are made on the folder's thread first, and a download starts only
once its staging file exists, so an unwritable folder costs no download. Received
data and the final check and copy go to the same thread, and a write that fails
stops the download. A quit that leaves the folder's thread busy can leave a staging
file behind. There is no retry or resume on a later launch, including
after a failed download or a later display-language change. If saving the choice
fails, no download starts. Completed files are validated before atomic installation;
failed downloads never replace an existing model. The language manager remains
available for manual installation.

### Forms and annotations

`FormModel` enumerates and edits AcroForm widgets on the live MuPDF PDF document.
`AnnotModel` performs the same role for supported text markup and note annotations.
All changes run through `Document::withPdfDocument`, which serializes them against
render workers. A changed page is evicted from the image cache and rendered again so
screen, print, and save output agree.

Form fields and annotations are standard PDF objects. `Document::savePdfTo` performs
one full MuPDF rewrite that captures both kinds of edits and preserves existing
encryption.

### Page and security operations

`PageOps` uses qpdf for rotation, deletion, extraction, splitting, and merging.
`QpdfService` inspects and changes encryption and permission settings. These services
write new files and do not share live MuPDF document handles with the viewer.

Rotate and Delete write the source document again, and qpdf keeps its encryption.
Extract, Split and Merge build each output from `emptyPDF()`, which has none, so
`PageOps` gives the writer the source's encryption
(`QPDFWriter::copyEncryptionParameters`) when every output page comes from one opened
source. That covers every Extract and Split output, and a merge whose rows all name
one path. The copy keeps the source's password and permission entries and its first
/ID, on which R2-R4 keys depend, so the same passwords open it and its flags stay.
qpdf writes every V4 copy with AES, so a V4 RC4 source, which Mervin's RC4-128 option
writes, gives an AES-128 copy, as it does for Rotate and Delete. A revision 3 RC4
source keeps RC4. When qpdf keeps or copies encryption, it takes the key length from
/Length, which V4 and V5 files may omit. Before Rotate, Delete and each encrypted
copy, `setKeyLength` in PageOps.cpp therefore sets that entry, in memory, to the
length qpdf read the source with. A merge of several files writes no encryption.
`PageOps::probe` reports whether a file is encrypted, including a file with only an
owner password, and `MergePlan::losesEncryption` tells the Merge dialog when to say
that the result will not be encrypted. The window opens an Extract or Delete result,
or a one-file merge (`MergePlan::resultPassword`), with the password that read its
source.

Extract, Split and Merge build each output from an empty PDF with
`QPDFPageDocumentHelper::addPage`, which copies a page but not its form fields.
`FormCopy` records each copied page, and before writing, `FormCopy::finish` gives the
pages private annotations and adds their fields to the output's `/AcroForm`, renaming a
field whose name is taken. It calls `QPDFAcroFormDocumentHelper::transformAnnotations`,
the function behind the `fixCopiedAnnotations` that qpdf's own page selection calls per
page, once for all pages of a source. Each call copies the whole tree of every field it
meets, so per page a 200-page form with one top-level field took minutes and gigabytes,
and a field shown on two pages became two fields. A page copied again goes in a later
pass and gets fields of its own. If a pass throws, as qpdf before 12.4 can on damaged
annotations, its pages are copied one at a time with `fixCopiedAnnotations`, and a page
that still fails stays as `addPage` copied it. `FormCopy` then points each annotation's
`/P` at its new page, which qpdf releases before 12.3 do not do. A copied field tree
comes along whole, so `finish` then removes the fields that have no widget on an output
page, keeping a parent that a copied widget shares. The source helpers are built with the
`QPDFAcroFormDocumentHelper` constructor, as `get` needs qpdf 12.3.

Delete Pages removes pages from the source's own `QPDF` and writes it, so it keeps
encryption, outlines and structure. `QPDFPageDocumentHelper::removePage` only takes a
page out of `/Pages`, so `removeDeletedContent` then removes what only the deleted
pages used. It removes the outline items that only go to a deleted page and have no
items left below them, relinking their siblings and correcting `/Count`. It removes
the structure elements whose content is only on deleted pages, with their `/ParentTree`
and `/IDTree` entries, and the deleted pages' content items from the elements that
stay. It removes the form fields whose widgets are all on deleted pages from `/Fields`,
`/CO` and their parents' `/Kids`, and drops `/XFA` when it removes one. Finally it
replaces the deleted pages, their annotations that no kept page lists and the removed
fields with null (`QPDF::replaceObject`), so links, named destinations and other
references cannot keep their content in the output. A damaged outline or structure
tree is left as it was.

In `PageOps::merge`, an exception while an input is read, such as unreadable
measurement data, sets `failedIndex` to that input. Extract maps index 0 to its
unreadable-source failure. Errors after the inputs are read keep -1, which the
dialogs report as a failed write.

Rotate Pages, Split, accepted Merge plans and Security run as their tab's operation
(`TabPage::startOperation`), so only that tab waits for the file. Page tool prompts
and file dialogs run first, as before.
The tab then commits its form and comment editors, takes a still picture of its pages
(`StillPicture`), pauses the viewer (`ViewerWidget::pauseForOperation`, which stops
its renders on their way, a find search, a zoom ease, auto scroll, its popups and form
editors) and shows the picture in its place. The job runs on `background::start` in the tab's job slot,
the same one a load uses, with values only: the paths, pages, password and the files
open in tabs. It never touches the shown document, which a hidden, paused viewer does
not read either. The window treats a busy tab as having no usable document
(`TabPage::isUsable`): Save, Print, the Document menu, the tools and the view
commands are off, the page number and zoom show where the viewer stopped, and the
sidebars keep what they showed, with thumbnails paused. Switching to a busy tab
clears them, as for an unloaded tab. Duplicate to new window is off for a busy tab,
and unloading skips it. Other tabs and windows stay usable.

The tab polls what the job reports every 250 ms, as for a load. After 2 s without
progress (`TabPage::kSlowMs`) a bar (`WaitBar`) at the top of the tab, from the left
edge to the scroll bar's column, says what the job waits for, with Stop waiting on the
right, and a veil covers the picture below it. A write names its folder, "Saving to
/nas/out. The drive is not answering.", as does a read of a file in that folder. A
read elsewhere names the file, "Reading plan.pdf from /nas/docs. The drive is not
answering." The text wraps and shortens like the slow load notice
(`fitFileSentence`). Once shown, the bar stays until the operation ends and follows
its latest step. Its colours are the `notice`, `borderNotice` and `veil` tokens. Stop
waiting never waits. The tab is usable at once, the job begins no commit afterwards
(see Disk access off the UI thread), and nothing it finds is shown. Split keeps the
files it committed before the stop.

Each job first refuses an output that is open in a tab, the source included, with
"The destination is open in a tab." It compares `fileKey()` of the output, on its own
thread, with the tabs' canonical paths. Split checks all its names before it writes
the first. A shared in-memory admission registry also checks the tabs opened after
the operation started. The job reserves all resolved outputs before any write,
atomically with tab admission. A later open is refused while the reservation lasts.
Stop releases the reservation before a commit, so a canceled writer cannot block a
new tab. A commit already started retains it until the replacement returns. Normal
completion and failure release it when the job's scope closes.

Split reads every output with `DestinationConsent` on the tab's worker after the
open-tab check, so that refusal comes before any replacement question, as in Security.
When an output exists and the run carries no approval, Split writes nothing and
returns every output's state, absent ones included, in `PageToolResult::toApprove`.
`finishPageTool` checks that before the failure messages and asks one window-modal
question with No as the default, "%n file(s) in %1 already exist. Replace them?", with
the folder fitted like the wait bar's (`fitFileSentence`) to half the screen's width.
Yes runs a copy of the `PageTool` whose `approved` field holds those states. Before
writing, the approved run reads only outputs that the approval lacks, as when the
source gained pages, and asks again if one of them exists. `PageOps::split` passes
each part's `mayCommit` to `AtomicPdfWriter::write`. It reads that output again under
the destination lock, after flushing and before commit admission, and compares it with
the state Split checked. Split replaces no file that changed, appeared or was removed
since then. It stops there with the earlier parts committed, and the window names that
file in a warning. A failed check fails the split with the file's name and reason. As
in Merge, another process can still change a file between the final comparison and the
rename. A window in the tray reports existing outputs through `notifyInTray` and
writes nothing.

When the tab's password does not open the file, the window asks for one
and runs the tool again with it, and the tab keeps a password that worked. Everything
a finished operation raises, the password prompt, an error or "Open it now?", is
window-modal in the window that holds the tab by then and waits in that window's tab
change queue. A window in the tray shows an error as a tray message and asks nothing.
`CloseFlow` continues tab, window, Quit and language restart requests through the
event loop. A close waits for a running user operation, selects its tab and shows
the existing wait notice immediately. It restores a waiting tray window. Stop on
that operation continues with the unsaved-changes question. Don't save can close
a stopped tab while its document closes in the background. Save waits for document
release, and Stop during that prerequisite cancels the close.

The close question is window-modal in the tab's current window. Permanent edit
generations and a final commit of active editors catch edits made after an earlier
Don't save answer. Tab removals and final window closes use the current owner's
ordered tab-change queue. A close claim suppresses automatic resume and suspension,
but never blocks that queue. Close all tabs removes each resolved original tab
promptly. Window close, Quit and restart preserve their windows and tabs until all
decisions pass. A Save result settles its original retained decision even after the
tab leaves that window's close scope, and also reaches any current close owner.
Cancel ends the remaining close request, preserving completed saves.
It releases the close claims and schedules the normal current-tab resume in each
affected window. A suspended tab selected for its close question therefore resumes
from its retained recovery copy without another click.
Cancel after a window close or Quit keeps an abandoned fresh tab with Retry. The
application records it in Recent after Retry succeeds.
The relaunch flag is set only after restart approval, before its windows close.

The updater keeps a separate synchronous `tryCloseAllForUpdate` adapter. A running
user operation or another pending close makes it return false immediately. A chosen
Save starts as the tab's background job after the callback returns false, for loaded
and unloaded tabs alike, and never triggers a later shutdown. The adapter returns
true only when approved windows close in that call.

Security first reads the document's encryption and permission information as a
tab operation, including any retry with a newly entered password. `SecurityDialog`
receives the information or read error as values and does no document I/O. Choosing
Encrypt, Remove Password or Strip Owner Restrictions closes the dialog and returns
the destination, passwords, algorithm and permissions to the window. Its save
picker chooses a path without confirming replacement. The next tab operation
captures the destination's absence or exact contents through `DestinationConsent`
before reading the source filesystem. An absent destination continues in the same
worker. An existing destination returns that snapshot for a window-modal replacement
question, then a new immutable request carries the approved snapshot into the write.
The window does not refresh consent while that question is open. Open-tab checks
reject known protected outputs before asking, and every actual write still checks
the source identity and reserves the destination.

Both encryption and decryption compare the approved snapshot again after flushing
the temporary output, under `AtomicPdfWriter`'s destination lock and before commit
admission. A changed or unreadable destination is preserved and reopens the retained
Security choices for another attempt. Source-password retries keep the original
snapshot, including an output that was absent before the password question. Fresh
form acceptance discards old consent. Hashing stays on the tab worker and checks
Stop between filesystem calls and chunks. Identical-byte replacement remains
approved. The lock orders this process's writers; another process can still change
the destination between the final comparison and rename because `QSaveFile` does
not provide an atomic compare-and-swap.

An encrypted result opens with its new open password. A failed write reopens
Security with the choices, destination and original information or read error
retained. Stop discards the result and opens no dialog. Queued results follow a
moved tab if its previous window disappears, select that tab in its current window
before showing the dialog, and are suppressed after confirmed shutdown. A replacement
question cannot authorize a write after its owner disappears, the tab moves away,
or the window becomes hidden or minimized. If the tab survives an interrupted
replacement question, it retains the Security choices and reopens the form when
that tab is usable and selected in a visible, restored window. A confirmation
result that arrives while the window is hidden or minimized retains the same
form. Neither path reuses an old Yes answer. Accepting the reopened form captures
fresh destination consent. The retained form belongs to the tab, follows owner
changes, and does not block other queued work while waiting for visibility. A
successful Security information read that finishes while its window is hidden in
the tray retains the populated form for restoration without repeating the read.
Automatic unloading before queued delivery also preserves that information until
the tab resumes, including a source password accepted for the completed read.

`MergePlan` and `ExtractPlan` are the GUI-free models behind the Merge PDFs and
Extract Pages dialogs, tested in `tst_merge_plan` and `tst_extract_plan`; each dialog
only renders its plan and writes edits back to it. Both dialogs show their rows in
the same reorderable `RowList`. Extract writes through `PageOps::merge`, which fails
on an out-of-range page instead of skipping it, so a stale page count cannot silently
shorten the output.

Extract and Delete first probe the source in a tab job, then open their dialog with
the returned page count and password state. Their constructors do no source I/O.
Acceptance captures parsed page indices, output and password, closes the dialog,
and starts another tab job. This job resolves the current source identity and refuses
an output that would replace it, including a source redirected after the dialog opened.
It reserves the output, verifies the password and page count again, then writes through
qpdf and the guarded atomic writer. The dialog and its Browse picker are window-modal.
A changed count reopens the choices before writing. A failed write or password check also
reopens the dialog with its rows, chosen filename, password and Open when done
choice. Completion follows the tab to its current window. Stop suppresses reopening
and output opening, while a rename already begun may finish. A hidden window shows
failures through its tray notice.

Merge's constructor and Add Files append pending rows immediately, each with a
probe ticket. A shared `background::SerialQueue` per lexical `RecentStore::volumeKey`
reads the PDF information under a separate job state. A running probe keeps its
queue alive after its dialog closes, so reopening against the same blocked source
adds no worker. The lexical key is a drive or UNC share on Windows and a folder on
Unix. Other keys keep their own queues.
Reading rows keep Merge disabled. Destination checks wait until the input probes
finish, avoiding another filesystem call to the source's default folder. The
footer names the first pending row and its file, following row moves and removals
until every probe answers. A filename that cannot wrap shortens in the middle to
keep the row number and reading reason visible. The footer then shows any settled
validation error. Duplicate pending rows share a ticket, and completion follows
that ticket through row removal and reordering. Answers update existing widgets
in place, preserving a range editor or drag grip. One warning reports the remaining
failed rows after their Add Files batch finishes. Closing the dialog stops its
probes and removes only its pending requests without waiting or dropping another
dialog's work. A read already inside a filesystem call keeps its queue until it
returns. Independent destination checks after input reads finish still use each
dialog's SaveCheck and can outlive the dialog if their filesystem calls stall.

Pressing Merge captures replacement consent on its background SaveCheck. It records
the resolved destination key and either true absence or the SHA-256 of an existing
regular file. Missing directory entries are distinct from unreadable files,
nonregular entries and dangling links, which fail the check. Closing the dialog
or requesting another check stops the old capture after its current filesystem
call returns. Consent stays fixed across the replacement question's nested event
loop.

Before publication, Merge compares this consent with the destination again while
AtomicPdfWriter owns its destination lock, after flushing and before commit
admission. A changed output keeps its current bytes and reopens the retained plan
for another explicit Merge and replacement decision. Failed destination checks
also preserve the output and remain retryable. Existing outputs require one full
read at acceptance and another before commit, using bounded memory and checking
Stop between reads. Identical-byte replacement remains approved. The lock orders
writers in this process; another process can still change a file after the final
comparison and before rename.

After SaveCheck and replacement confirmation, Merge accepts its plan and output
without writing. The launching tab's operation checks input and output identities,
refuses open tabs and reserves the destination before calling `PageOps::merge`.
Failure reopens the previously probed rows, ranges, passwords and output with the
existing error summary and detail. A reopened plan assigns new probe tickets above
its inherited ones. Completion follows the tab's current window, including when
an older queued move destroys its original owner. Reopening a failed plan selects
its launching tab. A hidden owner sends the same translated failure summary to
the tray. Stop and confirmed shutdown suppress all completion prompts.

Neither plan touches the disk on the UI thread. Each dialog checks its Save as text
with `SaveCheck` (`io/SaveCheck`) on a background thread. Extract checks whether the
folder exists, whether the name is a folder and whether the file exists. Both find
the file's canonical path, for telling it from the source, Merge's inputs and the
files open in tabs; Merge canonicalizes its inputs only for a write. While Save as
is not edited, the same check steps the default name past files in use. The tabs
hold canonical paths already, so the open files are compared as given. A newer check
drops one that has not started and only the newest answer counts, so a folder that
stopped answering holds one thread per dialog, and later checks in that dialog wait
behind it. Until a check answers, the error line keeps a reason about Save as, so
typing does not make it flicker, and drops any other reason; after 300 ms it says
the folder is being checked and disables the button. Pressing Extract or Merge
checks Save as again and proceeds once that answers, if the rows and Save as are
still what it was pressed for. Extract and Merge close with their captured requests
for the tab's worker. A default name that stepped past a file in use meanwhile
still counts, as the user did not choose it.

Delete Pages is the same `ExtractDialog` and `ExtractStrip` over an `ExtractPlan` in
`Mode::Delete`, not a separate dialog. The rows, Save as, the password row, job
acceptance and keyboard handling are shared, and the mode changes only what
differs. Its rows are a set of pages to leave out, so `RowList` drops the grips and
the Output column and there are no moves. Nor is there a Remove button beside the
rows, since each row's ✕ removes it. `ExtractPlan::cells()` returns every
source page, unfolded, flagged `deleted`, and `deletePage()`/`keepPage()` turn a
click on the strip into row edits, widening, joining or splitting ranges. The worker writes through
`PageOps::deletePages`, which writes the source without those pages and without what
only they used, so the copy keeps the source's structure and encryption, and refuses
to delete every page.

## Save and print pipeline

The application has two complementary writers:

- MuPDF writes the live document when forms or standard annotations changed.
- qpdf embeds editable measurement data, flattens measurement graphics, and performs
  structural or security operations.

Document commands live in `ui/DocumentActions.cpp`. `DocumentOutput` prepares a
snapshot through MuPDF and qpdf, including current forms, annotations, measurements,
and manual scales. An empty measurement set is written too, so deleting the final
measurement persists. Measurement dirty state compares serialized data against its
saved baseline.

In-place Save captures active editors, the full viewer state and measurements on
the UI thread, then freezes the tab behind its still picture. A worker checks the
original's source stamp and queues the existing changed-file question in the tab's
current window when needed. Save reserves the destination using the tab's stable
admission identity, so another admitted view or a new open cannot acquire edits
that this replacement would overwrite. Closing and reopening another tab with the
same path cannot impersonate the captured admission.

For a loaded document, the worker creates a private edited PDF and commits its
recovery manifest before the UI transfers the old document. The worker closes that
document before replacing the original through `QSaveFile`, with direct-write
fallback disabled, then reopens the result. Success restores the full viewer state
and clears the recovery files. Failure or Stop before recovery adoption retains
the original live document and its edits. After adoption, it loads the private copy
under the original logical path and keeps it unsaved. A failed recovery load keeps
its manifest and Retry placeholder. Save from an unloaded edited tab copies its
existing recovery PDF directly, without opening a PDF document.

Stop during a live snapshot keeps the trapped notice until the document Hold is
released. A stopped replacement that already began its rename can still finish.
A new Save waits on the earlier worker before replacing its recovery input, then
checks the original's source stamp again. The changed-file question also applies
if that earlier rename changed the original. Close and quit accept only explicit
Save success. Failure, Stop or a canceled question preserves the tab's edits.
Queued questions and Save reports follow a moved tab even when its previous
window closes and discards its queue. Stopped or superseded attempts cannot show
a late question, warning or Saved status. If replacing the original succeeded but
reopening failed, the warning reports that the file was saved even while the
recovery copy is still loading. It gives that retained copy's path without
claiming that the copy is already open.

Save as Copy and measured export capture the measurement data, password, document
lifetime and path identities before starting one background tab operation. The worker
checks and reserves the destination, creates its private temporary folder, and takes
`Document::Hold` while it writes the live PDF. Export also builds its page transforms
and measurement labels under that hold. The hold ends before qpdf embeds or flattens
measurements and before the final atomic replacement. Both commands preserve the
source's edits and dirty state. A frozen or deleted original remains copyable because
the loaded document owns its bytes.

Entering document use and Stop share the job state's mutex. Stop before that use
restores the tab immediately. Stop during a blocked live-snapshot write keeps the
still picture and says "Stopped waiting. The document stays locked until the drive
answers." Its Stop button disappears. The viewer resumes only after the worker
releases the document. Other tabs and windows remain usable. Stop suppresses result
messages and prevents a later commit, while a commit already started may finish.
The worker owns and removes only its private temporary files. It never removes the
user's final destination as cancellation cleanup.

Completion follows the tab through its current owner's tab-change queue. Success
offers to open the output with its captured password. A window in the tray receives
failure through its existing tray notice and receives no success question. The
Export explanation and completion prompts use guarded heap ownership because a
Quit in another window can destroy their owner during the nested dialog loop.

Print captures the committed document state before showing its existing dialog.
Measured documents first prepare a complete flattened copy on a worker under a
document hold. Its preview and output use that same owned copy. Cancel closes the
copy in the background before removing its temporary folder.

Prepared Print dialogs and failure messages follow the tab's current window queue.
If an earlier tab move closes that queue's owner, its destruction reposts delivery
through the surviving tab. The prepared document remains owned until delivery, and
the dialog still checks that its live document identity matches before opening.

After the dialog, the worker owns the QPrinter, begins and ends its QPainter, and
renders pages with Document::renderPage on the document's own context.
`printing::rasterPlan` sets the raster scale from the page's printed size, so the
quality setting holds on paper. A page reduced to the sheet renders fewer pixels, and
an enlarged page is not stretched. A device-pixel clip limits the raster to the part
of the page inside the printable area, and `printing::rasterBands` splits that part
into horizontal bands of whole raster rows, at most 2^24 pixels each. In Qt's PDF
engine, which Print to file and CUPS printers use, a band holds about 8 bytes a pixel
while it is drawn, so the job's peak memory stays near 135 MB above the app's own on
any sheet and quality. Qt's Windows engine records a sheet's images in a QPicture and
replays them when the sheet ends, so on a Windows printer the whole visible raster of
a sheet stays in memory until then. Each band but the last also renders the next
band's first row, which the next band draws over, because Qt's Windows engine
truncates an image's position and height to whole device pixels. Black and white
renders without antialiasing, so its threshold acts on colours, and MuPDF draws every
stroke at least one pixel wide.

MuPDF's rasteriser clamps every edge to 2^20 device pixels from the device origin, so
on a larger page raster the shapes beyond that vanish or bend. Rendering each band at
the device origin would not lift the limit, because an edge that reaches further from
the band is still clamped, which moves its part inside the band. `rasterPlan` therefore
plans no raster wider or taller than 2^20 pixels, and Document::renderPage returns no
image for one. Such a page reports that it could not be rendered instead of printing a
blank or distorted sheet.

Each band takes a short document hold for rendering, and the first band's hold also
reads the page size. Each hold ends before colour conversion, painting or any printer
call. The worker converts a band's colour mode before it draws the band. It begins the
job or starts a sheet only after the sheet's first band converted, so a failed render
or conversion reports its page instead of leaving a blank sheet. That band is drawn
even when Stop arrives while the printer starts the sheet. A blocked printer call
names its printer in the wait bar. Document::renderPage gives MuPDF no cookie, so a
band that is rendering when Stop arrives runs to its end under its hold, and the tab
stays covered until then, as for other jobs that hold the document. Then document
access ends, no further band renders, and completion is suppressed. A print
completion already submitted may finish. The worker checks painter.end and reports a
completion failure.

Qt cannot cancel a print job, because QPrinter::abort does nothing in Qt's PDF, CUPS
and Windows engines. When Stop or a failure ends a printer job early, the painter
still ends it, and the printer receives the sheets drawn so far. The band count
follows the visible raster at the render dpi, so an A4 or Letter page at 600 dpi
already prints in two or three bands. Stop or a failure between the bands of a page
leaves that sheet partly drawn, and its drawn top strip can be blank. Print to file
discards its private PDF.

The print preview renders through RenderEngine with the same raster plan, at screen
resolution for colour and grayscale, and like printing for black and white, at the
print quality without antialiasing. It lowers the resolution so that the visible part
stays within 2048 or 4096 pixels on a side. It shows that it could not render the
page when even that raster is beyond 2^20 pixels, or when the plan at the print quality
is, so it predicts the pages that printing refuses. It keeps the colour conversion and
its reduction to the device size between paints, and redoes them only for a new
render, colour mode or device size.

Print to file reserves and checks the destination against source and open tabs before
output begins. QPrinter writes a private local PDF, then closes before the worker
replaces the destination through DocumentOutput. Stop before that guarded replacement
keeps the destination's previous bytes. Closing tabs or windows commits active
editors and offers Save, Discard, or Cancel for unsaved changes.

Page deletion, extraction, merge, split, and rotation preserve editable measurement
metadata. Page numbers follow output order (including duplicates); rotation transforms
points and swaps anisotropic manual scales on quarter turns. Merged documents use the
display defaults of the first contributing measurement set.

`AtomicPdfWriter` streams all qpdf output through `QSaveFile`, including page,
security and measurement operations. A failed write discards its temporary file;
only a complete write replaces the destination. A split commits each output file
individually. `AtomicPdfWriter` and `DocumentOutput::replace` hold the destination's
`job::DestinationLock` from opening the temporary file to the end of the commit, and
a stop before commit admission discards the temporary file. An admitted commit can
still finish after Stop (see Disk access off the UI thread). A split stopped part
way keeps its committed files. Flattening copies inherited resource/font
dictionaries and allocates a unique label font name, preserving existing page fonts and sibling resources.
The Rotate prompt uses the same validated page-range grammar as Print, Extract and
Delete.

`ZoomAnimation` owns visual interpolation state, timing and overlay transforms;
`ViewerWidget` captures viewport rectangles and coordinates layout and repainting.
Comfort workers reuse their region plan until an image rectangle starts or ends;
each worker has its own mutable cache. Rectangle priority and pixel treatments are
unchanged.

The October 2026 improvement pass covered lifetime/save correctness, asynchronous
search/OCR, indexed viewport lookup, settings persistence, component extraction,
and a source-wide comment audit. Validation combines required synthetic regression
tests on Linux/Windows, Linux ASan/UBSan, and explicit public-corpus profiling.
The benchmark procedure and its limits are recorded in `BUILDING.md`.

## Persistence

Primary application files live in one per-user directory:

- Windows: `%APPDATA%/MervinPDF`
- Linux: `$XDG_CONFIG_HOME/mervin-pdf`, normally `~/.config/mervin-pdf`

The files are:

| File or directory | Purpose |
| --- | --- |
| `config.toml` | settings and saved window state |
| `recent.json` | recent paths, timestamps, page counts, and favourites |
| `viewstate.json` | page, zoom, rotation, and scroll state per path |
| `session.json` | currently open files, active document, and files kept while their drive is unavailable |
| `recovery/` | edited document snapshots and metadata for unloaded tabs and in-place Save |
| `tessdata/` | writable OCR language models |
| `downloads/` | PDF downloads when no normal download directory is available or a profile is active |

Paths are normalized before deduplication and lookup, with case folding on Windows.
Corrupt or missing state files fall back to defaults instead of blocking startup.

The directory can be on a network share, as with a roaming or redirected Windows
profile or a home folder on NFS. After startup, settings, store and recovery file
work runs in the background. Open from URL still creates and writes its download
directory on the UI thread, including when a profile or fallback places it here.
Startup reads the settings, the three stores and the recovery manifests before the
first window, which needs them; `ConfigPaths` creates the folders then, once. Later
`Settings::load()` returns the values in memory, and `appfiles::write`
(`io/AppFiles`) saves the settings and stores on a background thread through
`QSaveFile`, in the order asked. A newer write of a file drops a write of it that
has not started and that nobody waits for. Closing a tab removes its recovery files
the same way, the copy only once its document has closed on a background thread, and
tabs restored lazily adopt the recovery manifests startup found. Ordinary quitting waits up
to 5 s in all for those closes and then the writes before `WindowManager` lets go of
the single-instance lock, so the next copy reads them. With a profile folder that
stopped answering, the last writes land when it answers again or never, the process
says so on stderr, and a restart asked for (a new UI language) is skipped, as the
new copy would read the files without them. The Settings window and
the first-run window keep their question open when a save fails, so they wait up to
5 s for their own write, in its turn among the others. A write still waiting after
that lands later and counts as saved. The values in effect are the running
process's, so an edit made to `config.toml` by anything else while Mervin runs is
overwritten by its next save.

In-place Save and edited unload write their private PDF and version-1 manifest on
their worker. They set owner-only permissions and commit the manifest through
guarded `QSaveFile` before releasing the original document. These writes must
finish before adoption, so they do not use the application's serial background
write queue. The UI claims the manifest only after committing it. Stop or selection
before unload adoption schedules removal of the job's PDF and manifest after its
handles close. Committed recovery also waits for the session outcome and remains
discoverable after successful session end, as described above. After adoption, the
tab owns its recovery until Save succeeds or the user closes the tab, with the same
session outcome gate on removal. An abandoned loader
or an earlier stopped writer can still read a recovery input, so removal waits
for both readers and writers without blocking the UI. A superseded unload
checkpoint keeps its manifest until its readers close and its PDF is removed.

Settings saves merge only fields changed since that window's last load/save, preventing
stale windows from overwriting newer preferences, and commit through `QSaveFile`.
Every save writes all known keys and drops keys this version does not read. A default
that should also reach existing profiles therefore needs a new key, as `auto_update`
and `recent_keep_removed` have.
Closing a window batches view-state updates into one write.
Recent history kept (`recent_retention`) caps both stores. `RecentStore` keeps that
many of the newest entries that are not favourites, plus every favourite.
`ViewStateStore` keeps that many of the most recently updated view states of files
that are not favourites, plus every favourite's, which `WindowManager` marks
(`ViewStateStore::setKept`). A starred file therefore never loses its place or its
view to age. An entry that loses its star, and its view state, count again from
the next trim.
When the Recent setting to keep deleted or moved files is off, `WindowManager` has
`FileProbe` check the recent paths whenever the list is shown and drops entries
whose file is gone while its volume is attached (`FileFacts::removed`), together
with their view state, in one save. Clear missing files checks its entries again
first, so a file back in the meantime stays. The Recent page never touches the disk
itself. Its rows show the missing state and the file size from the probe's answers,
and a row shows no size and counts as there until its check answers. Rows are
checked again when listed after their answer is two seconds old. The Locate dialog
for a missing file starts in its folder only when a check found the folder.

On Windows the volume is the drive or UNC share. Elsewhere a GVFS path counts while
its share folder exists. Otherwise `RecentStore::isVolumeAttached` reads the mounted
file systems from `/proc/self/mountinfo` and the configured mount points from
`/etc/fstab`, and `mounts::holdingMount` (`recent/MountTable`) decides. The deepest
mounted file system above the path holds it and must answer `statvfs`, since a
network share can stay mounted after its server went away. The volume is away when
an fstab mount point or an automount (`autofs`) point lies between that file system
and the path, or when the path is under `/media`, `/run/media`, `/mnt` or `/net` with
nothing mounted below that folder. While such a volume is unmounted, Recent keeps
every file on it, and session restore keeps one in its top folder, where the empty
mount point is left behind. Recent and the session store canonical paths, and the
kernel lists mount points without links. The fstab folders are compared as written,
since resolving links in them would look inside other volumes and could mount an
unrelated automount point. A line written through a link, such as `/mnt/data` where
`/mnt` links to `/var/mnt`, is therefore not matched. The mount list is read as text
because `QStorageInfo::mountedVolumes()`, used only when mountinfo cannot be read,
queries every volume, which stalls on an offline share and mounts every automount
point.

The mount list cannot tell the empty folder of a share mounted by hand outside
fstab, such as an sshfs share, from a folder whose file was deleted. So when a file
is opened and is there, `WindowManager::recordOpen` stores the mount point of the
file system that holds it with its Recent entry (`RecentEntry::mount`, `"m"` in
`recent.json`), read from mountinfo only. A file opened while it is missing, such
as a crash recovery tab opened lazily, keeps the mount recorded before. For an entry
with a recorded mount, the volume is also away while another file system holds the
path and that folder still exists. A recorded mount never makes a volume attached
that the mount list calls away. A folder that went with its volume, as udisks and
per-login mounts such as KDE's kio-fuse leave it, falls back to the mount list.
Outside `/media`, `/run/media`, `/mnt` and `/net` that finds the file deleted, so
pruning drops it. Only the folder is recorded, so another volume mounted on the
same folder counts as the file's volume, unless that folder is `/media`,
`/run/media`, `/mnt` or `/net` itself.
`recordOpen` keeps the recorded mount at first and has `FileProbe` check the file;
the mount it is on now replaces it once the check finds the file there. An entry
without a recorded mount, such as one an older version wrote, gets the mount from
the first check of any kind that finds its file there, such as the Recent page's
own checks or a prune pass. `WindowManager::onFileChecked` collects those mounts.
It saves `recent.json` and announces the list once when the prune pass that found
them has its last answer, and otherwise `kMountSaveMs` (half a second) after the
first one, on a timer that later finds do not restart. `FileProbe` answers one file
at a time, so a pass on a slow share saves at most twice a second rather than once
per file. An ordinary quit saves the mounts still waiting, before the final write
of the app's files. Such a check never replaces a recorded mount.
`recordOpen` gives the mount to the session too, which saves it for each open or kept
document in `"mounts"` of `session.json`. Restore therefore still has it after Recent
trims or clears the file. For a session saved before that field existed, restore
takes the mount from Recent and keeps it from then on. A missing file under a
folder that is its recorded mount point, and is no longer used as one, stays listed
as away until it is cleared by hand or opened again. An entry from an older version
has no recorded mount until a check finds its file. Until then, a share mounted
outside fstab that holds the file is not recognised while it is unmounted.

`SettingsDialog` edits a copy of the settings. It emits a request for a manual update
check, and on Apply, or OK with changes pending, a request to apply the edited copy;
only its OCR language list changes files, through the OCR folder's thread. `MainWindow::applySettings` saves
that copy, re-applies viewing defaults to the open document only when they changed,
and broadcasts theme and annotation defaults through `WindowManager`.
The last update check's UTC timestamp and the pending download's version, path and
SHA-256 use `QSettings`; the downloads themselves live in a machine-local `updates/`
folder (`%LOCALAPPDATA%\MervinPDF` on Windows, `$XDG_CACHE_HOME/mervin-pdf` on Linux,
or inside the profile). Copies of different package kinds run by one user, such as an
AppImage beside the .deb install, share both, so each kind keeps its state under
`update/<kind>/` (for example `update/appimage/pendingFile`) and its downloads in
`updates/<kind>/`, named by `update::packageKindName`. A copy reads, installs and
deletes only its own kind's state and files. Earlier versions kept one shared
record under `update/` and the downloads in `updates/` itself. A copy still resumes
and cleans up such a record and such files when the file name is its kind's package
(`update::isPackageFileName`), leaves other kinds' alone, and uses the shared check
date until it has its own. Before installing, the updater hashes the file again and
discards it if the SHA-256 no longer matches. The check that found that download then
does not count. `update/<kind>/checkDue` makes the next start check again, and the
next completed check clears it. A record from an older version has no hash and is not
checked again. `Updater::install` also refuses a file whose name is
not this kind's package. At each start, also with automatic updates off, an installed
copy deletes its kind's downloads that are no longer newer than itself. A response
to the release check that is not a JSON object with a string `tag_name`, such as a
network filter's page sent with HTTP 200, does not count as a check. `Updater`
reads and writes this state and the downloads, streams a download to disk, and finds
the package manager or replaces the AppImage on one background thread, in the order
asked, and each step of a check, download or installation goes on when its disk work
answers. A step whose operation ended meanwhile, as when automatic updates were
switched off, is dropped.
The last check date is read when the updater starts and kept in memory, so Settings
shows it without reading the profile. Handing off to the Windows installer consumes
the pending update first, then closes the windows, starts the installer and quits in
one step, as a quit that came later would let an open dialog's event loop delete the
closed windows under it. The handoff goes ahead only once the consumed state is
written. Quitting waits for the updater's writes, such as an update put back after
the installer did not start, within the same 5 s it gives the app files. An automatic
installation checks again for other open dialogs after each disk step and waits for
them before it closes any window. Windows file-handler registration uses the per-user registry. A profile redirects the Qt
settings as well as the files above.

## Theme and icons

All themed application-chrome and viewer-surface colors are defined in
`src/ui/ThemeTokens.cpp`. `Theme` builds the application stylesheet from those
tokens and the selected system or custom accent. Colors written into PDF content and
the compile-time Comfort transform ramp are deliberate data-level exceptions.
Document color transforms are separate from application chrome so page appearance
can be changed independently. On Windows, the native title bar uses the system's
default caption and text colors in both UI themes.

The dark theme is "Nord", lifted blue-grey surfaces chosen for comfort in long
sessions: no near-black wells, body text at about 9.4:1 and a soft frost accent
that carries dark text. The light theme is "Cool slate", cool blue-grey bands with
white controls around the same slate canvas. Both themes install a matching
`QPalette` for native widgets, carrying the accent. Explicit dark and light
preferences take precedence even when the platform cannot force its colour
scheme. While the accent setting is "system", `Theme` reads the OS accent from
the platform palette in either theme,
and again whenever the platform reports a theme change or replaces the palette,
as Plasma does. Windows 11 reports a lighter shade of it in dark mode; Qt's GTK
theme and its built-in KDE theme report it only as the selection colour. A role
that still holds Fusion's #308cc6 came from Qt rather than the desktop, as on
Linux without a desktop platform theme, and counts as no accent. The OS accent
is lightened in dark or darkened in light, keeping its hue, until it reaches the
3:1 the design accents keep on the window and in the well. The design accents are
the fallback when the desktop reports no accent. The custom accent Reset value
is `#62c9ff` in both UI themes. Turning off the system accent keeps the resolved
system colour. Text on an accent fill is white or the theme's dark ink, whichever
has more contrast. The wait bar over a busy tab is pale yellow in light. In dark it
takes the popover surface with a yellow line along its bottom edge, so the page
under its veil stays easy to read.
`tst_theme` checks each theme's key text pairs against WCAG contrast targets.

The interface icons are [Lucide](https://lucide.dev) SVGs, vendored unmodified in
`resources/icons/lucide` and compiled into the `mervin_icons` library as a Qt
resource. `ui/Icons` renders them through Qt SVG in the theme's ink, so Windows and
Linux show identical artwork without depending on a platform icon font; the
stylesheet's check marks, close crosses and arrows are rendered from the same set.
Those are PNGs in memory, published as Qt resources (`io/MemoryFiles`) under a new
root for each theme, because the style sheet engine loads `image:url()` files on the
UI thread as it paints.
Icons use a 1.5-unit stroke on Lucide's 24-unit grid, which is one logical pixel in
the menus' 16 px icons. The toolbar's 20 px icons keep that one-pixel line, 1.2
units, so they match the menus at every display scale. Each icon is drawn from its
SVG at the exact device size it is shown at, so 125%, 150% and 175% scaling get
native renders rather than a larger render shrunk to fit. No render draws a line
thinner than about 1.07 device pixels. At one pixel or less Qt's raster engine uses
its hairline stroker, which drops the dots in icons such as Outline, About and
Keyboard shortcuts. A 16 px icon at 100% scaling therefore gets about 1.6 units,
and a 20 px one about 1.3. The Document menu's Split and Merge icons are Lucide's
split and merge turned a quarter clockwise, so their arrows point right.
Extract Pages mirrors Lucide's file-output horizontally, so its arrow points
right out of the document and its page fold moves to the upper-left corner.
Save Page As uses the same mirrored icon.

Fit page and Fit width have separate toolbar buttons, each selecting its mode
without toggling. Their icons overlay a centered directional arrow at 65% scale
inside Lucide's square, with the same stroke weight on both layers. The menu uses
the same actions and icons. Ctrl+1 selects Fit page, Ctrl+2 selects Fit width,
and Home continues to toggle between them, selecting Fit page from custom zoom.
The fit actions are disabled while the Recent view is active or no document is open.

The application and packages share the repository's approved icon assets.

### Application icon

The default is [mervin-icon.ico](../resources/icons/mervin-icon.ico), with matching
[SVG](../resources/icons/mervin-icon.svg) and
[PNG](../resources/icons/mervin-icon.png) assets. This is the selected
`mervin-p06-raised-p` design, renamed for application use. It retains the folded
blue page and original gradient, with the longer P stem and raised letter. The
page fills the icon's full height.

The original gradient is used at every size. Qt windows, dialogs, the taskbar,
and Recent files use the approved PNG frames embedded in the application.
Windows executable resources, installers, shortcuts, and PDF associations use
the same ICO. The Microsoft Store package uses the same frames as unplated
target-size images, so its taskbar, Start menu and file icons match the other
builds. Linux packages and AppImages install the PNG frames and scalable SVG
under the existing `mervin-pdf` desktop icon name.

See the [icon asset notes](../resources/icons/README.md) for geometry, export
sizes, and verification.

## Platform integration and networking

Windows integration registers a per-user PDF handler and opens the system Default
Apps page; Windows still requires the user to confirm the association. Linux
packages install a desktop entry, MIME metadata, and icon, leaving the default-app
choice to the desktop environment.

The application has no telemetry and does not upload documents. Networking is
limited to explicit URL opens and downloads, the OCR model catalog and selected
model downloads, and update checks/downloads.

## Build and verification

The project supports Windows x64 and Linux x86-64. CMake builds the targets; the
platform release pipeline adds a Windows MSI installer and Linux
AppImage/DEB/RPM artifacts. Windows uses Qt 6.12.0; Linux requires Qt 6.9 or newer
for the supported translation toolchain.
MuPDF 1.28.5 is built from source with OCR support.

QtTest targets cover the core stores, IPC, rendering helpers, document tools,
dialogs, layout, theme, and platform-sensitive behavior. Performance targets measure
rendering and startup separately. Windows contributor setup and exact commands live
in [BUILDING.md](BUILDING.md).


Required tests generate synthetic PDFs in the build directory. The optional photographic
reference corpus remains local and is labeled `optional-corpus`; CI excludes it and
fails on skips in every required target. A case that applies to one platform is
compiled out on the others instead of skipped. Linux Release and sanitizer jobs and Windows
Release jobs build and run the tests. `tst_perf_layout` compares indexed lookup against
a full scan on 10,000 pages and reports median and 95th-percentile lookup time.
