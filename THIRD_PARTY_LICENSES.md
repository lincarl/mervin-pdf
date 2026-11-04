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
| MuPDF | 1.28.6 | AGPL v3 or commercial (Artifex) | Static | Dominant distribution constraint. Built from the official source release with Mervin's patches in `scripts/mupdf-patches`, unavailable in vcpkg. |
| Qt 6 | 6.12.0 official binaries on Windows and Linux | LGPL v3 or commercial | **Dynamic** | Packages include Qt's libraries and plugins. Do not static-link. |
| qpdf | 12.4.2 (vcpkg baseline) | Apache 2.0 | Windows static; Linux bundled shared library | Page operations and security. |
| toml++ | 3.4.0 (vcpkg baseline or pinned development fallback) | MIT | Windows static; Linux bundled shared library or development header-only fallback | Config-file parsing and saving. |
| Lucide icons | lucide-static 1.61.0 | ISC; icons derived from Feather are MIT | Compiled in (Qt resource) | Every UI icon, vendored unmodified in `resources/icons/lucide`. The licence text, including the Feather MIT notice for icons such as check, x, the chevrons, plus, minus and lock, is `licenses/lucide-LICENSE.txt`. |

## vcpkg dependencies

The `x64-windows-static-md` triplet links the vcpkg libraries statically and keeps
the Microsoft runtime dynamic. qpdf uses libjpeg-turbo and zlib. The selected
libjpeg-turbo port also builds libspng and TurboJPEG support. The deployment does
not ship the vcpkg command-line tools or DLLs on Windows. Linux uses
`x64-linux-dynamic` and bundles its shared libraries in the private application
directory. Keeping qpdf's JPEG implementation in a shared library avoids binding
it to MuPDF's incompatible static JPEG implementation.

Static linking does not remove notice requirements. Packages include the
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
| Tesseract 5.5.4 | Apache 2.0 | OCR engine, updated runtime sources with Artifex integration preserved |
| MuJS 1.3.11 | ISC | MuPDF's bundled JavaScript library; Mervin does not enable PDF JavaScript |
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

The Tesseract update in `scripts/mupdf-patches/0007-tesseract-update-runtime-to-5.5.4.patch`
contains the upstream runtime source changes from 5.5.2 through 5.5.4, commit
`7e548e6eff172f38f97664afe7f7ce40db36c8d6`, plus adjustments to MuPDF's source
lists and generated version header. Artifex's OCR adapter, allocation hooks,
disabled legacy engine and double-precision build configuration remain in use.
The MuJS update comes from the upstream 1.3.11 tag, commit
`01a75ff4324ae8a54cae91a26edd38029424edaa`. Both components retain their existing
license texts. Mervin also corrects the MuJS 1.3.11 array rollback loop's index,
as the patch records. The patch files are part of the corresponding modified source.

## AGPL implications

- **Sharing the binary, or hosting it as a service:** the entire application
  source must be offered under the AGPL, **or** a commercial MuPDF licence must
  be purchased from Artifex.
- **Modified MuPDF:** Mervin changes MuPDF with the patch files in
  `scripts/mupdf-patches`. Each one states what it changes and the date of the
  change. The build scripts apply them to the MuPDF 1.28.6 source release, which
  they download and verify by SHA-256. Windows builds also define
  `FZ_HIDE_INTERNAL_JPEG` in MuPDF's `scripts/libjpeg/jconfig.h` through
  `scripts/build-mupdf-windows.ps1`, which renames MuPDF's bundled JPEG symbols.
  The source of a released build is <https://github.com/lincarl/mervin-pdf> at
  the release's tag together with
  <https://mupdf.com/downloads/archive/mupdf-1.28.6-source.tar.gz> (SHA-256
  `dc85000640a45d90ca9b77b061f6ca67cce0c4d4fbecf8819647baf573eef101`).

## LGPL implications for Qt (relevant only on distribution)

- Qt is linked **dynamically**. Installers ship its DLLs on Windows and private shared libraries on Linux.
- Include the Qt LGPL notice on the About page in Settings and in bundled documentation.
- Avoid static linking unless prepared to meet the additional LGPL obligations.

Qt's embedded third-party components and deployed Wayland protocols have
additional license and notice requirements. Their complete notices, including
the ICU 73.2 license for Linux SDK libraries, are included under
[`licenses/qt/`](licenses/qt/README.md). Keep that directory with the other
license files when redistributing packages.
