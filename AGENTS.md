# mervin-pdf - project instructions

## Version control

Commit directly to `main` and push to `origin/main`. **Never create a git
branch** - no feature branches, no PRs, no separate merge step. End commit
messages with the standard `Co-Authored-By` trailer.

## Colours live in exactly one file

Every UI colour is a token in [src/ui/ThemeTokens.cpp](src/ui/ThemeTokens.cpp)
(`theme::chrome()` for the theme-dependent chrome, `theme::doc()` for the
theme-independent on-page surfaces, `theme::brand()` for the app identity). Never
write a colour literal anywhere else - not in a QSS string, not in a `QColor(...)`
in paint code, not in a widget's local stylesheet. `tests/tst_theme.cpp` scans the
generated stylesheet and fails on any literal that is not a token, so a new colour
means a new token.

Three deliberate exceptions, each argued in ThemeTokens.h's header comment:
`annot::palette()` (written into PDF annotation objects), `EmitStyle`'s stroke/fill
in `render/MeasureContent.h` (PDF content-stream operands), and
`comfort::kRampBg/kRampFg` in `render/ComfortTransform.h` (a `constexpr` LUT input,
pinned by `tst_comfort_transform`; `theme::doc().paperComfort` re-exports it so the
viewer does not re-type the value). `resources/icons/make_icon.py` is an offline
generator that cannot include a C++ header and stays hand-synced with `brand()`.

One more trap worth knowing: `QWidget::foregroundRole()` derives a child's text
colour from the **inherited** `backgroundRole()`, and `QPalette::Dark`/`Shadow` map
to `QPalette::Light`. A widget parented to a viewport whose background role is Dark
therefore paints every plain `QLabel` in a bevel colour - invisible in both themes.
`ViewerWidget`'s viewport uses `QPalette::Window` for exactly this reason;
`tests/tst_viewer_preview.cpp` asserts it.

## Repo-wide text sweeps must never touch binaries

Any sweeping substitution - purging a character, renaming a token everywhere -
must be restricted to tracked **text** files. Drive it from `git ls-files`
filtered by extension; never from a recursive filesystem walk, and never with a
tool that rewrites every byte sequence it matches.

This is not hypothetical. An em-dash purge in v1.12.0 (2026-06-10)
rewrote two U+2014 sequences inside the binary
`resources/tessdata/eng.traineddata` to `-`, shrinking it by 4 bytes. That left
Tesseract's LSTM unicharset listing `-` twice and the container's offset table
stale, so the **first OCR in every build from v1.12.0 to v1.44.0 crashed the
whole app** - heap corruption, an access violation or an `abort()`, depending on
heap luck. `tests/tst_tessdata.cpp` guards that file now.

The tracked binary known to contain U+2014 bytes today, and so would be damaged
by a rerun, is `resources/tessdata/eng.traineddata`. Find such bytes with a byte
scan, not a text search, and check `git status` for unexpected `Bin` changes
before committing any sweep.

## Finishing a change (definition of done)

When you finish making code changes, unless the user has said otherwise, always
do the following, in order:

1. **Run the tests** - all of `ctest` must pass (prepend the Qt `bin` and vcpkg
   `bin` dirs to `PATH` in the same command, see Build & installers below).
   Read the **Skipped** list in the ctest summary, not just the pass count: a
   QSKIP exits 0, and the suite only reports skips as skips because
   `tests/CMakeLists.txt` matches QTest's totals line. A case that quietly stopped
   running is how a corrupt shipped `eng.traineddata` hid for six weeks.
   PDF fixtures under `examples/` and `docs/drawing.pdf` are intentionally local,
   so their data-dependent cases skip on a fresh clone. These skips are expected.
2. **Run the performance guardrails and compare** - execute
   `build\x64-release\tst_perf_render.exe` and
   `build\x64-release\tst_perf_startup.exe` directly (their tables also land in
   `build\x64-release\Testing\Temporary\LastTest.log` after a ctest run, but
   running them directly keeps the numbers next to the change). With local PDF
   fixtures present, tst_perf_render times page rasterization and pixel passes;
   tst_perf_startup times a full app startup against an isolated `--profile`.
   Compare the numbers with the
   previous run on this machine; anything that regressed noticeably (say >1.5x
   on a metric the change could plausibly touch) needs investigating before the
   change ships.
3. **Choose the release version** as a stable `vMAJOR.MINOR.PATCH` Git tag.
   Release CI strips the leading `v` and passes that version to CMake, the exe,
   the version header, the Win32 resource, and every installer/package. Untagged
   local builds default to `0.0.0`; override with `-DMERVIN_VERSION=x.y.z`.
4. **Build local release files when needed** - configure with an explicit
   `-DMERVIN_VERSION=x.y.z`, rebuild, then run
   `scripts\deploy.ps1 -Installer -Version x.y.z` from a shell with
   `vcvars64.bat` sourced. The deploy script refuses to package an exe whose
   embedded FileVersion differs from the requested version.
