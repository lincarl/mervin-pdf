# Mervin PDF - Functional overview

Mervin PDF is a native, local-first PDF reader for Windows and Linux. It combines
everyday reading tools with measurement, OCR, form filling, annotation, page
organization, and PDF security features. It does not edit existing page text or
images.

This document describes the application's current user-facing behavior. See
[design.md](design.md) for the implementation architecture and
[BUILDING.md](BUILDING.md) for build instructions.

## Reading and navigation

- Open local PDF files or explicit web links. Password-protected files prompt for
  the password when needed.
- Work with multiple documents in tabs and multiple windows. Tabs can be reordered,
  detached into a new window, moved between windows, duplicated, and reopened after
  being closed.
- Use continuous or single-page scrolling, with an independent two-page spread
  option.
- Navigate by page number, page thumbnails, document outline, and internal links.
- Use a wide zoom range, Fit Page or Fit Width, rotate the view in 90-degree steps,
  pan with the middle mouse button, and zoom toward the pointer.
- Select and copy text, or search the document with case-sensitive and whole-word
  options. Ctrl+F or the toolbar's search button opens a find card over the top
  right of the page, and Escape closes it. Search state is kept separately for each
  open tab.
- Choose a light, dark, or system application theme. PDF pages have a separate
  Traditional, Inverted, or Comfort theme.
- External web links in a PDF open in the system browser. An HTTP(S) URL entered
  through Open is downloaded locally with progress and then opened.

## Windows, sessions, and recent files

Mervin uses one process per user. Starting it again forwards files to the running
process. Close to tray is enabled by default. Closing a window hides that window
and keeps its tabs available from the tray. **Quit Mervin** in the tray menu exits
the application and offers Save, Discard, or Cancel for unsaved edits. If Close to
tray is off or the system tray is unavailable, windows close normally and closing
the last window exits. Mervin does not install a login service.

The Recent screen provides:

- recent files, with starred favourites in their own section at the top;
- one search field that searches file names, the text inside recent documents, or
  both (names first, then contents). Settings chooses where it starts, Names by
  default. A search inside documents shows which file it is reading and can be
  stopped, keeping what it found;
- page count, file size, and last-opened information;
- recovery choices for files that were moved or deleted, which stay listed until
  cleared unless Settings is set to drop them automatically. Entries on a detached
  drive or an offline network share are never dropped automatically; and
- file and folder actions such as copying paths or opening the containing folder.

The application can restore the previous session. It opens the selected document
and leaves the other restored tabs unloaded until selected. It remembers each file's
page, zoom, rotation, and scroll position.

Inactive documents unload after 30 minutes by default. This includes background tabs,
documents hidden by the Recent screen, and documents in minimized or hidden windows.
A visible current document stays loaded even when another application has keyboard
focus. Closing a window to the tray unloads its documents immediately. Returning to
an unloaded tab reloads its document and restores its view. Never disables both
timed and tray unloading.

Unsaved forms, annotations, measurements, and manual scales survive unloading in
local recovery snapshots. Snapshots preserve the source PDF's encryption, and
passwords remain in memory only. Unloading does not save edits into the original
file. If a snapshot cannot be written, the document stays loaded. A failed reload
keeps the tab and its recovery state available for retry.

## Measuring drawings

The measuring tool is intended for scaled plans, CAD exports, and maps.

- Mervin reads rectilinear PDF measurement metadata when it is present.
- A page can instead be calibrated from a known distance or assigned a scale ratio
  manually. Scale is stored per page.
- Supported measurements are distance, multi-segment path, polygon area and
  perimeter, and angle.
- Measurements can snap to vector vertices and edges in the drawing.
- Units, precision, and line width can be adjusted. Measurement vertices and value
  labels can be repositioned on the page.
- Editable measurements and manual scales can be saved back into the PDF for use in
  Mervin.
- A flattened export writes ordinary PDF graphics that are visible in other PDF
  readers. Printing can include the same measurement graphics.

## Selection OCR

Selection OCR extracts text from a rectangle drawn over a page. The selected region
is rendered at 300 DPI and processed locally by MuPDF's Tesseract-based OCR device.
The result opens in an editable dialog with line-break, trim, and copy controls.

