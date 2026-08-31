<#
.SYNOPSIS
  Stage a self-contained Mervin PDF tree (Qt + qpdf DLLs) and optionally build
  the NSIS installer.

.DESCRIPTION
  Runs windeployqt to gather Qt's DLLs/plugins and the VC runtime, copies the
  vcpkg-built qpdf and its dependency DLLs next to the exe, and (if present)
  stages eng.traineddata for the installer to drop into the per-user tessdata
  folder. MuPDF is statically linked into MervinPDF.exe, so no MuPDF DLL is
  needed.

  Run from a VS Dev Shell with QT6_DIR set. The release build is the GUI
  subsystem (no console) by default; this script asserts the staged exe is a
  GUI-subsystem binary and refuses to package a stray console build.

.PARAMETER Installer
  Also build the per-user installers from the staged tree: the NSIS .exe
  (friendly interactive wizard) AND the WiX .msi (managed / silent deployment,
  e.g. `msiexec /i MervinPDF-<ver>.msi /qn`). Both are always produced together.
#>
param(
    [switch]$Installer,
    [string]$Version = $env:MERVIN_VERSION
)

$ErrorActionPreference = "Stop"
$root   = Split-Path $PSScriptRoot -Parent
$build  = Join-Path $root "build\x64-release"
$exe    = Join-Path $build "MervinPDF.exe"
$deploy = Join-Path $build "deploy"

if (-not (Test-Path $exe)) { throw "Build MervinPDF.exe first (cmake --build --preset x64-release)." }
if (-not $env:QT6_DIR)      { throw "QT6_DIR is not set." }

# Release CI passes the version derived from its Git tag; local builds default to
# the CMake fallback. This script does not rebuild, so require the requested
# version to match the executable's embedded FileVersion.
if (-not $Version) { $Version = "0.0.0" }
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "Invalid version: $Version" }
$exeVersion = (Get-Item $exe).VersionInfo.FileVersion
if ($exeVersion -ne $Version) {
    throw ("MervinPDF.exe is stamped $exeVersion but packaging requested $Version. " +
           "Reconfigure and rebuild with -DMERVIN_VERSION=$Version, then rerun deploy.")
}

# The packaged app MUST be a GUI-subsystem binary. A console-subsystem exe makes
# Windows allocate a terminal window behind the app on EVERY launch (Explorer,
# Start menu, file double-click) - the exact bug a packaged build must never ship.
# The release build is GUI-subsystem by default (MERVIN_WIN32_SUBSYSTEM=ON); guard
# here so a stray -DMERVIN_WIN32_SUBSYSTEM=OFF dev build can't slip into a package.
# PE optional-header Subsystem field: 2 = Windows GUI, 3 = Windows console.
$peBytes   = [System.IO.File]::ReadAllBytes($exe)
$peOff     = [System.BitConverter]::ToInt32($peBytes, 0x3C)
$subsystem = [System.BitConverter]::ToUInt16($peBytes, $peOff + 4 + 20 + 68)
if ($subsystem -ne 2) {
    throw ("MervinPDF.exe is a console-subsystem binary (PE subsystem=$subsystem; 2=GUI, " +
           "3=console), which pops a terminal window behind the app. Reconfigure with " +
           "-DMERVIN_WIN32_SUBSYSTEM=ON and rebuild before packaging.")
}

# Fresh staging tree containing just the exe.
if (Test-Path $deploy) { Remove-Item $deploy -Recurse -Force }
New-Item -ItemType Directory -Force $deploy | Out-Null
Copy-Item $exe $deploy

