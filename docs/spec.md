# Mervin PDF - Functional overview

Mervin PDF is a native, local-first PDF reader for Windows and Linux. It combines
everyday reading tools with measurement, OCR, form filling, annotation, page
organization, and PDF security features. It does not edit existing page text or
images.

This document describes the application's current user-facing behavior. See
[design.md](design.md) for the implementation architecture and
[BUILDING.md](BUILDING.md) for build instructions.

## Reading and navigation

- Open local PDF files or explicit web links. A new tab loads in the background
  while other documents stay usable. A failed open keeps its reason and Retry.
  Password-protected files prompt for the password when their tab is visible.
- Work with multiple documents in tabs and multiple windows. Tabs can be reordered,
  detached into a new window, moved between windows, duplicated, and reopened after
  being closed.
- Use continuous or single-page scrolling, with an independent two-page spread
  option.
- Navigate by page number, page thumbnails, document outline, and internal links.
- Use a wide zoom range, Fit Page or Fit Width, rotate the view in 90-degree steps,
  pan with the middle mouse button, and zoom toward the pointer. Zoom goes up to
  10000%. A document with a page too long to render exactly at that zoom, such as a
  roll drawing several metres long, stops at the highest zoom that renders exactly,
  and the zoom box shows it.
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
process, also while that process is busy. A start while Mervin is quitting is held.
If the quit is cancelled, the running process opens its files. If the quit goes ahead,
the next copy opens them once the old process has exited, except when the quit is part
of signing out or shutting down. If the running process does not answer for 10
seconds, the files open in a separate process that leaves the session, Recent and
updates to the running one, and the running process does not open them when it
answers again. If an older version still running after an upgrade on Linux does not
answer, the files open in a separate process as well, and the older version opens
them again once it answers. If Mervin cannot set up its per-user lock, each start
runs on its own with the session, Recent and updates, and starting it again opens
another process. Close to tray is enabled by default. Closing a window hides that
window and keeps its tabs available from the tray. **Quit Mervin** in the tray menu
exits the application and offers Save, Discard, or Cancel for unsaved edits. If
Close to tray is off or the system tray is unavailable, windows close normally and
closing the last window exits. Mervin does not install a login service.

The Recent screen provides:

- recent files, with starred favourites in their own section at the top.
  Favourites never age out of the history. Recent history kept in Settings counts
  only the other files;
- one search field that searches file names, the text inside recent documents, or
  both (names first, then contents). Settings chooses where it starts, Names by
  default. A search inside documents shows which file it is reading and can be
  stopped, keeping what it found;
- page count, file size, and last-opened information;
- recovery choices for files that were moved or deleted. By default they drop out
  of the list automatically. With Keep removed files in list on, they stay listed
  until cleared. Entries on a detached drive, an offline network share or another
  unmounted volume are never dropped automatically. On Linux there are two
  exceptions. One is a volume mounted outside `/etc/fstab` and automount, and
  outside `/media`, `/run/media`, `/mnt` and `/net`, for example by hand, by
  pam_mount or with sshfs in the home folder. While it is unmounted, its files are
  dropped if its mount folder is removed on unmount, or if an older version last
  opened the file and Mervin has not seen it on disk since the update. The other
  is a file whose folder now holds another volume; and
- file and folder actions such as copying paths or opening the containing folder.

A new tab shows Loading immediately. If its drive does not answer for 2 seconds,
it names the file and folder and offers Stop waiting. Stop closes an ordinary new
tab without changing Recent or Reopen Closed Tab. If the tab recovered unsaved
edits, Stop keeps its recovery copy and Retry instead. Failed Duplicate windows close
unless another tab has arrived there. An unloaded source's Duplicate preserves its
saved page, zoom, rotation and scroll position.

