# Third-party licenses

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
| MuPDF | 1.28.5 | AGPL v3 or commercial (Artifex) | Static | Dominant distribution constraint. Built from source, unavailable in vcpkg. |
| Qt 6 | Windows 6.12.0; Linux distribution Qt (Ubuntu 26.04 uses 6.10) | LGPL v3 or commercial | **Dynamic** | Dynamic linking satisfies the LGPL. Do not static-link. |
| qpdf | Windows 12.4.2 (vcpkg baseline); Linux distribution package | Apache 2.0 | Windows static; Linux distribution library | Page operations and security. |
| toml++ | 3.4.0 (vcpkg baseline, Linux distribution package or pinned fallback) | MIT | Windows static; Linux distribution library or header-only fallback | Config-file parsing and saving. |
| Lucide icons | lucide-static 1.49.0 | ISC; icons derived from Feather are MIT | Compiled in (Qt resource) | Every UI icon, vendored unmodified in `resources/icons/lucide`. The licence text, including the Feather MIT notice for icons such as check, x, the chevrons, plus, minus and lock, is `licenses/lucide-LICENSE.txt`. |

## Windows vcpkg dependencies

The `x64-windows-static-md` triplet links the vcpkg libraries statically and keeps
the Microsoft runtime dynamic. qpdf uses libjpeg-turbo and zlib. The selected
libjpeg-turbo port also builds libspng and TurboJPEG support. The deployment does
not ship the vcpkg command-line tools or DLLs.

Static linking does not remove notice requirements. Windows packages include the
target packages' complete vcpkg copyright files under `licenses/vcpkg`, covering
qpdf, toml++, libjpeg-turbo, libspng and zlib in addition to the maintained notices
in this repository. libjpeg-turbo carries IJG, BSD and zlib-style terms; libspng
uses the BSD 2-Clause licence; zlib uses the zlib licence. Keep these notices when
redistributing the statically linked application.

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
- Include the Qt LGPL notice on the About page in Settings and in bundled documentation.
- Avoid static linking unless prepared to meet the additional LGPL obligations.
