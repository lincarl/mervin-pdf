# Mervin PDF - Design and architecture

This document summarizes the current implementation for contributors. It focuses on
the components and data flows needed to understand the application. User-facing
behavior is described in [spec.md](spec.md).

## Technology

Mervin is a C++20 desktop application built with CMake.

| Component | Responsibility |
| --- | --- |
| Qt 6 Core, GUI, Widgets, Network, and PrintSupport | Native UI, printing, networking, and local IPC |
| MuPDF | Document parsing, rendering, text extraction, forms, annotations, and OCR |
| qpdf | Page operations, encryption, permissions, and measurement PDF output |
| toml++ | Settings serialization |
| Tesseract data through MuPDF | Local OCR language models |

MuPDF is built from source and linked statically. Qt and qpdf are dynamically linked
in packaged builds. The application is licensed under AGPL-3.0; dependency notices
are maintained in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## Source layout

The build produces a reusable, widget-free static library named `mervin_core` and the
Qt Widgets executable `MervinPDF`.

```text
src/
  main.cpp       application startup and single-instance handoff
  app/           process-level window and store ownership
  config/        paths and TOML settings
  dialogs/       application dialogs
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
  update/        release checking and update download
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
outlives every open `Document`, ensuring worker threads stop before documents are
destroyed.

The explicit Duplicate to new window command bypasses normal path deduplication and
creates a second independent view.

Each `TabPage` groups one open document with its viewer and editing models. Tab
detachment and merging reparent this live tab object rather than reopening the file.

## Startup and single-instance behavior

The first process acquires a per-user `QLockFile` and starts a `QLocalServer`.
Subsequent launches send newline-delimited JSON containing file paths and open
behavior, wait for an acknowledgement, and exit. The primary process acknowledges
before opening files so a password or error dialog cannot make the sender time out.

If the primary process disappears during handoff, the new process retries ownership
and can fall back to a standalone window. Closing the last window shuts down the
render workers and exits the process.

The development-only `--profile <directory>` option redirects application files and
Qt settings, and gives the process a separate single-instance identity. Tests and
manual checks use it to avoid touching normal user state.

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
current ones.

`ViewerWidget` is a `QAbstractScrollArea` responsible for layout, visible-page render
requests, cache use, coordinate conversion, selection, links, and tool overlays.
`ViewLayout` handles continuous or single-page scrolling and the independent spread
setting. A page-image cache limits memory use; high zoom levels render visible tiles
and use a preview layer while fresh pixels arrive.

All interactive geometry uses a consistent unrotated page-point space with a
top-left origin. The viewer converts between that space, screen coordinates, and PDF
user space for rotated pages and non-zero page origins.

## Text, search, and navigation

`TextIndex` extracts structured text per document and keeps glyph rectangles for
selection, hit testing, copying, and find highlights. The in-document matcher adds
case-sensitive and whole-word behavior on top of the extracted text.

Recent-file content search runs on its own worker and opens files independently. It
returns the first matching page and a short snippet for each file. Generation and
cancellation tokens stop replaced searches from publishing stale results.

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
language change can submit the recognition again.

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

## Save and print pipeline

The application has two complementary writers:

- MuPDF writes the live document when forms or standard annotations changed.
- qpdf embeds editable measurement data, flattens measurement graphics, and performs
  structural or security operations.

When live MuPDF edits and measurement data coexist, the writers are applied in
sequence to a temporary file. For an in-place Save edits operation, the application
then closes the source handle, replaces the destination, and reopens it. Failed
replacement restores the original where possible. Save as copy writes directly to
the chosen destination. JSON state files use `QSaveFile` for atomic replacement.

Printing rasterizes the current live document at the selected quality, capped by the
printer resolution. If measurements are present, a temporary flattened copy is used
so the printed result contains them without changing the source.

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
| `tessdata/` | writable OCR language models |
| `downloads/` | PDF downloads when no normal download directory is available or a profile is active |

Paths are normalized before deduplication and lookup, with case folding on Windows.
Corrupt or missing state files fall back to defaults instead of blocking startup.
Update throttling and skipped-version state use `QSettings`; Windows file-handler
registration uses the per-user registry. A profile redirects the Qt settings as well
as the files above.

## Theme and icons

All themed application-chrome and viewer-surface colors are defined in
`src/ui/ThemeTokens.cpp`. `Theme` builds the application stylesheet from those
tokens and the selected system or custom accent. Colors written into PDF content and
the compile-time Comfort transform ramp are deliberate data-level exceptions.
Document color transforms are separate from application chrome so page appearance
can be changed independently.

`ui/Icons` paints the interface icon set with `QPainter`, producing consistent
artwork on Windows and Linux without depending on a platform icon font. The packaged
application icon is generated from the repository's icon assets.

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
platform release pipeline adds Windows NSIS/MSI installers and Linux
AppImage/DEB/RPM artifacts. Windows uses Qt 6.8.3; Linux requires Qt 6.6 or newer.
MuPDF 1.28.0 is built from source with OCR support.

QtTest targets cover the core stores, IPC, rendering helpers, document tools,
dialogs, layout, theme, and platform-sensitive behavior. Performance targets measure
rendering and startup separately. Windows contributor setup and exact commands live
in [BUILDING.md](BUILDING.md).