The application can restore the previous session. It opens the selected document and
leaves the other restored tabs unloaded until selected. It remembers each file's
page, zoom, rotation, and scroll position. A saved file that was deleted, moved or
renamed is dropped. A file on a detached drive, an offline network share or an
unmounted volume gets no tab but stays in the saved session. A later start that
finds it reopens it after the other restored tabs. Mervin stops looking for it 30
days after it was first missing.

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
keeps the tab and its recovery state available for retry. When the drive does not
answer a reload for 2 seconds, the tab names the file and its folder and offers Stop
waiting. Stopping leaves the tab unloaded with its recovery state and Retry, and
selecting the tab does not reload it until Retry.

Clean documents unload immediately. Edited documents and unfinished measurements
write their snapshot in the background while other tabs remain usable. Selecting
the tab before its first document reservation cancels unloading immediately.
Selecting it after that reservation keeps the live document when the snapshot
finishes. Stop during a held snapshot keeps the document locked until the drive
answers, then restores it with its edits.

On Windows, logoff waits at most four seconds for a Save or unload snapshot already
in progress to become recoverable. Mervin then allows logoff, including when the
snapshot has not finished. Edits that still exist only in memory can be lost. This
does not save other unsaved tabs or wait for replacing and reopening the original
after a recovery manifest commits. If the OS cancels logoff, Save and unloading
become available again. Accepted Security and Merge actions, document passwords,
open actions and completed results wait for that outcome. Eligible actions
continue after cancellation and remain suppressed during successful logoff.
The four-second limit bounds Mervin's own wait. OS scheduling can delay its return.

The exception is an unloaded tab without unsaved edits whose file was deleted, moved
or renamed, such as the temporary copy of a mail attachment. It closes without a
question, and Reopen Closed Tab does not offer it. Mervin judges the file as session
restore judges a saved one, so a tab whose folder, drive or share is away keeps
Retry. It checks when one of its windows becomes active and when such a tab fails to
reload. A window in the tray keeps its tabs until it is shown, and a loaded document
stays open.

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
  Mervin. Saved measurement data that decompresses to more than 16 MiB, or that is
  stored other than plain or compressed once with Flate, or that Mervin cannot parse,
  is not shown when the document opens, and the page tools report it as invalid.
  Mervin never replaces such data. Saving edits, copies, flattened exports and the
  recovery copy of an unloaded tab keep it as stored, and Measure says it is not
  available for that file. Saving, copies and the page tools write nothing and say the
  measurements are too large to save when they would store more than 16 MiB, as merging
  files with many measurements can.
- A flattened export writes ordinary PDF graphics that are visible in other PDF
  readers. Printing can include the same measurement graphics.

## Selection OCR

Selection OCR extracts text from a rectangle drawn over a page. The selected region
is rendered at 300 DPI and processed locally by MuPDF's Tesseract-based OCR device.
The result opens in an editable dialog with line-break, trim, and copy controls.

OCR requires an installed Tesseract language model:

- Language models are downloaded separately and are not included in installers.
- On first start, **Download OCR** is checked by default. Continue downloads the
  best models matching the selected display language and primary OS language,
  once per distinct model. Unsupported languages are skipped. Failed, stalled or
  interrupted downloads fail silently and are not retried on later launches.
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
- delete pages, writing a copy without them, with a preview of the result;
- extract pages into a new file, in any order, with a preview of the result;
- split every page into a separate PDF; and
- merge and reorder multiple PDFs.

Extract Pages, Delete Pages and Merge PDFs read page ranges with one grammar: `5`,
`8-10`, `7-` (to the end), `-5` (from the first page), and `all` (on its own). Merge
PDFs takes a list per file (`1-3, 5, 8-10`); Extract Pages builds one file from rows
of one page or range each, reordered like the files in Merge PDFs (typing a comma
starts the next row). Extract and Merge take pages in the order given and keep
duplicates. A range that does not resolve is reported, never skipped, and Extract
shows the result as thumbnails before anything is written. Extract, Delete and Merge
close with their selected plan and write in the launching tab, with the same wait bar
and Stop waiting as other page tools. None saves over a file open in a tab or over an
input document. Failure reopens the chosen ranges, order and output with its reason.