# Qt DLLs, plugins, and the compiler runtime.
& "$env:QT6_DIR\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler `
    --compiler-runtime (Join-Path $deploy "MervinPDF.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }

# windeployqt only stages the VC++ runtime (vc_redist.x64.exe, via --compiler-runtime)
# and the Direct3D shader compiler (dxcompiler.dll / dxil.dll) when it can locate them,
# which needs the full VS + Windows SDK environment: vcvars64.bat sourced so the VC
# redist dir is known and the Windows SDK bin (which holds dxcompiler/dxil) is on PATH.
# Without that environment windeployqt SILENTLY drops all three; the installer still
# builds but is ~30 MB smaller and broken - no bundled VC++ runtime means the app can
# fail to launch on a clean machine. Fail loudly so a wrong-environment build never ships.
$required = 'vc_redist.x64.exe', 'dxcompiler.dll', 'dxil.dll'
$missing  = $required | Where-Object { -not (Test-Path (Join-Path $deploy $_)) }
if ($missing) {
    throw ("windeployqt did not stage: $($missing -join ', '). Run from a VS Dev Shell " +
           "with vcvars64.bat sourced (Windows SDK on PATH) so windeployqt can find the " +
           "VC++ runtime and the Direct3D shader compiler. See CLAUDE.md > Build & installers.")
}

# vcpkg dependency DLLs (qpdf + its deps like z.dll/jpeg, and tomlplusplus).
# Copy them all - names vary (zlib ships as z.dll here) and over-copying is
# harmless. MuPDF is statically linked into the exe, so it needs no DLL.
$vbin = Join-Path $build "vcpkg_installed\x64-windows\bin"
if (Test-Path $vbin) {
    Get-ChildItem (Join-Path $vbin "*.dll") | ForEach-Object { Copy-Item $_.FullName $deploy }
}

# English OCR data the installer seeds into the per-user tessdata folder. The
# copy tracked in the repo (resources\tessdata) is the canonical source so every
# build ships a language out of the box; a developer's own %APPDATA% copy is only
# a fallback. Without this, a fresh machine would package no OCR language at all.
$tessData = $null
$tessRepo = Join-Path $root "resources\tessdata\eng.traineddata"
$tessUser = Join-Path $env:APPDATA "MervinPDF\tessdata\eng.traineddata"
if (Test-Path $tessRepo)     { $tessData = $tessRepo }
elseif (Test-Path $tessUser) { $tessData = $tessUser }
else { Write-Warning "eng.traineddata not found (resources\tessdata or %APPDATA%); the installer will ship without an OCR language." }

# Carry the application and dependency license texts alongside the app.
Copy-Item (Join-Path $root "LICENSE") $deploy
Copy-Item (Join-Path $root "THIRD_PARTY_LICENSES.md") $deploy
Copy-Item (Join-Path $root "licenses") (Join-Path $deploy "licenses") -Recurse

Write-Output "Deployed to: $deploy"
Get-ChildItem $deploy | Select-Object Name | Format-Table -AutoSize

if ($Installer) {
    $makensis = "C:\Program Files (x86)\NSIS\makensis.exe"
    if (-not (Test-Path $makensis)) { $makensis = "C:\Program Files\NSIS\makensis.exe" }
    $outfile = Join-Path $build "MervinPDF-Setup-$Version.exe"
    $nsiArgs = @("/DDEPLOY_DIR=$deploy", "/DVERSION=$Version", "/DOUTFILE=$outfile")
    if ($tessData) { $nsiArgs += "/DTESSDATA=$tessData" }
    $nsiArgs += (Join-Path $root "packaging\nsis\mervin.nsi")
    & $makensis @nsiArgs
    # makensis is a native exe, so a non-zero exit does NOT trip $ErrorActionPreference;
    # check it explicitly (as the WiX step below does) so a failed NSIS build fails the
    # whole deploy loudly instead of leaving a stale/missing .exe beside a fresh .msi.
    if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }
    Write-Output "NSIS installer: $outfile"

    # ---- MSI (WiX) -----------------------------------------------------------
    # Always produced alongside the NSIS .exe. The MSI is the managed/silent
    # counterpart (msiexec /i ... /qn) and is per-user too (no elevation),
    # authored in packaging\wix\mervin.wxs. Built with the WiX v6/v7 `wix` CLI.
    $wix = (Get-Command wix.exe -ErrorAction SilentlyContinue).Source
    if (-not $wix -and (Test-Path "C:\Program Files\WiX Toolset v7.0\bin\wix.exe")) {
        $wix = "C:\Program Files\WiX Toolset v7.0\bin\wix.exe"
    }
    if (-not $wix) {
        throw "WiX CLI (wix.exe) not found. Install it with: winget install -e --id WiXToolset.WiXCLI"
    }
    # WiX v6/v7 gate use behind the OSMF EULA. Accepting is persisted per-user,
    # so this is idempotent; it encodes the project's decision to accept (see
    # packaging\wix\README.md for the licensing note).
    & $wix eula accept wix7 | Out-Null
    $icon = Join-Path $root "resources\icons\mervin.ico"
    $wxs  = Join-Path $root "packaging\wix\mervin.wxs"
    $msi  = Join-Path $build "MervinPDF-$Version.msi"
    $wixArgs = @("build", "-arch", "x64",
                 "-d", "Version=$Version", "-d", "DeployDir=$deploy", "-d", "IconFile=$icon")
    if ($tessData) { $wixArgs += @("-d", "TessData=$tessData") }
    $wixArgs += @("-o", $msi, $wxs)
    & $wix @wixArgs
    if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }
    Write-Output "MSI installer:  $msi"
}
