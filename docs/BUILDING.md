# Building Mervin PDF

Windows 11, x64, MSVC. The build uses CMake + Ninja, vcpkg (manifest mode) for
most C/C++ deps, Qt 6 from aqtinstall, and **MuPDF built from source** (it is not
in the vcpkg registry).

## Toolchain (one-time)

```powershell
# Compiler + Windows SDK (one UAC prompt)
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-source-agreements --accept-package-agreements `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
winget install --id Kitware.CMake     --scope machine --source winget --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja --source winget --accept-source-agreements --accept-package-agreements
winget install --id NSIS.NSIS         --source winget --accept-source-agreements --accept-package-agreements
winget install --id Python.Python.3.12 --source winget --accept-source-agreements --accept-package-agreements

# Qt 6.8.3 (official prebuilt dynamic DLLs, no Qt account)
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m pip install --user aqtinstall
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir C:\dev\Qt
# => C:\dev\Qt\6.8.3\msvc2022_64

# vcpkg
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
```

## MuPDF from source (not in vcpkg)

```powershell
# Download + extract the 1.28.0 source release (bundles all thirdparty deps).
curl.exe -L --fail -o C:\dev\src\mupdf-1.28.0-source.tar.gz https://mupdf.com/downloads/archive/mupdf-1.28.0-source.tar.gz
tar -xzf C:\dev\src\mupdf-1.28.0-source.tar.gz -C C:\dev\src
# (A few symlinks in thirdparty wrapper/demo dirs fail to extract on Windows - harmless.)

# Build the core static library (Release|x64). The .sln targets toolset v142;
# retarget to v143. This pulls libthirdparty + harfbuzz + tesseract/leptonica +
# barcode/zxing + pkcs7 + resources as project dependencies.
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  C:\dev\src\mupdf-1.28.0-source\platform\win32\mupdf.sln `
  /t:libmupdf /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
# => libs land in  C:\dev\src\mupdf-1.28.0-source\platform\win32\x64\Release\*.lib
```

`cmake/FindMuPDF.cmake` locates these (default root `C:/dev/src/mupdf-1.28.0-source`,
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
$env:QT6_DIR    = "C:\dev\Qt\6.8.3\msvc2022_64"
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation
cmake --preset x64-release
cmake --build --preset x64-release
# => build\x64-release\MervinPDF.exe
```

## Run

An unpackaged development build needs the Qt and vcpkg DLLs on `PATH`:

```powershell
$env:Path = "C:\dev\Qt\6.8.3\msvc2022_64\bin;" + $env:Path
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