Merge opens at once and reads each input's page count in the background. Pending
rows show Reading and keep Merge disabled. The footer names the first row and file
still being read, including when that row is scrolled out of view. Windows drives
and UNC shares have independent read queues; on Unix, folders do. Added files that
cannot be read produce one warning after that batch finishes. Removing a pending row suppresses
its warning.

Rotate Pages, Split and Merge run in the document's tab once their questions are answered.
The tab shows its pages as they were and cannot be edited until the tool ends, while
other tabs and windows stay usable. When the drive has not answered for 2 seconds, a
bar over the pages names the folder or file the tool waits for, such as "Saving to
/nas/docs. The drive is not answering.", and offers Stop waiting. Stopping makes the
tab usable at once and prevents later output commits. A replacement already admitted
may still finish. Split keeps its committed files. These tools do not write over a
file that is open in a tab. When some of Split's `<name>-NNN.pdf` files already exist
in the chosen folder, Split asks once, for example "3 files in /nas/out already exist.
Replace them?", with No as the default. No writes nothing. Yes writes every file and
replaces the existing ones. A file that another program changes, creates or removes
after Split checked it stays as it is. Split stops at that file, keeps the files it
wrote before, and names the file in a warning. A window hidden in the tray cannot ask.
Its tray message names the folder and how many files exist, and Split writes nothing.
Closing a tab or requesting an ordinary application
quit waits for the tool and shows its wait notice. Closing a window does the same
unless it hides to the tray. Stop waiting then continues the close decision,
including any unsaved-edit question. Windows session end follows the bounded
recovery policy above.

Document > Extract Pages opens with one row that holds every page, first to last.
Save Page As in a page's right-click menu opens it with that page as its one row.

Document > Delete Pages opens the same window with the page in view as its one row.
Its rows list the pages to leave out, in any order, and repeats count once. The strip
shows every page of the document. A page to be deleted is dimmed and struck through
in red, with its number crossed out. Clicking a page deletes it or keeps it, adding it
to a row or taking it out of the row that holds it. The result line names the pages
that remain, for example "Result: 3 of 6 pages remain (1, 3-4)." Deleting every page
is refused before anything is written, with the reason on the error line. The window
writes a copy, by default `<name>-edited.pdf` beside the document, which keeps the
document's structure and encryption; the original file is never changed. Extract and
Delete share one remembered Open when done choice.

Extract and Delete check the source in the background before opening their dialog.
Their write checks the chosen password and page count again. A changed count keeps
the ranges and asks the user to check them before another attempt. Stopping the
probe opens no dialog, and stopping a write suppresses its later result.

Extract and Delete copy pages from the saved file, so unsaved comments, form entries
and measurements are not included; their window says so when the open document has
any, including edits kept while the document was unloaded. The
password typed to open an encrypted document is remembered for that tab, in memory
only, and reused by Extract, Delete, the other page operations, Save edits, Security
and Merge PDFs, so they do not ask again. Rotate, Delete, Extract and Split write
copies that keep the document's encryption, with the same passwords and permission
flags, and so does Merge PDFs when every row comes from the same file. A copy of a
document encrypted with Mervin's RC4-128 option uses AES-128 instead. When Mervin
opens such a copy after writing it, or a copy written by Save as copy or Export with
measurements, which keep that encryption too, it reuses the password. If the
remembered password no longer opens the file, Extract and Delete ask inside their
window, Merge PDFs marks the document Locked, and the other operations prompt.

Merge PDFs reads added files without a password, so a file that needs an open
password is marked Locked and cannot be merged. A file with only an owner password
opens without one and can be merged. A merge of several files is not encrypted and
drops the files' permission restrictions. When one of its files is encrypted, the
window says "The merged file will not be encrypted."

