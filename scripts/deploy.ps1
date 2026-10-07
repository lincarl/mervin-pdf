<#
.SYNOPSIS
  Stage a self-contained Mervin PDF tree (Qt + qpdf DLLs) and optionally build
  the MSI installer.

.DESCRIPTION
  Runs windeployqt to gather Qt's DLLs/plugins, stages app-local VC runtime DLLs, copies the
  vcpkg-built qpdf and its dependency DLLs next to the exe, and (if present)
  stages eng.traineddata for the installer to drop into the per-user tessdata
  folder. MuPDF is statically linked into MervinPDF.exe, so no MuPDF DLL is
  needed.

  Run from a VS Dev Shell with QT6_DIR set. The release build is the GUI
  subsystem (no console) by default; this script asserts the staged exe is a
  GUI-subsystem binary and refuses to package a stray console build.

.PARAMETER Installer
  Also build the per-user WiX .msi from the staged tree. It supports an
  interactive wizard and silent deployment, for example
  `msiexec /i MervinPDF-<ver>.msi /qn`.
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
$metadata = Get-Content (Join-Path $build 'generated\package-version.json') -Raw | ConvertFrom-Json
if ($metadata.version -ne $Version) { throw "Build metadata does not match requested version $Version" }
$msiVersion = $metadata.msi_version
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

# Qt DLLs and plugins. Stage the compiler runtime as DLLs below so the MSI
# works without a separate machine-wide prerequisite installer or elevation.
# --no-translations: Mervin's catalogs, Qt's own strings included, are compiled into the exe.
& "$env:QT6_DIR\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler `
    --no-compiler-runtime (Join-Path $deploy "MervinPDF.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }

# Use only the redistributable release DLLs supplied by the active MSVC
# toolchain. Windows 11 supplies the Universal CRT. App-local runtime updates
# ship with Mervin instead of depending on a separately installed VC runtime.
if (-not $env:VCToolsRedistDir) { throw "VCToolsRedistDir is missing. Run from a VS Dev Shell." }
$crt = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
if (-not (Test-Path $crt)) { throw "MSVC x64 redistributable runtime not found: $crt" }
Get-ChildItem (Join-Path $crt '*.dll') | ForEach-Object { Copy-Item $_.FullName $deploy }

# windeployqt needs the Windows SDK environment to find the Direct3D shader
# compiler. Reject an incomplete payload before building a runnable installer.
$required = 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'dxcompiler.dll', 'dxil.dll'
$missing  = $required | Where-Object { -not (Test-Path (Join-Path $deploy $_)) }
if ($missing) {
    throw ("Deployment did not stage: $($missing -join ', '). Run from a VS Dev Shell " +
           "with the Windows SDK on PATH. See docs/BUILDING.md > Package.")
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
    # The per-user MSI supports interactive and silent installation without
    # elevation. Built with WiX 7 and its matching UI extension.
    $wix = (Get-Command wix.exe -ErrorAction SilentlyContinue).Source
    if (-not $wix -and (Test-Path "C:\Program Files\WiX Toolset v7.0\bin\wix.exe")) {
        $wix = "C:\Program Files\WiX Toolset v7.0\bin\wix.exe"
    }
    if (-not $wix) {
        throw "WiX CLI (wix.exe) not found. Install WiX 7 and its UI extension as described in docs/BUILDING.md."
    }
    # WiX 7 gates use behind the OSMF EULA. Accepting is persisted per-user,
    # so this is idempotent; it encodes the project's decision to accept (see
    # packaging\wix\README.md for the licensing note).
    & $wix eula accept wix7 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Accepting the WiX EULA failed ($LASTEXITCODE)" }
    $icon = Join-Path $root "resources\icons\mervin-icon.ico"
    $wxs  = Join-Path $root "packaging\wix\mervin.wxs"
    $msi  = Join-Path $build "MervinPDF-$Version.msi"
    # Remove only empty directories owned by this payload on uninstall. Derive
    # the list from staging so new Qt plugin directories cannot be left behind.
    $cleanupFile = Join-Path $build 'generated\installer-remove-folders.wxi'
    $cleanup = @('<Include xmlns="http://wixtoolset.org/schemas/v4/wxs">',
                 '  <RemoveFolder Id="RemoveInstallFolder" Directory="INSTALLFOLDER" On="uninstall" />')
    $index = 0
    foreach ($dir in Get-ChildItem $deploy -Directory -Recurse | Sort-Object FullName) {
        $relative = $dir.FullName.Substring($deploy.Length + 1)
        $escaped = [System.Security.SecurityElement]::Escape($relative)
        $cleanup += "  <RemoveFolder Id=`"RemovePayloadFolder$index`" Directory=`"INSTALLFOLDER`" Subdirectory=`"$escaped`" On=`"uninstall`" />"
        $index++
    }
    $cleanup += '</Include>'
    Set-Content -Path $cleanupFile -Value $cleanup -Encoding utf8
    $wixArgs = @("build", "-arch", "x64", "-ext", "WixToolset.UI.wixext",
                 "-d", "Version=$msiVersion", "-d", "DisplayVersion=$Version",
                 "-d", "DeployDir=$deploy", "-d", "IconFile=$icon",
                 "-d", "DirectoryCleanup=$cleanupFile")
    if ($tessData) { $wixArgs += @("-d", "TessData=$tessData") }
    $wixArgs += @("-o", $msi, $wxs)
    & $wix @wixArgs
    if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }
    Write-Output "MSI installer:  $msi"
}
