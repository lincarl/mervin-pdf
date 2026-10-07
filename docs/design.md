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

MuPDF is built from source and linked statically. Qt and qpdf are dynamically linked
in packaged builds. The application is licensed under AGPL-3.0; dependency notices
are maintained in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

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
  recent/        recent-file and per-file view-state stores
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
writer of the shared recent, view-state, and session files. The render engine
outlives every open `Document`. Queued jobs hold a shared lifetime gate:
document destruction clears its pointer under the gate lock, waiting only for active
document access. Rasterization of an independent display list can finish afterward.

The explicit Duplicate to new window command bypasses normal path deduplication and
creates a second independent view.

Each `TabPage` groups one tab with its viewer, document when loaded, and retained
state when unloaded. Tab detachment and merging reparent this tab object rather
than reopening the file.

## Startup and single-instance behavior

The first process acquires a per-user `QLockFile` and starts a `QLocalServer`.
Subsequent launches send newline-delimited JSON containing file paths and open
behavior, wait for an acknowledgement, and exit. The primary process acknowledges
before opening files so a password or error dialog cannot make the sender time out.

If the primary process disappears during handoff, the new process retries ownership
and can fall back to a standalone window. Session restoration creates the tab list
but opens only the selected document. Other restored tabs load when selected.

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
`i18n/mervin_<id>.ts`, named by locale ID (`sv`, `zh_CN`) or the explicit
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
language and script, preferring the same territory, so zh-HK would pick a zh_TW
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
direction from the language. For Simplified Chinese, Japanese and Korean, it adds
an installed font for that language as the fallback for Han text and the relevant
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

Before releasing a document with unsaved edits, the tab writes a local recovery
snapshot with its forms, annotations, measurements, and manual scales. It retains
the original logical path and dirty state, and the snapshot preserves PDF encryption.
Passwords are held only in memory, never in the recovery metadata or settings.
Snapshot failure leaves the live document available. Reload failure keeps the tab
and saved recovery state for retry instead of discarding them.

The shared MuPDF store and allocator can retain memory after individual objects are
released, so process resident memory is not the sum of the live tab caches.

## Rendering and document access

`RenderEngine` owns one base MuPDF context and a small worker pool. Each worker uses
a cloned context, which is MuPDF's supported multithreaded pattern. Requests are
processed newest-first so pages that just became visible take priority.

A single MuPDF document handle is not safe for concurrent access. `Document`
therefore provides an access mutex used while loading pages, parsing content,
extracting text, reading geometry, or changing PDF objects. Workers build display
lists while holding that mutex, then rasterize the independent lists in parallel.

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

Recent-file content search runs on its own worker and opens files independently. It
returns the first matching page and a short snippet for each file. The Recent page's
All scope lists file-name matches at once and scans only the remaining files. Generation tokens are checked when queued results reach the UI.
A persistent worker takes the newest pending query; cancellation never joins on the
UI thread, except when the service is destroyed.

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
the official `tessdata_best` catalog from GitHub, validates downloaded model files,
and stores them in the writable per-user tessdata directory. The OCR dialog displays
and edits the result, while the caller retains the selected page rectangle so a
language change can submit the recognition again. OCR captures a display list under the
document lock, then recognizes on a private context in a serial background worker.
Closing the popup or changing languages invalidates results and signals Tesseract cancellation.

Installers include the OCR engine but no language models. When enabled in the
first-run window, `OcrProvisioner` downloads the official `tessdata_best` files for
the selected display language and the OS's primary display language. The fixed
`OcrLanguageMapping` table matches language and script, deduplicates model codes,
and skips unsupported languages without a fallback download. Bokmål and Nynorsk
both use `nor`; Montenegrin and Romansh have no matching model. The selected
language's model becomes the initial OCR default, or the OS model when only that
one matches.

Saving `ui_language` consumes the first-run offer before any network request.
Downloads fail silently, stop after 30 seconds without data, and are cancelled
on application exit. There is no retry or resume on a later launch, including
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

`MergePlan` and `ExtractPlan` are the GUI-free models behind the Merge PDFs and
Extract Pages dialogs, tested in `tst_merge_plan` and `tst_extract_plan`; each dialog
only renders its plan and writes edits back to it. Both dialogs show their rows in
the same reorderable `RowList`. Extract writes through `PageOps::merge`, which fails
on an out-of-range page instead of skipping it, so a stale page count cannot silently
shorten the output.

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

In-place Save closes the source handle and replaces it through `QSaveFile`, with
direct-write fallback disabled. A failed replacement opens the staged edited snapshot
under the original logical path; it remains dirty and can be saved again. If recovery
also fails, the snapshot is retained on disk and its location is reported. Saving a
file open in another view requires closing that view or choosing Save as Copy.

Save as Copy and measured export prepare output before atomically replacing the
destination. Printing a document with measurements uses a complete flattened snapshot
and reports preparation or rendering failures. Closing tabs or windows commits active
editors and offers Save, Discard, or Cancel for unsaved changes.

Page deletion, extraction, merge, split, and rotation preserve editable measurement
metadata. Page numbers follow output order (including duplicates); rotation transforms
points and swaps anisotropic manual scales on quarter turns. Merged documents use the
display defaults of the first contributing measurement set.

