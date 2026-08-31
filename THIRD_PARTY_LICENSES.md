# Third-Party Licenses

Mervin PDF is built on several open-source components. This file documents each
component, its licence, and the obligations that apply. Full licence texts live
in the [`licenses/`](licenses/) directory.

> **Distribution note.** Mervin PDF is released under the GNU Affero General
> Public License v3.0. Binary distributors must also satisfy the obligations of
> the dependencies listed below. The dominant constraint is MuPDF's AGPL (see
> below).

## Direct dependencies

| Component | Version (pinned) | Licence | Linking | Notes |
|---|---|---|---|---|
| MuPDF | 1.28.0 | AGPL v3 or commercial (Artifex) | Static | Dominant distribution constraint. Built from source — not available in vcpkg. |
| Qt 6 | Windows 6.8.3; Linux distribution Qt (Ubuntu 26.04 uses 6.10) | LGPL v3 or commercial | **Dynamic** | Dynamic linking satisfies the LGPL. Do not static-link. |
| qpdf | Windows vcpkg baseline; Linux distribution package | Apache 2.0 | Dynamic | Page operations and security. |
| toml++ | Windows vcpkg baseline; Linux distribution package or pinned 3.4.0 fallback | MIT | Header-only | Config-file parsing. |

## MuPDF bundled / transitive dependencies

Pulled in by MuPDF's static build. Most are individually permissive; `jbig2dec`
is Artifex AGPL and is covered by the same arrangement as MuPDF.

| Component | Licence | Used for |
|---|---|---|
| Tesseract 5.5.2 | Apache 2.0 | OCR engine |
| Leptonica 1.87.0 | BSD 2-Clause | Image processing for OCR |
| freetype | FTL or GPL v2 | Font rasterization |
| harfbuzz | MIT-style | Text shaping |
| IJG libjpeg | IJG permissive licence | JPEG decoding |
| openjpeg | BSD | JPEG 2000 decoding |
| jbig2dec | AGPL (Artifex) | JBIG2 decoding |
| zlib | zlib | Compression |
| gumbo-parser | Apache 2.0 | HTML parsing |
| lcms2 | MIT | Colour management |
| brotli | MIT | Compression |

The verified MuPDF source archive also carries licence texts for its bundled
auxiliary components (cmark-gfm, curl, extract, freeglut, MuJS, Zint, and
ZXing-C++). Their verbatim notices are included under `licenses/` and shipped
with every installer.

## AGPL implications

- **Sharing the binary, or hosting it as a service:** the entire application
  source must be offered under the AGPL, **or** a commercial MuPDF licence must
  be purchased from Artifex.

## LGPL implications for Qt (relevant only on distribution)

- Qt is linked **dynamically** (the standard installer ships the Qt DLLs).
- Include the Qt LGPL notice in the About dialog and bundled documentation.
- Avoid static linking unless prepared to meet the additional LGPL obligations.
