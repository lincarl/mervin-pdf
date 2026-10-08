<#
.SYNOPSIS
  Build MuPDF 1.28.5 static libraries on Windows (MSVC) via its VS solution,
  exactly as mervin-pdf's cmake/FindMuPDF.cmake expects.

.DESCRIPTION
  Downloads the official 1.28.5 source release (the tarball ships every
  thirdparty submodule pre-extracted, incl. tesseract/leptonica), builds
  platform\win32\mupdf.sln in Release|x64, and verifies libmupdf.lib landed in
  platform\win32\x64\Release. Writes the source root path to stdout (use it as
  MUPDF_DIR). Enables MuPDF's bundled JPEG symbol prefix so static qpdf can use
  its own JPEG ABI. Reuses only builds with the matching build-flavor marker.

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
$flavor = if ($NoLtcg) { "native-jpeg-prefix-v1" } else { "ltcg-jpeg-prefix-v1" }

if (Test-Path $lib) {
    $builtFlavor = if (Test-Path $flavorFile) { (Get-Content $flavorFile -Raw).Trim() } else { "unmarked" }
    if ($builtFlavor -ne $flavor) {
        throw "MuPDF at $Dest has build flavor $builtFlavor. Run this script with a separate -Dest to build $flavor."
    }
    Write-Host "MuPDF already built at $Dest"
    Write-Output $Dest
    exit 0
}

if (-not (Test-Path (Join-Path $Dest "platform\win32\mupdf.sln"))) {
    if ((Test-Path $Dest) -and (Get-ChildItem $Dest -Force | Select-Object -First 1)) {
        throw "Destination $Dest is not an empty MuPDF source directory. Choose a new -Dest."
    }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $tmpRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
    $tar = Join-Path $tmpRoot ("mupdf-" + [guid]::NewGuid().ToString("N") + ".tar.gz")
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $tar
    $actual = (Get-FileHash -Algorithm SHA256 $tar).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        Remove-Item $tar -Force
        throw "MuPDF archive SHA-256 mismatch: expected $sha256, got $actual"
    }
    # Extract directly into the chosen directory. Stripping the archive's root
    # avoids overwriting another MuPDF checkout beside a custom -Dest and avoids
    # a cross-drive move of MuPDF's deep thirdparty paths.
    # bsdtar (tar.exe) ships on windows-2022 runners and handles .tar.gz.
    # The excludes skip thirdparty demo/binding dirs that contain symlinks:
    # creating those needs elevation/Developer Mode on Windows, and a failed
    # symlink makes tar exit 1 even though nothing the build needs is missing.
    & tar -xzf $tar -C $Dest --strip-components 1 `
        --exclude "*/thirdparty/freeglut/progs/*" `
        --exclude "*/thirdparty/zxing-cpp/wrappers/*"
    if ($LASTEXITCODE -ne 0) { throw "tar extraction failed ($LASTEXITCODE)" }
    Remove-Item $tar -Force
    if (-not (Test-Path (Join-Path $Dest "platform\win32\mupdf.sln"))) {
        throw "Could not find extracted MuPDF source under $Dest"
    }
}

# MuPDF's bundled JPEG 10 and qpdf's libjpeg-turbo have incompatible ABIs.
# Their shared jconfig.h enables upstream symbol renaming for every MuPDF JPEG
# caller and implementation. Updating the header also invalidates cached objects.
$jpegConfig = Join-Path $Dest "scripts\libjpeg\jconfig.h"
$jpegConfigText = Get-Content $jpegConfig -Raw
if ($jpegConfigText -notmatch '(?m)^#define FZ_HIDE_INTERNAL_JPEG(?:\s|$)') {
    Set-Content $jpegConfig -Value ("#define FZ_HIDE_INTERNAL_JPEG`r`n" + $jpegConfigText) -NoNewline
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