Mervin can also inspect PDF encryption, remove encryption or owner restrictions,
and create encrypted copies using AES-256, AES-128, or legacy RC4-128. Permission
flags are shown and can be written to encrypted files, but the viewer treats them as
advisory and does not disable reading, copying, or printing because of them. The
Security panel shows printing limited to low resolution as Low resolution only and
modifying limited to page assembly as Page assembly only.

An empty owner password becomes the open password while every permission is allowed.
When a permission is not allowed, an empty owner password becomes a random password
that nobody is told, so the open password cannot lift the restriction in other PDF
readers. Strip Owner Restrictions still removes it with the open password. Security
refuses an owner password that matches the open password while a permission is not
allowed, before it asks where to save. AES-128 and RC4-128 use only the first 32
characters of a password and AES-256 the first 127 bytes, so longer passwords that
agree that far also count as matching. For those, Security names the limit and asks
for passwords that differ within it. AES-128 and RC4-128 store passwords in
PDFDocEncoding, as the PDF format requires, so a password with other characters,
such as Chinese, fails with a request to choose AES-256. AES-256 accepts any
character.

Saving supports these workflows:

- Save edits writes forms, annotations, editable measurements, and manual scales
  back to the open PDF.
- Save as copy writes the same edits to a new PDF.
- Export with measurements produces a flattened, portable copy including current forms and annotations.
- Print supports page ranges, scaling, orientation, paper selection, duplex options,
  forms, annotations, and measurements.

Print prepares measured previews and prints pages in background tab jobs. The
printer wait bar names the printer, while file output writes a private PDF before
replacing the chosen destination. Printing to the source or another open tab is
refused. Stop before the file replacement keeps its previous bytes. Printer errors
include a failure to finish submitting the job. A job sent to a printer cannot be
cancelled, so after Stop or an error the printer still prints the sheets drawn so far.
After Stop, the job finishes the part of the page it is rendering, then stops using
the document, and the tab stays covered until then. A sheet whose printed area holds
more than about 16 million raster pixels, such as an A4 or Letter page at High
quality on a printer of 600 dpi or more, prints in several horizontal parts. Printing
renders at the lower of the quality and the printer's resolution. Stop or an error
partway through such a page leaves that sheet partly printed, and the printed part
can be blank if the page has no ink there. Stop never starts a sheet without
drawing on it.

Print renders each page at the selected quality for its size on paper, and only the
part that lands on the sheet. Black and white renders without antialiasing, so lines
thinner than a printed dot still print one dot wide. A page whose raster would be
too large reports that it could not be rendered, in the preview and when printing.

Save as copy and Export run in the tab's background job. Their wait bar offers Stop
waiting after two seconds without progress, and other tabs and windows stay usable.
Stopping during a blocked snapshot keeps the document covered until the drive
answers, then restores its edits. Both commands preserve the source's dirty state.
They refuse a destination open in another tab and reserve it against later opens
until writing ends. A commit already started when Stop is pressed may still finish.

Closing edited tabs or quitting offers Save, Discard, or Cancel. Hiding a window in
the tray keeps its edits without prompting. Deleting the final saved
measurement or calibration is an edit. If saving cannot replace the destination, the
edited snapshot remains available for retry or recovery.

## Settings and platform integration

Settings is one window with a page menu on the left: General (display language,
opening files, session restore, memory and tray, recent files, and the Windows
default-app action),
Appearance (UI theme, accent colour, document theme), Viewing (default zoom,
scrolling, spreads), Annotations (default colour, author name), OCR (default and
installed languages), Measuring (snapping and the defaults new tabs start from),
Forms, Keyboard shortcuts, Updates, and About (version and licences). The main menu opens
Settings; Keyboard shortcuts and About are available in its page menu. OK applies
the changes and closes Settings. Apply puts them into effect and keeps Settings
open; it is available only while a change is pending. If the settings file can't
be written, OK and Apply say why and keep Settings open, and nothing changes until
a save works. Cancel discards changes made since the last Apply. Removing or adding
OCR languages takes effect at once. Session
restore and automatic updates are enabled by default. Copies that cannot update
themselves (portable and development builds) show no automatic-update switch, only
Check for Updates. General determines the initial window height, capped to the
available screen. Pages scroll on smaller screens. Scrolling over number fields
and closed dropdowns scrolls the page without changing their values. Keyboard
editing and scrolling an opened dropdown remain available.

