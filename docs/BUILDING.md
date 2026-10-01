# Building Mervin PDF

Windows 11, x64, MSVC. The build uses CMake + Ninja, vcpkg (manifest mode) for
most C/C++ deps, Qt 6 from aqtinstall, and **MuPDF built from source** (it is not
in the vcpkg registry).

The pinned dependency set is Qt 6.12.0, MuPDF 1.28.5, qpdf 12.4.2 and toml++
3.4.0. `vcpkg.json` pins the registry revision for qpdf, toml++ and their
dependencies. Linux release packages use the supported distribution's Qt and
qpdf packages; MuPDF is built from the same source release on both platforms.

## Toolchain (one-time)

```powershell
# Compiler + Windows SDK (one UAC prompt)
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-source-agreements --accept-package-agreements `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
winget install --id Kitware.CMake     --scope machine --source winget --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja --source winget --accept-source-agreements --accept-package-agreements
winget install --id NSIS.NSIS         --source winget --accept-source-agreements --accept-package-agreements
winget install --id Python.Python.3.12 --source winget --accept-source-agreements --accept-package-agreements

# Qt 6.12.0 (official prebuilt dynamic DLLs, no Qt account)
# aqtinstall 3.3.0 cannot read Qt 6.11+ Windows repository paths; use the same
# merged fix as the release workflow until it ships in an aqtinstall release.
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m pip install --user --upgrade "git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b"
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 --outputdir C:\dev\Qt
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
# Download + extract the 1.28.5 source release (bundles all thirdparty deps).
curl.exe -L --fail -o C:\dev\src\mupdf-1.28.5-source.tar.gz https://mupdf.com/downloads/archive/mupdf-1.28.5-source.tar.gz
tar -xzf C:\dev\src\mupdf-1.28.5-source.tar.gz -C C:\dev\src
# (A few symlinks in thirdparty wrapper/demo dirs fail to extract on Windows - harmless.)

# Build the core static library (Release|x64). The .sln targets toolset v142;
# retarget to v143. This pulls libthirdparty + harfbuzz + tesseract/leptonica +
# barcode/zxing + pkcs7 + resources as project dependencies.
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  C:\dev\src\mupdf-1.28.5-source\platform\win32\mupdf.sln `
  /t:libmupdf /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
# => libs land in  C:\dev\src\mupdf-1.28.5-source\platform\win32\x64\Release\*.lib
```

`cmake/FindMuPDF.cmake` locates these (default root `C:/dev/src/mupdf-1.28.5-source`,
overridable via the `MUPDF_DIR` env/cache variable) and links all produced `.lib`s.

Notes:
- MuPDF's Release config uses `/MD` (dynamic CRT), matching Qt - do not mix with `/MT`.
- Only the **Release** libs were built; an `x64-debug` app build would need the Debug
  MuPDF libs (`/t:libmupdf /p:Configuration=Debug`).

## Configure & build

The Ninja generator needs the MSVC environment, so launch the VS Dev Shell first.
Two env vars drive the CMake presets.

```powershell
$env:VCPKG_ROOT = "C:\dev\vcpkg"
$env:QT6_DIR    = "C:\dev\Qt\6.12.0\msvc2022_64"
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation
cmake --preset x64-release
cmake --build --preset x64-release
# => build\x64-release\MervinPDF.exe
```

## Run

An unpackaged development build needs the Qt and vcpkg DLLs on `PATH`:

```powershell
$env:Path = "C:\dev\Qt\6.12.0\msvc2022_64\bin;" + $env:Path
# Also needs the vcpkg deps (qpdf etc.) on PATH for a non-deployed dev run:
$env:Path = "build\x64-release\vcpkg_installed\x64-windows\bin;" + $env:Path
.\build\x64-release\MervinPDF.exe "path\to\document.pdf"
```

## Package

Build a self-contained tree and the per-user installer. The release build is the
GUI subsystem (no console window) by default, so no extra flag is needed - just
run the deploy script from a VS Dev Shell with `QT6_DIR` set:

```powershell
cmake --preset x64-release -DMERVIN_VERSION=1.2.3
cmake --build --preset x64-release
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Installer -Version 1.2.3
# => build\x64-release\deploy\           (self-contained: runs with nothing on PATH)
# => build\x64-release\MervinPDF-Setup-<version>.exe   (per-user, no UAC)
```

Official releases derive this value from a stable `vMAJOR.MINOR.PATCH` Git tag;
untagged local builds default to `0.0.0` unless `MERVIN_VERSION` is overridden.

`scripts/deploy.ps1` runs `windeployqt` (Qt DLLs/plugins + VC runtime) and copies
the vcpkg dependency DLLs (qpdf + zlib/jpeg/toml++); MuPDF is statically linked.
`packaging/nsis/mervin.nsi` installs to `%LOCALAPPDATA%\Mervin PDF`, adds a Start
Menu shortcut + Apps & Features entry + uninstaller, and seeds `eng.traineddata`
into `%APPDATA%\MervinPDF\tessdata`. The bundled
`resources/tessdata/eng.traineddata` provides English OCR out of the box.


## Linux development and verification

Install Qt 6.6+ (Core, Gui, Widgets, Network, PrintSupport, Svg, Test), qpdf development
headers, toml++, CMake, Ninja, a C++20 compiler, and Python 3. Build the pinned MuPDF
with `scripts/build-mupdf-linux.sh`, then:

```bash
export MUPDF_DIR=/path/to/mupdf-1.28.5-source
cmake --preset linux-release
cmake --build --preset linux-release --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-release --output-on-failure -LE optional-corpus
```

CMake generates required PDF fixtures from `tests/generate_fixtures.py`. No personal
documents or fixture downloads are needed. Encryption/form tests generate their own
inputs. The optional photographic corpus is described in `examples/README.md`.
Run `ctest -R tst_perf -V` to inspect performance measurements; use the same machine,
build configuration, and workload for comparisons.

For memory and undefined-behavior checks, configure a separate Debug build with
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"` and
`-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`. CI disables leak detection
for the prebuilt Qt/dependency binaries; address and undefined-behavior checks remain enabled.

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
