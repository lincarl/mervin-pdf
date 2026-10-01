<#
.SYNOPSIS
  Build MuPDF 1.28.5 static libraries on Windows (MSVC) via its VS solution,
  exactly as mervin-pdf's cmake/FindMuPDF.cmake expects.

.DESCRIPTION
  Downloads the official 1.28.5 source release (the tarball ships every
  thirdparty submodule pre-extracted, incl. tesseract/leptonica), builds
  platform\win32\mupdf.sln in Release|x64, and verifies libmupdf.lib landed in
  platform\win32\x64\Release. Writes the source root path to stdout (use it as
  MUPDF_DIR). Idempotent: if libmupdf.lib already exists it skips the build, so
  CI caching short-circuits the multi-minute rebuild.

  Requires a developer environment with msbuild on PATH (e.g. after
  ilammy/msvc-dev-cmd or microsoft/setup-msbuild). The Release config already
  includes Tesseract OCR and the codecs.

.PARAMETER Dest
  MuPDF source root to build in / extract to. Default C:\dev\src\mupdf-1.28.5-source
  (C:\dev is where this project keeps its development tools; the CI job caches the
  same path and so relies on this default).

.PARAMETER NoLtcg
  Build native object files for CI tests, avoiding repeated link-time optimization
  in every test executable. Use a separate Dest from release builds.
#>
param(
    [string]$Dest = "C:\dev\src\mupdf-1.28.5-source",
    [switch]$NoLtcg
)

$ErrorActionPreference = "Stop"
$version = "1.28.5"
$sha256 = "98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934"
$url = "https://mupdf.com/downloads/archive/mupdf-$version-source.tar.gz"
$lib = Join-Path $Dest "platform\win32\x64\Release\libmupdf.lib"
$flavorFile = Join-Path (Split-Path $lib -Parent) "mervin-build-flavor.txt"
$flavor = if ($NoLtcg) { "native" } else { "ltcg" }

if (Test-Path $lib) {
    $builtFlavor = if (Test-Path $flavorFile) { (Get-Content $flavorFile -Raw).Trim() } else { "ltcg" }
    if ($builtFlavor -ne $flavor) {
        throw "MuPDF at $Dest uses $builtFlavor objects; choose a separate -Dest for $flavor objects."
    }
    Write-Host "MuPDF already built at $Dest"
    Write-Output $Dest
    exit 0
}

if (-not (Test-Path (Join-Path $Dest "platform\win32\mupdf.sln"))) {
    $parent = Split-Path $Dest -Parent
    New-Item -ItemType Directory -Force $parent | Out-Null
    $tmpRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
    $tar = Join-Path $tmpRoot ("mupdf-" + [guid]::NewGuid().ToString("N") + ".tar.gz")
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $tar
    $actual = (Get-FileHash -Algorithm SHA256 $tar).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        Remove-Item $tar -Force
        throw "MuPDF archive SHA-256 mismatch: expected $sha256, got $actual"
    }
    if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
    # Extract straight into the parent (NOT a temp dir + Move-Item): a cross-drive
    # Move-Item copies file-by-file and MuPDF's deep thirdparty paths
    # (e.g. zxing-cpp\wrappers\rust\core) overflow Windows' 260-char MAX_PATH.
    # Extracting into the short, same-drive parent avoids the move entirely.
    # bsdtar (tar.exe) ships on windows-2022 runners and handles .tar.gz.
    # The excludes skip thirdparty demo/binding dirs that contain symlinks:
    # creating those needs elevation/Developer Mode on Windows, and a failed
    # symlink makes tar exit 1 even though nothing the build needs is missing.
    & tar -xzf $tar -C $parent `
        --exclude "*/thirdparty/freeglut/progs/*" `
        --exclude "*/thirdparty/zxing-cpp/wrappers/*"
    if ($LASTEXITCODE -ne 0) { throw "tar extraction failed ($LASTEXITCODE)" }
    Remove-Item $tar -Force
    # Filter on the exact version: a wildcard (mupdf-*-source) would match an
    # older MuPDF tree already sitting in the same parent and rename it.
    $src = Get-ChildItem -Path $parent -Directory -Filter "mupdf-$version-source" | Select-Object -First 1
    if (-not $src) { throw "Could not find extracted MuPDF source under $parent" }
    # Top-level directory rename (same parent) - a metadata op, no deep-path copy.
    if ($src.FullName -ne $Dest) { Rename-Item $src.FullName (Split-Path $Dest -Leaf) }
}

$sln = Join-Path $Dest "platform\win32\mupdf.sln"
# Pipe msbuild's output to the host so the success stream carries only the final
# Write-Output $Dest (callers do `$dir = build-mupdf-windows.ps1 | Select -Last 1`).
$buildArgs = @($sln, "/p:Configuration=Release", "/p:Platform=x64", "/p:PlatformToolset=v143", "-m")
if ($NoLtcg) { $buildArgs += "/p:WholeProgramOptimization=false" }
& msbuild @buildArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw "msbuild failed ($LASTEXITCODE)" }
if (-not (Test-Path $lib)) { throw "Build did not produce $lib" }
Set-Content -Path $flavorFile -Value $flavor

Write-Host "Built MuPDF static libs in $(Split-Path $lib -Parent)"
Write-Output $Dest
