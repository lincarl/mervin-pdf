# Mervin PDF

**A fast, private PDF reader built for people who work with documents, not just look at them.**

Mervin PDF combines a focused native reader with practical tools for technical drawings and everyday document work. Measure scaled plans, OCR part of a scanned page, fill forms, annotate, reorganize pages, and manage PDF security without sending the document to an online service.

> Mervin PDF is under active development. Please report bugs and feature requests through [GitHub Issues](https://github.com/lincarl/mervin-pdf/issues).

## What makes Mervin different?

Most lightweight PDF readers stop at viewing and annotation. Full PDF suites include more, but can feel heavy, account-driven, or cloud-first. Mervin aims for the useful space between them:

| | Mervin PDF |
|---|---|
| **Technical drawings** | Detects embedded CAD scales or lets you calibrate a page, then measures distance, paths, area, perimeter, and angles with vertex and edge snapping. |
| **Local-first tools** | Selection OCR, search, page operations, form filling, annotations, and security operations run on your computer. |
| **Comfortable reading** | Traditional, inverted, and Comfort document themes are independent of the application theme. |
| **Real document workflow** | Detachable and mergeable tabs, session restore, per-file view state, thumbnails, outlines, and continuous or spread layouts. |
| **No resident process** | No tray application, login service, or background daemon. Closing the last window exits Mervin. |
| **Open source** | The complete application source is available under the AGPL-3.0 license. |

The measuring workflow is the main distinction. Mervin understands rectilinear PDF measurement metadata exported by CAD software, supports manual calibration when metadata is absent, and can preserve measurements for later editing or burn them into a portable PDF that any reader can display.

## Features

- Fast native rendering with MuPDF
- Continuous, single-page, and two-page spread layouts
- Zoom from 8% to 1000%, Fit Page, Fit Width, rotation, pan, and zoom-to-cursor
- Text selection, document search, thumbnails, and outlines
- Distance, path, area, perimeter, and angle measurement
- Automatic scale detection, manual calibration, and CAD geometry snapping
- Editable saved measurements, flattened measurement export, and measurement-aware printing
- Local 300-DPI selection OCR with support for additional Tesseract languages
- AcroForm filling for text fields, check boxes, radio buttons, combo boxes, and list boxes
- Highlights, underlines, strikeouts, sticky notes, and a comments panel
- Rotate, delete, extract, split, and merge pages
- Inspect, add, change, or remove PDF encryption and permissions using qpdf
- Multiple windows, detachable tabs, recent files, session recovery, and per-document resume
- Dark, light, and system UI themes plus independent document color themes

## Privacy by default

Documents, OCR, search, settings, recent-file history, and session data stay on the local machine. Mervin has no telemetry. It does not require an account, upload documents, or keep a background service running.

Opening a web URL is an explicit user action and downloads that PDF for local viewing.

## Platforms

Mervin PDF provides release packages for:

- Windows 11 x64: NSIS installer and MSI
- Linux x86-64: AppImage, DEB, and RPM (Ubuntu 26.04 or a compatible distribution is the current baseline)

Download packaged versions from [GitHub Releases](https://github.com/lincarl/mervin-pdf/releases).

## Build from source

Mervin is a C++20 and Qt 6 application. It uses MuPDF for rendering and OCR, qpdf for structural and security operations, and CMake for its build.

The build requires Qt 6.6 or newer, MuPDF 1.28.0 built from source, qpdf, and toml++. Windows uses MSVC and vcpkg; Linux uses CMake/Ninja and the corresponding development packages.

See [docs/BUILDING.md](docs/BUILDING.md) for detailed Windows setup and build instructions.

## Contributing

Bug reports and focused pull requests are welcome. Before starting a substantial change, please open an issue so the intended behavior and scope can be discussed. Keep changes small, include tests where practical, and run the existing test suite before submitting.

## License

Mervin PDF is licensed under the [GNU Affero General Public License v3.0](LICENSE).

Mervin includes and links to third-party open-source components with their own licenses. See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for details. MuPDF is used under the AGPL; distributors must comply with the licenses of Mervin and all bundled dependencies.