OCR requires an installed Tesseract language model:

- English is bundled with the application.
- The Manage OCR languages dialog can download and remove official
  `tessdata_best` models and choose the default language. Settings > OCR lists the
  installed models, removes them, chooses the default and opens the same manager.
- Returning from the manager refreshes the language picker.
- Changing the language in the OCR result dialog immediately runs OCR again for the
  current selection.
- Language choice is explicit. The OCR engine does not automatically identify the
  document language.

## Forms

Mervin fills existing AcroForm fields and can automatically enter form mode when a
document contains them. Supported fields include text boxes, check boxes, radio
buttons, combo boxes, and list boxes. Field highlighting can be enabled to make
fillable and required fields easier to find.

Filled values render immediately and are included when saving or printing. Read-only
and signature fields are displayed but not edited. Creating form fields, XFA forms,
and form JavaScript are outside the application's scope.

## Annotations and comments

PDFs can be marked with highlights, underlines, strikeouts, and sticky-note
comments. Existing supported annotations can be inspected, edited, recolored, or
deleted. A Comments sidebar lists annotations and navigates to them.

These annotations are stored as standard PDF annotations, so other PDF readers can
display them. Saved and printed output includes the current annotations.

## Page and file operations

The Document menu provides structural operations that create new output files:

- rotate selected pages;
- delete selected pages;
- extract pages into a new file, in any order, with a preview of the result;
- split every page into a separate PDF; and
- merge and reorder multiple PDFs.

Extract Pages and Merge PDFs read page ranges with one grammar: `5`, `8-10`, `7-` (to
the end), `-5` (from the first page), and `all` (on its own). Merge PDFs takes a list
per file (`1-3, 5, 8-10`); Extract Pages builds one file from rows of one page or range
each, reordered like the files in Merge PDFs (typing a comma starts the next row).
Pages are taken in the order given and duplicates are kept. A range that does not
resolve is reported, never skipped, and Extract shows the result as thumbnails before
anything is written. Both dialogs write before they close: neither saves over a file
that is open in a tab, and a write that fails leaves the dialog open with its plan.

Save Page As in a page's right-click menu opens Extract Pages with that page as its
one row.

Extract copies pages from the saved file, so unsaved comments and form entries are not
included; the dialog says so when the open document has any. The password typed to
open an encrypted document is remembered for that tab, in memory only, and reused by
Extract, the page operations, Save edits, Security and Merge PDFs, so they do not ask
again. Opening a copy written by Rotate, Delete, Save as copy or Export with
measurements, which keeps that encryption, reuses it too. If it no longer opens the
file, Extract asks inside its dialog, Merge PDFs marks the document Locked, and the
other operations prompt. Other encrypted files cannot be added to a merge plan.

Mervin can also inspect PDF encryption, remove encryption or owner restrictions,
and create encrypted copies using AES-256, AES-128, or legacy RC4-128. Permission
flags are shown and can be written to encrypted files, but the viewer treats them as
advisory and does not disable reading, copying, or printing because of them.

Saving supports these workflows:

- Save edits writes forms, annotations, editable measurements, and manual scales
  back to the open PDF.
- Save as copy writes the same edits to a new PDF.
- Export with measurements produces a flattened, portable copy including current forms and annotations.
- Print supports page ranges, scaling, orientation, paper selection, duplex options,
  forms, annotations, and measurements.

Closing edited tabs or quitting offers Save, Discard, or Cancel. Hiding a window in
the tray keeps its edits without prompting. Deleting the final saved
measurement or calibration is an edit. If saving cannot replace the destination, the
edited snapshot remains available for retry or recovery.

## Settings and platform integration

Settings is one window with a page menu on the left: General (display language,
opening files, session restore, memory and tray, recent files, updates, and the
Windows default-app action),
Appearance (UI theme, accent colour, document theme), Viewing (default zoom,
scrolling, spreads), Annotations (default colour, author name), OCR (default and
installed languages), Measuring (snapping and the defaults new tabs start from),
Forms, Keyboard shortcuts, and About (version and licences). The main menu opens
Settings; Keyboard shortcuts and About are available in its page menu. OK applies
the changes and closes Settings. Apply puts them into effect and keeps Settings
open; it is available only while a change is pending. Cancel discards changes made
since the last Apply. Removing or adding OCR languages takes effect at once. Session
restore and automatic updates are enabled by default. Copies that cannot update
themselves (portable and development builds) show no automatic-update switch, only
Check for Updates.