5. **Commit and push** to `main` (no branch). Push the chosen version tag only
   after repository checks pass; the tag workflow builds and publishes every
   installer.

## Continuous integration / all-platform releases

[.github/workflows/ci.yml](.github/workflows/ci.yml) validates repository metadata
and scripts on pushes and pull requests.

[.github/workflows/release.yml](.github/workflows/release.yml) builds **every** release
artifact on GitHub Actions from a stable `vMAJOR.MINOR.PATCH` tag and attaches
them to a GitHub Release: Windows NSIS `.exe` + WiX `.msi`, a Linux **AppImage** (built
on Ubuntu 26.04), a native **`.deb`** (Ubuntu 26.04 container), and an **`.rpm`**
(Fedora container).

> **Ubuntu 26.04 is the minimum supported release, for every Linux artifact.** The `.deb`
> needs it because 24.04 ships Qt 6.4, which lacks the Qt 6.6/6.8 APIs Mervin uses
> (`QPalette::Accent`, `QStyleHints::setColorScheme`). The AppImage used to be built on
> 24.04 so its bundled glibc would also run on 26.04, but that did not work in practice,
> so it is now built on 26.04 as well and no longer covers 24.04. `WindowManager.cpp`
> guards its color-scheme calls with `#if QT_VERSION >= 6.8.0`, but that guard covers only
> those calls - **Qt 6.6 is a hard floor for compiling at all**, because
> `ThemeTokens.cpp` uses `QPalette::Accent` unguarded. Measured on Ubuntu 24.04 (Qt 6.4.2):
> `mervin_core` fails with `'Accent' is not a member of 'QPalette'`.

- MuPDF is built from source **in CI** on each OS (cached) - see
  [scripts/build-mupdf-windows.ps1](scripts/build-mupdf-windows.ps1) (msbuild the VS
  solution) and [scripts/build-mupdf-linux.sh](scripts/build-mupdf-linux.sh)
  (`make build=release USE_TESSERACT=yes HAVE_X11=no HAVE_GLUT=no libs` →
  `libmupdf.a` + `libmupdf-third.a`, with bundled Tesseract OCR).
- Linux deps: **system Qt for all three artifacts** - the AppImage builds against 26.04's
  Qt 6.10 just like the `.deb`, and linuxdeploy copies it into the AppDir, so the bundle
  is still self-contained without an `install-qt-action` pin. `.deb`/`.rpm` link the
  distro Qt and let CPack `SHLIBDEPS`/`AUTOREQ` resolve runtime deps natively. qpdf ≥ 11
  is required (26.04/Fedora satisfy it). Note the asymmetry this creates: Windows still
  pins Qt 6.8.3, so the Linux artifacts now ship a *newer* Qt than Windows.
- **No CI job builds the tests** (`-DMERVIN_BUILD_TESTS=OFF` everywhere, Windows included).
  The pipeline packages artifacts; it never runs `ctest`, so compiling the `tst_`/`dump_`
  targets there is dead weight - on Windows it was 109 of the build step's 125 minutes,
  since every test statically re-links MuPDF+Tesseract. Tests and the `tst_perf_*`
  guardrails are the local pre-ship check (see "Finishing a change"), not a release step.
- Local Windows builds/installers are unchanged - `deploy.ps1` still works exactly as before.

## Build & installers

- **All development tools live under `C:\dev`** - that is the preferred root on this
  machine, and anything installed for this project belongs there. Build env:
  `QT6_DIR=C:\dev\Qt\6.8.3\msvc2022_64`, `VCPKG_ROOT=C:\dev\vcpkg`, MuPDF source
  `C:\dev\src\mupdf-1.28.0-source`, MSVC via VS2022 BuildTools `vcvars64.bat`.
  These env vars are not set in the tool shell - set them (or run `vcvars64.bat`)
  in the same command as the build. (A `C:\devtools\...` path in an older doc or a
  stale CMake cache is from a different machine; `C:\devtools` does not exist here.)
- Incremental build (Ninja):
  `cmd /c '"...\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\x64-release'`
- Kill any running `MervinPDF.exe` before building (the exe is locked) and before
  launching the fresh build (single-instance forwarding hands off to the
  installed copy).
- **Manual testing without touching real user state**: launch the dev build with
  `--profile <dir>` to keep ALL persisted state (settings, recent files, session,
  view state, tessdata) in `<dir>` instead of `%APPDATA%\MervinPDF`. A profile
  instance also forms its own single-instance group, so it opens its own window
  instead of forwarding to a running installed instance - no need to kill the
  installed app just to try the dev build. `--quit-after-startup` makes the app
  exit right after startup completes (used by tst_perf_startup to time startup).
- Installers: `scripts\deploy.ps1 -Installer` (requires `QT6_DIR`) always emits
  BOTH `MervinPDF-Setup-<ver>.exe` (NSIS) and `MervinPDF-<ver>.msi` (WiX) into
  `build\x64-release`. Delete any older files matching those two installer
  patterns before running it; the folder must contain only the latest NSIS and
  MSI installer afterward.