New profiles follow the system theme and use the system accent. Turning off the system
accent keeps its current colour; Reset selects the custom default `#62c9ff`.
Keep removed files in list is off by default. Versions up to 1.65.0 saved it as on in
nearly every profile, so it starts off once after updating to 1.65.4 or later from any
earlier version. Later choices are kept.

The interface is available in 53 languages, including English, Swedish, Simplified
Chinese and Traditional Chinese. See [TRANSLATING.md](TRANSLATING.md) for coverage.
The first time Mervin starts without a saved display language (a new install, or the first
start after updating from a version without language support), a welcome window
appears before any other. Its display language picker starts at the first
language in the OS preference list that Mervin offers in the same script, or
English. Traditional Chinese systems select 繁體中文 (`zh_TW`), and picking
another language switches the window to it at once. On Windows, when Mervin is not the
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
saved language applies at the next start. Until then Settings shows the saved
language with the restart note, and picking the language on screen takes the change
back. With session restore on, the new copy reopens the documents that were open.

Starting Mervin with `--language <code>` (or `--language=<code>`), such as
`--language sv`, shows that language for that run only. It skips the welcome window
and leaves the saved setting unchanged; Settings shows the language on screen.
Use a shipped catalog code, such as `en`, `sv`, `zh_CN` or `zh_TW`.
Case and `-` or `_` do not matter, and an unknown code shows English. If Mervin is
already running, the new launch hands its files to the running copy and the flag
has no effect.

Memory and tray contains a numeric inactivity interval from 1 through 10080 whole
minutes, with a default of 30. The Never checkbox disables the number field and
keeps loaded documents resident, including in the tray. The separate Close to tray
switch is enabled by default.

Appearance offers illustrated choices with labels and an exclusive selection
indicator. Application holds the UI theme in the order Dark, Light, Follow system.
Each preview shows a Mervin window in that scheme, and Follow system splits the
window between the two. New installs follow the system. Document holds the
document themes in the order Traditional, Comfort, Inverted. Traditional preserves
page colours, Comfort darkens the page while keeping photos readable, and Inverted
reverses all page colours.

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
  on the next start after network or download transfer errors, or when the update
  server's answer cannot be read, as when a network filter returns its own page.
  A manual check then says that it could not check. A downloaded update that no
  longer matches its checksum when it is about to install is deleted, and the next
  start checks again. Handing off to the
  Windows installer also resets the interval, so an unsuccessful installation
  cannot cause an immediate install and restart loop. An installed copy
  downloads the new release in the same package format it was installed from
  (MSI installer, AppImage, .deb, or .rpm), installs it automatically, then restarts.
  Copies in different formats on one computer, such as an AppImage beside the .deb
  install, keep separate check dates and downloads, and never install or delete each
  other's packages. Each start of an installed copy deletes its downloaded update
  once that is no longer newer than the copy, such as the one just installed, also
  with automatic updates off.
  This applies to both Check for Updates and scheduled checks, with no separate
  install confirmation. The usual save prompts protect unsaved documents, and
  cancelling a save prompt cancels the restart. Operating system permission
  prompts still apply. Linux systems without the required package installation
  tools open the desktop installer and require manual completion and restart.
  Turning automatic updates off prevents automatic checks
  and installation, including a previously downloaded update. Check for Updates
  still downloads, installs and restarts when an update exists. Copies that
  cannot update themselves, such as dev builds, offer the release page instead.

## Product boundaries

Mervin is a reader and document-workflow tool, not a full PDF authoring suite. It
does not provide original text or image editing, OCR language detection, form
creation, digital signing, JavaScript execution, cloud storage, or collaboration
services.
