# Building Mervin PDF

Windows 11, x64, MSVC. The build uses CMake + Ninja, vcpkg (manifest mode) for
most C/C++ deps, Qt 6 from aqtinstall, and **MuPDF built from source** (it is not
in the vcpkg registry).

The pinned dependency set is Qt 6.12.0, MuPDF 1.28.6, qpdf 12.4.2 and toml++
3.4.0. `vcpkg.json` pins the 2026.10.16 registry release for qpdf, toml++ and
their dependencies, including zlib 1.3.2, libjpeg-turbo 3.2.0 and libspng 0.7.4.
Linux release packages bundle the same upstream Qt and vcpkg library versions.
MuPDF is built from the same source release on both platforms.
The minimum supported qpdf is 11.10.0, whose Flate decoder limit is required.
The Ubuntu 24.04 compatibility job builds that verified source release because
its distribution package is older. Current bundled Linux and Windows jobs also
exercise qpdf 12.

Windows vcpkg libraries use the `x64-windows-static-md` target triplet. This
links qpdf, toml++ and their library dependencies statically while keeping the
Microsoft C/C++ runtime dynamic (`/MD`, or `/MDd` for Debug), matching the
prebuilt Qt and MuPDF libraries. Build tools use the separate `x64-windows`
host triplet. Qt, its plugins and the redistributable Microsoft runtime remain
DLLs. Linux uses `x64-linux-dynamic` and keeps its bundled shared libraries in
Mervin's private application directory. Linux must not use static qpdf with
MuPDF's unprefixed static JPEG implementation. MuPDF's hidden symbols and the
shared qpdf dependencies keep their different JPEG ABIs separate.

## Bundled dependency limits