- **Always run `deploy.ps1` from a shell with `vcvars64.bat` sourced.** `windeployqt`
  only stages `vc_redist.x64.exe` (the VC++ runtime) and `dxcompiler.dll`/`dxil.dll`
  (the Direct3D shader compiler) when the full VS + Windows SDK environment is present
  (VC redist dir known, SDK `bin` on PATH). Without it those three are silently dropped
  and the installer comes out ~30 MB smaller but **broken** - the app can fail to launch
  on a clean machine. `deploy.ps1` now asserts these three landed and throws otherwise,
  so a wrong-environment build fails loudly instead of shipping.
- Sanity-check installer size: a correct release is ~70 MB (NSIS .exe) / ~80 MB (MSI).
  A ~40 MB installer means the three files above were dropped - rebuild with the proper
  environment. Smaller is NOT leaner here; it's a defective package.

### Driving the Windows build from WSL

An agent on the WSL side **can** build and test the real Windows binaries. Interop is
enabled (`/proc/sys/fs/binfmt_misc/WSLInterop`); `cmd.exe` is simply not on `PATH`, so
invoke it as `/mnt/c/Windows/System32/cmd.exe /c ...`. Do not conclude interop is
missing from `which cmd.exe` - run something and see.

The toolchain is installed at the `C:\dev` paths above: VS 2022 BuildTools (MSVC
14.44, with `cmake.exe`/`ninja.exe`/`MSBuild.exe` bundled), `C:\dev\Qt\6.8.3\msvc2022_64`,
`C:\dev\vcpkg` (bootstrapped, and its clone does carry the `builtin-baseline` commit
`vcpkg.json` pins), and `C:\dev\src\mupdf-1.28.0-source` (`libmupdf.lib` built).

Env vars do not survive between calls, so each step runs through a `.bat` that sources
`vcvars64.bat` first. Three live in `C:\dev` and are the fastest way back in:
`run-build.bat` (configure + build), `run-ctest.bat`, `run-perf.bat` (the `tst_perf_*`
tables). A Windows process ignores the Linux `PATH`, so DLLs must be put on the Windows
`PATH` inside the `.bat` - not exported from bash.

Native Windows builds run at full speed on `C:\`; the 9p penalty below applies only to
*Linux* processes reading `/mnt/c`.

### Running the tests from WSL

A Linux build gives an agent on the WSL side a fast `ctest` without needing the
Windows toolchain. (Windows interop DOES work here - `/mnt/c/.../cmd.exe` runs fine
by absolute path, it is simply not on `PATH` - so this is a convenience, not the only
option; see the Windows notes above for the real thing.) `~/dev` mirrors the `C:\dev`
convention on ext4 - do NOT put a build tree under `/mnt/c`, which is a 9p mount
measured at ~360x slower than ext4 for the many-small-file writes a compile does.

- Deps: `apt install cmake ninja-build build-essential pkg-config libqpdf-dev
  libtomlplusplus-dev libgl1-mesa-dev libxkbcommon-dev libxcb-cursor0`.
- **Do not use the distro Qt on 24.04** - it is 6.4 and cannot compile `mervin_core`
  (see the Qt 6.6 floor above). Install the pinned Qt with
  `aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O ~/dev/Qt` (aqtinstall in a venv,
  since 24.04's Python refuses a global pip install).
- MuPDF: `scripts/build-mupdf-linux.sh ~/dev/src/mupdf-1.28.0-source` (~35 min on 2
  cores, once - it is idempotent afterwards).
- Configure + run out of tree, so nothing lands in the Windows `build\x64-release`:
  `cmake -S . -B ~/dev/build/mervin-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
  `-DCMAKE_PREFIX_PATH=~/dev/Qt/6.8.3/gcc_64 -DMUPDF_DIR=~/dev/src/mupdf-1.28.0-source`,
  then `QT_QPA_PLATFORM=offscreen ctest --output-on-failure` (there is no display).
- What this does NOT cover, so it never substitutes for the Windows pass: the MSVC
  build, `deploy.ps1` and both installers, the Win32-only paths, and the `tst_perf_*`
  guardrails - step 2 of "Finishing a change" compares against previous runs *on the
  same machine*, and a 2-core WSL container is not comparable to the Windows baseline.
- Expected skips here: `tst_measure`/`tst_measure_export` (untracked `docs/drawing.pdf`)
  and one `tst_form_model` case (`MERVIN_FORM_AES_PDF` not set).
- **Ninja silently misses edits to files under `/mnt/c`.** The 9p mount stamps mtimes
  about two hours behind WSL's own clock, so a source just edited can look *older* than
  the object built from it: `ninja` says "no work to do" and the previous binary is what
  gets tested. `touch` does not help - the new mtime is skewed too. Delete the object (or
  the whole `<target>.dir`) before rebuilding anything you just changed, and distrust any
  run whose result did not change: check that a compile line actually scrolled past. This
  is not hypothetical - it produced a confidently wrong test result while fixing the
  `tst_icons` case below.
