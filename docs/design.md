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
configuration. The application is licensed under AGPL-3.0; dependency notices are
maintained in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

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
  ipc/           process lock, local socket, and message framing
  merge/         merge-plan model
  net/           safe URL request and download helpers
  ocr/           language-model discovery and validation
  platform/      Windows and Linux integration
  print/         print-range parsing
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

Two places still follow a position, and only the tab order can come out wrong. A
move within the bar is `QTabBar`'s own and follows the pressed position, so a tab
that arrives while the button is held makes it move the tab now at that position.
`MainWindow::openFile` inserts at the position its caller worked out before the
password prompt of an encrypted file, so a tab that arrives during the prompt can
shift the new tab by one.

Removing a tab later, from a background answer, waits until the window can change
its tabs (`MainWindow::canChangeTabs`). That means no modal dialog or popup in the
application, no document operation or unload in the window, no mouse button held and
no tab drag. A modal dialog in one window therefore holds back changes in the others
too. `MainWindow::whenTabsCanChange` queues such a change and never runs it inside
the call. The window runs its queue in order on the next turn of the event loop,
after a mouse button release, after a modal window closes, when a document operation
ends (`MainWindow::OperationScope`) and on the one-second activity tick. A change
finds its tab and window again when it runs. Once a quit or restart closes a window,
it runs no more changes, and those still queued go with it. Adding a tab is not
gated. A tab's operation that has ended shows its questions and errors through this
queue too, in the window that holds the tab by then (see Page and security
operations).

### Disk access off the UI thread

A file call on a drive or network share that stopped answering blocks its thread
until the system gives up, which can take minutes or never happen on a hard NFS
mount, and nothing can cancel it. The UI thread therefore hands disk work to
`background::start` (`io/Background`) and never waits for it. Results come back
through `background::post`, which drops them if the object that asked is gone.
Nothing joins these threads. Closing a window or quitting leaves stuck work behind,
and the process ends without running static destructors while any is left
(`main.cpp`). A stuck call keeps its thread, so
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
has open is removed only after it is closed, as Windows cannot remove an open file,
while its manifest goes at once so it is never offered again. Quitting waits for
those removals within the time it gives the app's own files (see Persistence).
MuPDF's Tesseract runs one recognition at a time in a process, so a canceled OCR
still finishing makes the next one wait its turn on its background thread. At exit,
`main.cpp` flushes only standard output and error before leaving with stuck work
behind, as flushing every C stream would wait for one that a stuck read of a document
holds.

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
in a file call cannot be stopped, so a stopped job gives up at its next step and never
replaces a file. A writer asks `job::beginCommit()` right before
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
- Print, Delete Pages, Extract Pages, merge, and security. The chosen design
  shows such a wait in its tab, with a bar over its locked document, so other
  tabs and windows stay usable. New tabs and unloaded tabs already load in their
  placeholders, and Rotate Pages and Split run as tab jobs (see Page and security
  operations).
- Reads at startup before the first window, and the system file dialogs, which
  run on the UI thread by design.
- Opening a folder or a link with the system's handler (`QDesktopServices::openUrl`),
  which on Windows waits for `ShellExecute`.
- Two small updater steps: telling how this copy was installed when the updater
  starts (a registry read on Windows, a check that `$APPIMAGE` is a file on Linux),
  and starting the Windows installer, whose working folder is the updates folder.

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

The first process acquires a per-user `QLockFile` and starts a `QLocalServer`.
Subsequent launches send newline-delimited JSON containing file paths and open
behavior, wait for an acknowledgement, and exit. The primary process acknowledges
before opening files so a password or error dialog cannot make the sender time out.

If the primary process disappears during handoff, the new process retries ownership
and can fall back to a standalone window. Session restoration creates the tab list
but opens only the selected document. Other restored tabs load when selected.

The first window is shown before any document is checked, and command-line files
create their placeholders at once. Staged batches finish when their shells are queued,
so a command-line read that stopped answering cannot delay session restore. The
development-only `--quit-after-startup` option separately waits for requested loads
after both shell batches are queued, so startup timing still measures usable documents.
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
that is there opens as an unloaded tab in its saved place, once the session's other
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
change at once through `LanguageChange`, and it acknowledges launches that arrive
meanwhile and opens their files with the first main window.

The first-run window also offers **Download OCR**, checked by default. Continue
saves the display language before scheduling one background download attempt for
each matching OCR model. This first-run choice takes effect without a restart.
Closing the first-run window keeps the display language but skips the downloads.

The other windows never retranslate. When Settings picks a different language,
OK or Apply saves it and `WindowManager::restart()` closes every window as Quit
does, so unsaved documents still prompt and cancelling one cancels the restart.
`relaunch` then starts the new copy from `main()` once the single-instance server
is gone. On Linux it waits until this process has exited. From an AppImage it starts
the AppImage file, and a `--profile` run keeps its profile.

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
close question retries that read asynchronously, keeping the recovery copy. Stop
while waiting for that Save prerequisite cancels the close. Explicit Save runs on
a worker and can save an unloaded tab directly from its recovery copy. Operations
that need a loaded document remain unavailable until its load finishes.

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

Rotate Pages and Split run as their tab's operation (`TabPage::startOperation`), so
only that tab waits for the file. Their prompts and file dialogs run first, as before.
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