The interface is available in English, Swedish, and Simplified Chinese. The first
time Mervin starts without a saved display language (a new install, or the first
start after updating from a version without language support), a welcome window
appears before any other. Its display language picker starts at the first
language in the OS preference list that Mervin offers, or English, and picking
another switches the window to it at once. On Windows, when Mervin is not the
default PDF viewer and has not offered this before, the window also has a ticked
"Make Mervin PDF my default PDF viewer" checkbox; Continue then opens the system
Default Apps settings to confirm. An offer made by an earlier version on its first
launch counts. Closing the window keeps the language it shows and leaves the
default viewer alone. Files opened while the window is up, such as a
double-clicked PDF, open with the first main window.

Language in General holds the same picker. Each language is listed by its own name
and its English name, such as "Svenska (Swedish)", and the open list has a search
field that also matches the name in the current interface language and the language
code, ignoring case and accents. Choosing a language other than the one shown adds
the note "Mervin will restart." OK or Apply then saves the choice, closes Settings,
and restarts Mervin. The restart closes windows as Quit does, so unsaved documents
still offer Save, Discard, or Cancel; cancelling one cancels the restart, and the
saved language applies at the next start. With session restore on, the new copy
reopens the documents that were open.

Starting Mervin with `--language <code>` (or `--language=<code>`), such as
`--language sv`, shows that language for that run only. It skips the welcome window
and leaves the saved setting unchanged. The codes are `en`, `sv`, and `zh_CN`;
case and `-` or `_` do not matter, and an unknown code shows English. If Mervin is
already running, the new launch hands its files to the running copy and the flag
has no effect.

Memory and tray contains a numeric inactivity interval from 1 through 10080 whole
minutes, with a default of 30. The Never checkbox disables the number field and
keeps loaded documents resident, including in the tray. The separate Close to tray
switch is enabled by default.

Appearance offers illustrated choices with labels and an exclusive selection
indicator. Application holds the UI theme in the order Dark, Light, Follow system.
Each preview shows a Mervin window in that scheme, and Follow system splits the
window between the two. Document holds the document themes in the order
Traditional, Comfort, Inverted. Traditional preserves page colours, Comfort darkens
the page while keeping photos readable, and Inverted reverses all page colours.

On Windows, Mervin can register itself as a PDF handler and open the system Default
Apps settings. If Mervin is not already the default, the welcome window offers this
once, and the same action remains available in Settings. Linux packages install
the desktop and MIME metadata needed for the desktop environment's Open With and
default-application controls; Mervin does not expose a Linux default-app button.

## Privacy and network access

Document rendering, search, OCR, measurement, form filling, annotation, page
operations, security operations, recent history, and settings are all local. Mervin
has no account requirement or telemetry.

Network access follows an explicit action or the automatic update setting:

- opening or downloading an explicit web URL;
- loading the OCR language catalog or downloading a chosen language model; or
- checking for updates, manually from Settings > General or automatically on
  startup when at least 30 days have passed since the last successful check (on by
  default, off in Settings > General). A profile without a saved check date checks on its next start.
  Successful manual checks also reset the interval. Failed scheduled checks retry
  on the next start after network or download transfer errors. An installed copy
  downloads the new release in the same package format it was installed from
  (NSIS or MSI installer, AppImage, .deb, or .rpm) in the background, then asks
  before installing. The choices are Install Now,
  Later (asked again on every start), or Never (turns automatic updates off and
  deletes the download). Copies that cannot update themselves, such as dev builds,
  offer the release page instead.

## Product boundaries

Mervin is a reader and document-workflow tool, not a full PDF authoring suite. It
does not provide original text or image editing, OCR language detection, form
creation, digital signing, JavaScript execution, cloud storage, or collaboration
services.
