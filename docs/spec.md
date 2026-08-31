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
  options. Search state is kept separately for each open tab.
- Choose a light, dark, or system application theme. PDF pages have a separate
  Traditional, Inverted, or Comfort theme.
- External web links in a PDF open in the system browser. An HTTP(S) URL entered
  through Open is downloaded locally with progress and then opened.

## Windows, sessions, and recent files

Mervin uses one foreground process per user. Starting it again forwards files to
the running process. There is no tray icon, login service, or background daemon;
closing the last window exits the application.

The Recent screen provides:

- recent files and favourites;
- filtering by file name or local full-text search inside recent documents;
- page count, file size, and last-opened information;
- recovery choices for files that were moved or deleted; and
- file and folder actions such as copying paths or opening the containing folder.

The application can restore the previous session. It also remembers each file's
page, zoom, rotation, and scroll position.

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
  `tessdata_best` models and choose the default language.
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
- extract a page range;
- split every page into a separate PDF; and
- merge and reorder multiple PDFs.

Encrypted inputs are supported by the single-document operations after a password
prompt. They cannot be added to a merge plan.

Mervin can also inspect PDF encryption, remove encryption or owner restrictions,
and create encrypted copies using AES-256, AES-128, or legacy RC4-128. Permission
flags are shown and can be written to encrypted files, but the viewer treats them as
advisory and does not disable reading, copying, or printing because of them.

Saving supports these workflows:

- Save edits writes forms, annotations, editable measurements, and manual scales
  back to the open PDF.
- Save as copy writes the same edits to a new PDF.
- Export with measurements produces a flattened, portable copy.
- Print supports page ranges, scaling, orientation, paper selection, duplex options,
  forms, annotations, and measurements.

## Settings and platform integration

Settings cover viewing defaults, application and document themes, open behavior,
recent-file limits, measurement snapping, form behavior, annotation defaults, and
optional update checks. The OCR default is selected in Manage OCR languages, and
session restore is enabled by default.

On Windows, Mervin can register itself as a PDF handler and open the system Default
Apps settings. If Mervin is not already the default, it offers this once on first
launch, and the same action remains available in Settings. Linux packages install
the desktop and MIME metadata needed for the desktop environment's Open With and
default-application controls; Mervin does not expose a Linux default-app button.

## Privacy and network access

Document rendering, search, OCR, measurement, form filling, annotation, page
operations, security operations, recent history, and settings are all local. Mervin
has no account requirement or telemetry.

Network access follows an explicit action or an opt-in setting:

- opening or downloading an explicit web URL;
- loading the OCR language catalog or downloading a chosen language model; or
- checking for updates manually or through the startup check, which is off by
  default. A Windows installer is downloaded only after the user accepts an offered
  update; Linux opens the release page instead.

## Product boundaries

Mervin is a reader and document-workflow tool, not a full PDF authoring suite. It
does not provide original text or image editing, OCR language detection, form
creation, digital signing, JavaScript execution, cloud storage, or collaboration
services.