`MergePlan` and `ExtractPlan` are the GUI-free models behind the Merge PDFs and
Extract Pages dialogs, tested in `tst_merge_plan` and `tst_extract_plan`; each dialog
only renders its plan and writes edits back to it. Both dialogs show their rows in
the same reorderable `RowList`. Extract writes through `PageOps::merge`, which fails
on an out-of-range page instead of skipping it, so a stale page count cannot silently
shorten the output.

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
checks Save as again and writes once that answers, if the rows and Save as are still
what it was pressed for. A default name that stepped past a file in use meanwhile
still counts, as the user did not choose it.

Delete Pages is the same `ExtractDialog` and `ExtractStrip` over an `ExtractPlan` in
`Mode::Delete`, not a separate dialog. The rows, Save as, the password row, writing
before closing and the keyboard handling are shared, and the mode changes only what
differs. Its rows are a set of pages to leave out, so `RowList` drops the grips and
the Output column and there are no moves. Nor is there a Remove button beside the
rows, since each row's ✕ removes it. `ExtractPlan::cells()` returns every
source page, unfolded, flagged `deleted`, and `deletePage()`/`keepPage()` turn a
click on the strip into row edits, widening, joining or splitting ranges. The dialog writes through
`PageOps::deletePages`, which copies the whole document minus those pages, so the
copy keeps the source's structure and encryption, and refuses to delete every page.

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
and clears the recovery files. Failure or Stop loads the private copy under the
original logical path and keeps it unsaved. A failed recovery load keeps its
manifest and Retry placeholder. Save from an unloaded edited tab copies its
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

After the dialog, the worker owns the QPrinter, begins and ends its QPainter, and
renders each page with Document::renderPage on the document's own context. A short
document hold covers rendering and page geometry, then releases before painting or
calling the printer system. A blocked printer call names its printer in the wait bar.
Stop aborts further pages and suppresses completion. A print completion already
submitted may finish. The worker checks painter.end and reports a completion failure.

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
in a stopped job they discard the temporary file instead of replacing the destination
(see Disk access off the UI thread). A split stopped part way keeps the files it
committed before. Flattening copies inherited resource/font dictionaries and allocates
a unique label font name, preserving existing page fonts and sibling resources.
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
profile or a home folder on NFS, so after startup the UI thread does not touch it.
Startup reads the settings, the three stores and the recovery manifests before the
first window, which needs them; `ConfigPaths` creates the folders then, once. Later
`Settings::load()` returns the values in memory, and `appfiles::write`
(`io/AppFiles`) saves the settings and stores on a background thread through
`QSaveFile`, in the order asked. A newer write of a file drops a write of it that
has not started and that nobody waits for. Closing a tab removes its recovery files
the same way, the copy only once its document has closed on a background thread, and
tabs restored lazily adopt the recovery manifests startup found. Quitting waits up
to 5 s in all for those closes and then the writes before `WindowManager` lets go of
the single-instance pipe, so the next copy reads them. With a profile folder that
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
before unload adoption removes the job's PDF and manifest after its handles close.
After adoption, the tab owns them until Save succeeds or the user closes the tab. An abandoned loader
or an earlier stopped writer can still read a recovery input, so removal waits
for both readers and writers without blocking the UI. A superseded unload
checkpoint keeps its manifest until its readers close and its PDF is removed.

Settings saves merge only fields changed since that window's last load/save, preventing
stale windows from overwriting newer preferences, and commit through `QSaveFile`.
Every save writes all known keys and drops keys this version does not read. A default
that should also reach existing profiles therefore needs a new key, as `auto_update`
and `recent_keep_removed` have.
Closing a window batches view-state updates into one write.
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
per-login mounts such as KDE's kio-fuse leave it, falls back to the mount list. Only
the folder is recorded, so another volume mounted on the same folder counts as the
file's volume, unless that folder is `/media`, `/run/media`, `/mnt` or `/net` itself.
`recordOpen` keeps the recorded mount at first and has `FileProbe` check the file;
the mount it is on now replaces it once the check finds the file there.
`recordOpen` gives the mount to the session too, which saves it for each open or kept
document in `"mounts"` of `session.json`. Restore therefore still has it after Recent
trims or clears the file. For a session saved before that field existed, restore
takes the mount from Recent and keeps it from then on. A missing file under a folder that was a mount point when the
file was opened, and is no longer used as one, stays listed as away until it is
cleared by hand or opened again. An entry from an older version has no recorded
mount, so on a share mounted by hand it is not recognised once the share is
unmounted.

`SettingsDialog` edits a copy of the settings. It emits a request for a manual update
check, and on Apply, or OK with changes pending, a request to apply the edited copy;
only its OCR language list changes files, through the OCR folder's thread. `MainWindow::applySettings` saves
that copy, re-applies viewing defaults to the open document only when they changed,
and broadcasts theme and annotation defaults through `WindowManager`.
The last update check's UTC timestamp and pending download use `QSettings`; the downloads
themselves live in a machine-local `updates/` folder (`%LOCALAPPDATA%\MervinPDF` on
Windows, `$XDG_CACHE_HOME/mervin-pdf` on Linux, or inside the profile). `Updater`
reads and writes them, streams a download to disk, and finds the package manager or
replaces the AppImage on one background thread, in the order asked, and each step of
a check, download or installation goes on when its disk work answers. A step whose
operation ended meanwhile, as when automatic updates were switched off, is dropped.
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