`AtomicPdfWriter` streams all qpdf output through `QSaveFile`, including page,
security and measurement operations. A failed write discards its temporary file;
only a complete write replaces the destination. A split commits each output file
individually. Flattening copies inherited resource/font dictionaries and allocates
a unique label font name, preserving existing page fonts and sibling resources.
Rotate/Delete prompts use the same validated page-range grammar as Print/Extract.

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
| `session.json` | currently open files and active document |
| `recovery/` | edited document snapshots and metadata for unloaded tabs |
| `tessdata/` | writable OCR language models |
| `downloads/` | PDF downloads when no normal download directory is available or a profile is active |

Paths are normalized before deduplication and lookup, with case folding on Windows.
Corrupt or missing state files fall back to defaults instead of blocking startup.
Settings saves merge only fields changed since that window's last load/save, preventing
stale windows from overwriting newer preferences, and commit through `QSaveFile`.
Closing a window batches view-state updates into one write.
When the Recent setting to keep deleted or moved files is off, `WindowManager` checks
the recent paths on a worker thread whenever the list is shown and drops entries
whose file is gone while its volume is attached (`RecentStore::isRemovedFromDisk`:
the drive or UNC share on Windows, a mount below `/media`, `/run/media`, `/mnt` or
`/net`, or a GVFS share folder elsewhere), together with their view state.
`SettingsDialog` edits a copy of the settings. It emits a request for a manual update
check, and on Apply, or OK with changes pending, a request to apply the edited copy;
only its OCR language list changes files directly. `MainWindow::applySettings` saves
that copy, re-applies viewing defaults to the open document only when they changed,
and broadcasts theme and annotation defaults through `WindowManager`.
The last update check's UTC timestamp and pending download use `QSettings`; the downloads
themselves live in a machine-local `updates/` folder (`%LOCALAPPDATA%\MervinPDF` on
Windows, `$XDG_CACHE_HOME/mervin-pdf` on Linux, or inside the profile). Windows
file-handler registration uses the per-user registry. A profile redirects the Qt
settings as well as the files above.

## Theme and icons

All themed application-chrome and viewer-surface colors are defined in
`src/ui/ThemeTokens.cpp`. `Theme` builds the application stylesheet from those
tokens and the selected system or custom accent. Colors written into PDF content and
the compile-time Comfort transform ramp are deliberate data-level exceptions.
Document color transforms are separate from application chrome so page appearance
can be changed independently.

The dark theme is "Nord", lifted blue-grey surfaces chosen for comfort in long
sessions: no near-black wells, body text at about 9.4:1 and a soft frost accent
that carries dark text. The light theme is "Cool slate", cool blue-grey bands with
white controls around the same slate canvas. Both themes install a matching
`QPalette` for native widgets, carrying the accent. While the accent setting is
"system", `Theme` reads the OS accent from the platform palette in either theme,
and again whenever the platform reports a theme change or replaces the palette,
as Plasma does. Windows 11 reports a lighter shade of it in dark mode; Qt's GTK
theme and its built-in KDE theme report it only as the selection colour. A role
that still holds Fusion's #308cc6 came from Qt rather than the desktop, as on
Linux without a desktop platform theme, and counts as no accent. The OS accent
is lightened in dark or darkened in light, keeping its hue, until it reaches the
3:1 the design accents keep on the window and in the well. The design accents are
the Reset value and the fallback when the desktop reports no accent. Text on an
accent fill is white or the theme's dark ink, whichever has more contrast.
`tst_theme` checks each theme's key text pairs against WCAG contrast targets.

The interface icons are [Lucide](https://lucide.dev) SVGs, vendored unmodified in
`resources/icons/lucide` and compiled into the `mervin_icons` library as a Qt
resource. `ui/Icons` renders them through Qt SVG in the theme's ink, so Windows and
Linux show identical artwork without depending on a platform icon font; the
stylesheet's check marks, close crosses and arrows are rendered from the same set.
Icons use a 1.5-unit stroke on Lucide's 24-unit grid, but no render draws a line
thinner than about 1.07 device pixels: at one pixel or less Qt's raster engine uses
its hairline stroker, which drops the dots in icons such as Outline, About and
Keyboard shortcuts. A 16 px icon at 100% scaling therefore gets about 1.6 units.
The application and packages share the repository's approved icon assets.

### Application icon

The default is [mervin-icon.ico](../resources/icons/mervin-icon.ico), with matching
[SVG](../resources/icons/mervin-icon.svg) and
[PNG](../resources/icons/mervin-icon.png) assets. This is the selected
`mervin-p06-raised-p` design, renamed for application use. It retains the folded
blue page and original gradient, with the longer P stem and raised letter.

The original gradient is used at every size. Qt windows, dialogs, the taskbar,
and Recent files use the approved PNG frames embedded in the application.
Windows executable resources, installers, shortcuts, and PDF associations use
the same ICO. Linux packages and AppImages install the PNG frames and scalable
SVG under the existing `mervin-pdf` desktop icon name.

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
fails on skips in every required target. Linux Release and sanitizer jobs and Windows
Release jobs build and run the tests. `tst_perf_layout` compares indexed lookup against
a full scan on 10,000 pages and reports median and 95th-percentile lookup time.