The official Qt 6.12.0 binaries include libpng 1.6.58. They do not contain the
[libpng 1.6.59 fix for CVE-2026-46675](http://www.libpng.org/pub/png/libpng.html).
They also include PCRE2 10.48, which predates the
[10.49 security fix](https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49).
The PCRE2 advisory requires attacker-controlled regular expressions; Mervin's
current patterns are fixed. These observations do not establish an exploit
through the application. The project retains official Qt binaries, so replacing
separate vcpkg libraries does not fix Qt's embedded copies. A later Qt release or
a custom Qt build is required to replace them.

MuPDF 1.28.6 also retains its upstream-selected third-party bundle and Mervin's
existing patches. Its Tesseract 5.5.2 predates the model-deserialization fixes in
[5.5.3](https://github.com/tesseract-ocr/tesseract/releases/tag/5.5.3), and its
MuJS 1.3.10 predates the fixes in
[1.3.11](https://github.com/ArtifexSoftware/mujs/compare/1.3.10...1.3.11).
Mervin does not enable PDF JavaScript. Its OCR model-container checks do not
replace Tesseract's internal neural-network validation. Independent updates must
preserve Artifex's integration changes and pass both platform builds. MuPDF 1.28.6
updates its Brotli fork to a revision after 1.2.1.

Bundled Linux libraries receive updates through new Mervin packages rather than
the distribution's Qt or qpdf packages. Core operating-system and display-driver
libraries remain system dependencies. The supported distribution baselines and
compiler toolchains remain separate from the upstream application-library pins.
The latest released vcpkg registry also retains its tested Meson 1.9.0,
NASM 3.01 and Windows pkgconf 3.0.7 acquisition helpers. These build-only tools
are older than their individual upstream releases and are not shipped with
Mervin. Updating them requires vcpkg port changes beyond the registry update.

## Toolchain (one-time)

```powershell
# Compiler + Windows SDK (one UAC prompt)
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-source-agreements --accept-package-agreements `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
winget install --id WiXToolset.WiXCLI --version 7.0.0.0 --source winget --accept-source-agreements --accept-package-agreements
wix eula accept wix7
wix extension add --global WixToolset.UI.wixext/7.0.0
wix extension add --global WixToolset.Util.wixext/7.0.0
winget install --id Python.PythonInstallManager --source winget --accept-source-agreements --accept-package-agreements
# Open a new shell if pymanager is not yet on PATH.
pymanager install 3.15.1
pymanager exec -V:3.15 -m venv C:\dev-tools\mervin-build-venv
$env:Path = "C:\dev-tools\mervin-build-venv\Scripts;" + $env:Path
# Run from the repository. Downloads verified CMake 4.4.4 and Ninja 1.13.2 wheels.
python scripts\install-build-tools.py

# Qt 6.12.0 (official prebuilt dynamic DLLs, no Qt account)
# aqtinstall 3.3.0 cannot read Qt 6.11+ Windows repository paths; use the same
# merged fix as the release workflow until it ships in an aqtinstall release.
python -m pip install --upgrade "git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b" "py7zr==1.1.4"
# py7zr 1.1.4 fixes archive extraction vulnerabilities. CI overrides the Qt
# installation action's older default and uses Python 3.15.1.
# Without --archives aqt installs every archive, including the qttools,
# qtdeclarative and qttranslations ones the UI translations need (see below).
python -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 --outputdir C:\dev\Qt
# => C:\dev\Qt\6.12.0\msvc2022_64

# vcpkg
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
# From the Mervin source directory, align the registry and tool with the manifest.
$baseline = (Get-Content vcpkg.json | ConvertFrom-Json).'builtin-baseline'
git -C C:\dev\vcpkg checkout --detach $baseline
C:\dev\vcpkg\bootstrap-vcpkg.bat
```

## MuPDF from source (not in vcpkg)

```powershell
# Run from a VS Dev Shell. Use a new destination to retain earlier MuPDF builds.
$env:MUPDF_DIR = & scripts\build-mupdf-windows.ps1 -Dest C:\dev\src\mupdf-1.28.6-static-deps-source | Select-Object -Last 1
# => platform\win32\x64\Release\*.lib below MUPDF_DIR
```

The script verifies the official source archive, applies Mervin's MuPDF patches,
builds the libraries with the v143 toolset, and enables MuPDF's
`FZ_HIDE_INTERNAL_JPEG` symbol prefix. MuPDF bundles IJG JPEG 10, while qpdf's
libjpeg-turbo dependency uses the JPEG 6b ABI. Their identically named symbols
cannot safely share one statically linked executable. The prefix keeps MuPDF's
JPEG implementation separate. Linux builds do not use the prefix. They hide all of
MuPDF's symbols instead, as described in
[Linux development and verification](#linux-development-and-verification).

Rebuild MuPDF when migrating from the dynamic-vcpkg build. The script refuses
cached libraries from the older build configuration. Use a separate destination
instead of replacing libraries another checkout still uses. For faster test
builds, add `-NoLtcg` and use another destination. That mode retains JPEG symbol
isolation while avoiding repeated link-time optimization in each test executable.

`cmake/FindMuPDF.cmake` locates and links the `.lib` files under `MUPDF_DIR`.

Notes:
- MuPDF's Release config uses `/MD` (dynamic CRT), matching Qt - do not mix with `/MT`.
- Only the **Release** libs were built; an `x64-debug` app build would need the Debug
  MuPDF libs (`/t:libmupdf /p:Configuration=Debug`).

### MuPDF patches

Both build scripts apply the patches in `scripts/mupdf-patches` to the MuPDF
source with `git apply`, so they need `git` on `PATH`. The patches fix MuPDF
defects that the pinned release still has (see
[MuPDF patches](design.md#mupdf-patches)). The scripts record the patch set's
SHA-256 in `mervin-patch-set.sha256` beside the libraries, and
`cmake/FindMuPDF.cmake` stops with an error when that file is missing or names
another patch set. The scripts delete that file before they change the source or
rebuild, so an interrupted build never counts as patched.

Builds made before Mervin carried patches, and builds with an older patch set,
need the script run again with the same destination (and `-NoLtcg` on Windows if
the destination was built with it). The script then applies the current patches,
first reverting an older set from the copies it keeps in `mervin-patches` in the
destination, and rebuilds only the files they change. When the current patches do
not apply, the script stops and leaves the source at the patch set it records,
putting back a set it has just reverted. If they do not apply to the unchanged
release either, for example because the MuPDF source tree was modified, delete
that destination and run the script again.

## Configure & build

The Ninja generator needs the MSVC environment, so launch the VS Dev Shell first.
Set the Qt, vcpkg and MuPDF roots before configuring.

```powershell
$env:VCPKG_ROOT = "C:\dev\vcpkg"
$env:QT6_DIR    = "C:\dev\Qt\6.12.0\msvc2022_64"
$env:MUPDF_DIR  = "C:\dev\src\mupdf-1.28.6-static-deps-source"
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation
cmake --preset x64-release
cmake --build --preset x64-release
# => build\x64-release\MervinPDF.exe
```

When migrating an existing dynamic-vcpkg build, move its build directory aside
and configure a fresh one. Merely changing the triplet or using `cmake --fresh`
can leave obsolete DLLs beside old executables. Rebuild the application and its
deployment tree together. Do not remove DLLs from an old dynamic executable's
deployment tree and treat it as a static build.

## Run

An unpackaged development build needs the Qt DLLs on `PATH`. It does not need
a vcpkg runtime directory:

```powershell
$env:Path = "C:\dev\Qt\6.12.0\msvc2022_64\bin;" + $env:Path
.\build\x64-release\MervinPDF.exe --profile "C:\dev-temp\mervin-review-abcd\profile" "path\to\generated-test.pdf"
```

## Package

Build a self-contained tree and the per-user MSI installer. The release build is the
GUI subsystem (no console window) by default, so no extra flag is needed - just
run the deploy script from a VS Dev Shell with `QT6_DIR` set:

```powershell
cmake --preset x64-release -DMERVIN_VERSION=1.2.3
cmake --build --preset x64-release
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Installer -Version 1.2.3
# => build\x64-release\deploy\           (self-contained: runs with nothing on PATH)
# => build\x64-release\MervinPDF-<version>.msi       (per-user, no UAC)
```

Stable releases derive this value from a `vMAJOR.MINOR.PATCH` Git tag. Automatic
prereleases use `vMAJOR.MINOR.PATCH-rcN`, starting at `rc1` and retaining the same
target version until the stable release. Local builds accept either form without
the `v` prefix and default to `0.0.0` unless `MERVIN_VERSION` is overridden.

After CI passes for a push to `main`, the packaging workflow assigns the next RC
tag and publishes a GitHub prerelease. Publish the numeric stable tag only when
the user asks and CI has passed. The in-app updater offers only stable releases,
including the stable version that succeeds an installed candidate with the same
base version. See [RELEASING.md](RELEASING.md) for the full policy and package
version mapping.

`scripts/deploy.ps1` runs `windeployqt` to preserve Qt's DLLs and runtime-loaded
plugins. It copies no vcpkg DLLs. MuPDF and the required vcpkg libraries are
linked into the executable. Qt and Microsoft binaries retain their original
embedded signatures.
It skips Qt's translation files, because the UI catalogs, Qt's own strings
included, are compiled into `MervinPDF.exe`.
The script copies the active MSVC toolchain's redistributable runtime DLLs beside
the app. No separate Visual C++ runtime installation or administrator access is
needed. Runtime security updates must ship in new Mervin releases.
The existing application and dependency notices remain in the deployment tree.
The deployment script also copies the target vcpkg packages' copyright files
into `licenses/vcpkg`, including notices for statically linked code.
`packaging/wix/mervin.wxs` installs to `%LOCALAPPDATA%\Mervin PDF` by default,
adds a Start menu shortcut and an Installed apps entry. Installers and Linux
packages include no OCR language models. The application downloads models into
the writable user profile. The interactive wizard lets users choose the installation
folder and launch the app when installation finishes. Silent installs do not launch the app. See
[WiX packaging](../packaging/wix/README.md) for install, repair, and uninstall
commands.

## Microsoft Store package

For a Microsoft Store upload, package the same deployed files as MSIX. Use a
nonzero major version and build the executable with that version first.
The Windows SDK supplies MakeAppx and MakePri. MakePri indexes the icon variants
in `resources.pri`, as described in the [MSIX notes](../packaging/windows/msix/README.md).

```powershell
scripts\build-msix.ps1 -Version 1.2.3
# => build\x64-release\MervinPDF-1.2.3-x64.msix
```

This package is unsigned and intended for Partner Center. Store certification
supplies its distribution signature. Do not install the unsigned file or submit
a package signed with a local test certificate. See
[RELEASING.md](RELEASING.md#microsoft-store-packages) for the workflow and version
rules. Native lifecycle verification uses `scripts/test-windows-msix.ps1` on
a disposable Windows account or hosted runner.

## Windows payload verification

Deployment runs `scripts/check-windows-payload.py` before building the MSI and
writes `build/x64-release/windows-payload.json`. The check inventories every
packaged EXE and DLL, reads ordinary and delayed PE imports, and rejects missing
dependencies without searching development directories or `PATH`. It also checks
required runtime-loaded Qt plugins and graphics libraries. These cannot all be
discovered from the executable's import table alone.

The payload must contain and import none of `tomlplusplus-3.dll`, `qpdf30.dll`,
`jpeg62.dll`, `turbojpeg.dll`, `spng.dll` or `z.dll`. On Windows, rerun the check
against the staged or extracted installer directory with:

```powershell
python scripts/check-windows-payload.py --directory build/x64-release/deploy --report build/x64-release/windows-payload.json
```

An off-Windows inspection must provide an actual Windows system directory with
`--system-directory`. Passing this check does not verify Authenticode trust or
application-control policy acceptance. `MervinPDF.exe` and the MSI remain
unsigned. A policy that requires trusted application binaries still needs an
appropriate signing or administrator approval process outside this build change.

Run the existing tests against the rebuilt application libraries. The focused
cases cover settings parsing and saving, qpdf operations, document output and
rendering. The `tst_qpdf::jpegDecodersCoexist` case, which Linux also runs,
encodes a JPEG with Qt and decodes it with qpdf and MuPDF in one process to catch
JPEG ABI collisions. CI runs the complete required suite:

```powershell
ctest --test-dir build/x64-release --output-on-failure -R '^(tst_settings_store|tst_qpdf|tst_document|tst_document_output|tst_render_lifetime|tst_measure_export)$'
```

Use a disposable Windows VM or account for installation and launch checks. Keep
development and dependency directories out of the launched application's `PATH`,
use a separate `--profile` directory, and open only generated test PDFs. The
existing `scripts/test-windows-installer.ps1` exercises silent installation,
startup, upgrade, downgrade rejection, repair and uninstall. It requires a
disposable account or hosted CI runner and refuses existing Mervin installations
and data. `scripts/test-windows-installer-ui.ps1` exercises the interactive wizard
on a disposable GitHub-hosted runner. See the [installer guide](../packaging/wix/README.md)
for their requirements. Do not run installer tests on an installed daily-driver
copy or change real PDF associations. PE inspection alone does not establish that
the native installer and application work.


## Linux development and verification

Release builds use the `linux-bundled` preset with official Qt 6.12.0 and the
manifest's vcpkg libraries. Install a C++20 compiler, git, curl, tar, zip, unzip,
pkg-config, NASM and patchelf. Qt Gui configuration also needs OpenGL development
headers and linker files, provided by `libgl1-mesa-dev` on Ubuntu or
`mesa-libGL-devel` on Fedora. Install the distribution's X11/XCB, xkbcommon,
font, GTK 3, CUPS and Wayland runtime packages. Keep Ubuntu 26.04 as the minimum
package baseline. Fedora packages build on Fedora 44. The compiler and
operating-system libraries remain managed by those supported distributions.

Use a separate Python environment for build tools. The portable Python helper
requires a new destination and does not replace the system interpreter. Its
recorded extension compiler is Clang. The example selects GCC explicitly when
pip builds archive-extraction extensions that have no Python 3.15 wheels.

```bash
build_python="$(scripts/bootstrap-python-linux.sh "$HOME/dev-tools/mervin-python-3.15.1")"
"$build_python" -m venv "$HOME/dev-tools/mervin-build-venv"
source "$HOME/dev-tools/mervin-build-venv/bin/activate"
python scripts/install-build-tools.py
CC=gcc CXX=g++ LDSHARED="gcc -shared" \
python -m pip install "git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b" "py7zr==1.1.4"
python -m aqt install-qt linux desktop 6.12.0 linux_gcc_64 --outputdir "$HOME/dev-tools/Qt"
export QT6_DIR="$HOME/dev-tools/Qt/6.12.0/gcc_64"
export VCPKG_ROOT="$(scripts/bootstrap-vcpkg-linux.sh)"
export MUPDF_DIR="$(scripts/build-mupdf-linux.sh "$HOME/dev-tools/mupdf-1.28.6-source")"
cmake --preset linux-bundled
cmake --build --preset linux-bundled --parallel 2
python scripts/fetch-test-font.py build/linux-bundled/test-fonts
python scripts/fetch-test-tessdata.py build/linux-bundled/test-tessdata
QT_QPA_PLATFORM=offscreen \
LD_LIBRARY_PATH="$PWD/build/linux-bundled/vcpkg_installed/x64-linux-dynamic/lib" \
MERVIN_TEST_FONT="$PWD/build/linux-bundled/test-fonts/NotoSansCJKsc-Regular.otf" \
ctest --test-dir build/linux-bundled --output-on-failure -LE optional-corpus
```

MuPDF's helper applies the current [patches](#mupdf-patches). Run it again at the
same destination when CMake reports an older patch set. Existing local development
builds can continue using the `linux-release` preset with compatible system
libraries, including Qt 6.9 or newer and qpdf 11.10 or newer. That preset does not
produce the bundled release layout.

The build-tree test command supplies the vcpkg library path because Qt's own
search path can otherwise select the system zlib. Installed packages use their
private libraries without this environment variable.

The bundled install keeps its executable, shared libraries and Qt plugins under
`/usr/lib/mervin-pdf`, with a launcher at `/usr/bin/MervinPDF`. The application
uses relative library search paths and its own `qt.conf`. It does not replace
system Qt files or export library paths to applications it launches.

Stage and inspect a package without installing it on the development machine.

```bash
DESTDIR="$PWD/dev-temp/AppDir" cmake --install build/linux-bundled --prefix /usr
python scripts/check-linux-runtime.py dev-temp/AppDir/usr
cpack --config build/linux-bundled/CPackConfig.cmake -G DEB -B dev-temp/packages
# On a Fedora builder, use -G RPM instead.
bash scripts/make-appimage.sh 0.0.0 dev-temp/AppDir
```

The AppImage uses the same private runtime as the native packages. Packaging
checks verify library resolution and startup with an isolated profile. Native
DEB/RPM dependency metadata describes external operating-system libraries rather
than requiring a second Qt or qpdf from the distribution.

CMake generates required PDF fixtures from `tests/generate_fixtures.py`. No personal
documents are needed. Encryption/form tests generate their own inputs. Real OCR
tests use a pinned English `tessdata_best` model fetched by
`scripts/fetch-test-tessdata.py` into the build directory. CI fetches this test-only
model on both platforms. Set `MERVIN_TEST_TESSDATA_DIR` to use a different test
model directory. Test data is never included in application packages. The optional
photographic corpus is described in `examples/README.md`.
Run `ctest -R tst_perf -V` to inspect performance measurements; use the same machine,
build configuration, and workload for comparisons.

For memory and undefined-behavior checks, configure a separate Debug build with
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"` and
`-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`. CI disables leak detection
for the prebuilt Qt/dependency binaries; address and undefined-behavior checks remain enabled.

MuPDF's archives bundle libjpeg, FreeType, zlib and Brotli. Shared qpdf,
Qt and Qt's plugins must resolve their own dependencies separately.
`cmake/FindMuPDF.cmake` links the archives with `-Wl,--exclude-libs`, which keeps
all of MuPDF's symbols out of the executables' dynamic symbol tables. Without it,
GNU ld exports MuPDF's definitions of the names those libraries use, the dynamic
linker binds the libraries to MuPDF's copies, and colour printing writes empty
page images. The `linux_symbol_exports` test runs `scripts/check-linux-exports.py`
on the application and three test executables. The Linux release jobs build
without tests and run the script on the application:

```bash
python3 scripts/check-linux-exports.py build/linux-release/MervinPDF
```

CI's `linux-tests` jobs retain Ubuntu 24.04 and qpdf 11.10 to test minimum-version
compatibility, with official Qt 6.12.0. Those older qpdf binaries are test-only.
The `linux-system-qt-tests` job keeps its historical identifier for the required
CI gate, but now builds and tests the upstream libraries on Ubuntu 26.04. It also
checks a staged bundled runtime. AppImage and DEB packages use that dependency
set. RPM packages build it on Fedora 44 and check exported symbols and the staged
runtime. Native Windows CI verifies the application, MSI and MSIX installers.

## Qt modules for the UI translations

The build embeds the UI catalogs in `i18n/`, Qt's own catalogs and the standard
widget supplements in `i18n/qt/`. It needs Qt's LinguistTools (`lupdate`, `lrelease`)
and Qt's translation catalogs (`qtbase_<id>.qm`). Configure stops with an error
when a language has neither an installed Qt catalog nor a maintained supplement.
Packages and installers need neither at run time.

| Qt install for local development | What to add |
| --- | --- |
| aqtinstall | Nothing for a plain `install-qt`, which installs every archive. With `--archives`, include `qttools`, `qtdeclarative` (`lupdate` and `lrelease` link Qt Qml) and `qttranslations`. |
| Ubuntu 26.04 | `qt6-tools-dev`, which pulls `qt6-tools-dev-tools` and `qt6-l10n-tools` (`lprodump`, `lupdate`, `lrelease`; `qt6-l10n-tools` alone lacks the CMake package), and `qt6-translations-l10n`. |
| Fedora 44 | `qt6-qttools-devel`, which requires `qt6-linguist`, and `qt6-qttranslations`. |

The normal build never changes `i18n/*.ts`. After changing UI text, rewrite them
from the sources (use `--preset x64-release` on Windows):

```bash
cmake --build --preset linux-release --target update_translations
```

Then translate the new strings as described in [TRANSLATING.md](TRANSLATING.md).
The `i18n_catalogs` test fails until the catalogs match the sources and every
message is finished.

## Repeatable corpus profiling

`mervin_benchmark` is built with the tests but runs only when explicitly invoked.
Supply local PDFs; it does not download files or modify them:

```bash
build/linux-release/tests/mervin_benchmark /path/to/*.pdf > before.json
# Rebuild after a change, then run the same files:
build/linux-release/tests/mervin_benchmark /path/to/*.pdf > after.json
python3 scripts/compare-benchmarks.py before.json after.json
```

Each file records its SHA-256, page count, five open times, five render times per
case, rendered pixel hashes, and exact matches for a whole-word search for “the”.
Render cases use the first, quarter, middle, three-quarter and last pages, in Light
and Comfort at 1.5x full-page and 5x clipped to a 1200×900 region. The first search
includes extraction; subsequent searches use cached text. Times include worker
queue/delivery overhead but exclude hashing. `first_ms` is the first measured run,
**not** a cold filesystem-cache measurement; five samples make p95 the maximum.
The comparison script rejects changed pixels, search results or workloads, and
reports timings without treating noisy wall-clock results as correctness failures.

The October 2026 pass used these public documents locally (no PDFs are committed):

| Input | Pages | SHA-256 |
| --- | ---: | --- |
| [LaTeX introduction, CTAN](https://mirrors.ctan.org/info/lshort/english/lshort.pdf) | 153 | `ecef13f225de55549ee5214f70cca30998f3251dee65c6b00931aae18b7736c2` |
| [NASA HECC December 2024 report](https://www.nas.nasa.gov/hecc/assets/monthlies/pdf/HECC_12-24.pdf) | 51 | `27f38757d3be658a421072bcf92da39d2f2b272ce6a997ff1ce64fab7c80c079` |
| [NASA scanned technical report](https://ntrs.nasa.gov/api/citations/19970026889/downloads/19970026889.pdf) | 164 | `e5e7cb62e14fe01d95746b8ead479c067338f3540e62b6ef689bf2a0ca6e98e9` |

On Ubuntu 26.04, GCC 15 Release, Qt 6.12.0 and MuPDF 1.28.5, with four exposed
Core 3 100U CPUs, all 60 before/after render hashes and all search results matched.
Baseline median opens were 2.15/0.95/1.27 ms respectively; first searches were
124.6/75.4/404.6 ms and cached searches 1.20/0.15/1.55 ms. This sample covers text,
illustrations and scans; it is not an exhaustive PDF compatibility corpus.

The focused `tst_perf_render imageRegionPass` case (128 image regions, 1.92 MP,
15 samples) measured 3.33 → 2.94 ms median after reusing row plans between rectangle
boundaries. A follow-up alternating old/new transforms in the same process (five
warm-ups, 30 measured pairs) measured 2.28 → 1.85 ms at 1.92 MP and 4.52 → 3.82 ms
at 3.15 MP for 128 regions, with identical pixels. Whole-document timings varied
substantially, including in unchanged
Light rendering, so they do not establish an overall rendering speedup. The
10,000-page layout benchmark separately verifies indexed lookup against a full
scan. Run benchmarks with competing builds stopped and repeat before drawing
performance conclusions on another machine.
