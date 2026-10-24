# Bundled Qt notices

These notices accompany the official Qt 6.12.0 runtime used by Mervin PDF.
Qt's own license texts are in the parent directory.

- `qtbase-NOTICES.txt` retains Qt Base and Qt Svg copyright statements and
  complete upstream license texts. Each entry names its source URL, version
  when available, and applicable documents. Each complete document includes
  its source URL and SHA-256 hash. Upstream text is unchanged.
- `Wayland-NOTICES.txt` retains notices from 29 runtime protocol XML files,
  including Qt extension protocols, plus full supporting license texts. It also covers compiled protocols omitted from Qt's binary SBOM.
- `ICU-LICENSE.txt` is the complete, unchanged ICU 73.2 license, including ICU's
  own third-party notices. It accompanies the ICU libraries deployed from the
  official Linux Qt SDK.

This software is based in part on the work of the Independent JPEG Group.
Qt also includes code from the FreeType Project.

## Provenance

The Qt notices come from the official
[`qt/qtbase` v6.12.0 sources](https://github.com/qt/qtbase/tree/v6.12.0).
The installed SDK identifies Qt Base commit
`025bdad181de241e81bf853c8a2d7bf3d19261a9`. The downloaded attribution files
match the checksums in its `qtbase-6.12.0.source.spdx` source inventory.

`qt_attribution.json` supplies the component names, versions, copyright text,
and license document paths. When it supplies only a license identifier, the
corresponding text comes from Qt's `LICENSES/` directory at the same tag.
Qt Svg's additional XSVG attribution comes from
[`qt/qtsvg` v6.12.0, `src/svg/REUSE.toml`](https://github.com/qt/qtsvg/blob/v6.12.0/src/svg/REUSE.toml),
at commit `883a58532078d003cefef0a549bfe9f885ce41f5`. It covers code in both
Qt Gui and Qt Svg.

The notice file also retains the relevant Apache Tika and PRONOM attribution
excerpts from Tika's `NOTICE.txt` at the exact commit recorded by Qt.

ICU's license comes from
[`unicode-org/icu`, tag `release-73-2`, `icu4c/LICENSE`](https://github.com/unicode-org/icu/blob/release-73-2/icu4c/LICENSE).
The Qt Linux SDK's `qtbase-6.12.0.spdx.json` identifies ICU 73.2, matching its
`libicu*.so.73.2` libraries. This ICU version is separate from the Unicode
character and locale data embedded directly in Qt Core.

Selected versions recorded by Qt are listed below.

| Component | Version |
| --- | --- |
| libpng | 1.6.58 |
| PCRE2 and SLJIT | 10.48 |
| HarfBuzz | 14.5.0 |
| double-conversion | 3.4.0 |
| TinyCBOR | 7.0 |
| MD4C | 0.5.3 |
| libjpeg-turbo | 3.2.0 |
| zlib | 1.3.2 |
| FreeType | 2.14.3 |
| Vulkan Memory Allocator | 3.3.0 |
| D3D12 Memory Allocator | 3.1.0 |
| ICU in the Linux SDK | 73.2 |

## Scope

The selection covers third-party code and data attributed to Qt Core, Gui,
Network, Widgets, PrintSupport, Svg, and their desktop platform dependencies.
It includes embedded algorithms, MIME and Unicode data, XCB input support,
D-Bus headers, Windows tablet and graphics support, and Wayland client and
shell protocols. Source build rules and installed binaries supplement the
SDK SBOMs, which omit seven compiled Wayland protocol attributions and
Qt Svg's XSVG attribution. The KDE appmenu protocol carries LGPL-2.1-or-later,
whose full text is included in `Wayland-NOTICES.txt`.

The set deliberately covers some optional desktop backends as well. Including
FreeType, zlib, OpenGL ES, Vulkan, D3D12, and Windows notices here does not mean
every platform binary contains all those components. In particular, the
Linux SDK uses system FreeType and zlib; the Windows SDK bundles them. Both
bundle HarfBuzz, libpng, libjpeg-turbo, PCRE2, double-conversion, and MD4C.
Linux ICU libraries are
copied into the application's private runtime directory. System libraries
that the installer does not copy are outside this notice selection.

Test fixtures, Qt build tools, SQL drivers, Android, WebAssembly, macOS-only
code, ARM-only Pixman assembly, and Wayland compositor-only protocols are
excluded. Notices for other application dependencies remain in the parent
directory.

The bundled public suffix data is under MPL-2.0. Qt records its original
[source list at commit `e452c7058d6946bd76952b128c12f5ce87a5acb8`](https://github.com/publicsuffix/list/blob/e452c7058d6946bd76952b128c12f5ce87a5acb8/public_suffix_list.dat).
The generated Qt representation is
[`src/3rdparty/libpsl/psl_data.cpp`](https://github.com/qt/qtbase/blob/v6.12.0/src/3rdparty/libpsl/psl_data.cpp).

## Updating

For a Qt update, compare the new SDK's module SBOMs and configuration summary
with the files actually deployed. Recheck the selected desktop attribution
files at that Qt release tag, including referenced license and copyright files,
and retain their text unchanged. Update ICU from the release identified by the
SDK, rather than assuming its version matches Qt. Recheck optional backends
when the deployed plugin set changes.
