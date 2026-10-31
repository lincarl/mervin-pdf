<#
.SYNOPSIS
  Build MuPDF 1.28.6 static libraries on Windows (MSVC) via its VS solution,
  exactly as mervin-pdf's cmake/FindMuPDF.cmake expects.

.DESCRIPTION
  Downloads the official 1.28.6 source release (the tarball ships every
  thirdparty submodule pre-extracted, incl. tesseract/leptonica), applies the
  patches in scripts\mupdf-patches with git apply, builds
  platform\win32\mupdf.sln in Release|x64, and verifies libmupdf.lib landed in
  platform\win32\x64\Release. Writes the source root path to stdout (use it as
  MUPDF_DIR). Enables MuPDF's bundled JPEG symbol prefix so static qpdf can use
  its own JPEG ABI. Reuses only builds with the matching build-flavor marker.

  mervin-patch-set.sha256 beside libmupdf.lib records the patch set the
  libraries were built with. When it differs from the current patch set, the
  script deletes it, brings the source to the current set and rebuilds what
  changed. <Dest>\mervin-patches records the set the source has, with copies of
  its patches; without that folder the source is the unchanged release. An
  existing unpatched build gets the patches. A build with another patch set has
  that set reverted first. If the current set then does not apply, the script
  puts the previous set back and stops.

  Requires a developer environment with msbuild on PATH (e.g. after
  ilammy/msvc-dev-cmd or microsoft/setup-msbuild), and git. The Release config
  already includes Tesseract OCR and the codecs.

.PARAMETER Dest
  MuPDF source root to build in / extract to. Default C:\dev\src\mupdf-1.28.6-source
  (C:\dev is where this project keeps its development tools; the CI job caches the
  same path and so relies on this default).

.PARAMETER NoLtcg
  Build native object files for CI tests, avoiding repeated link-time optimization
  in every test executable. Use a separate Dest from release builds.

.PARAMETER PrintPatchSet
  Only write the patch set's identity to stdout, which the CI cache keys use.
#>
param(
    [string]$Dest = "C:\dev\src\mupdf-1.28.6-source",
    [switch]$NoLtcg,
    [switch]$PrintPatchSet
)

$ErrorActionPreference = "Stop"
$version = "1.28.6"
$sha256 = "dc85000640a45d90ca9b77b061f6ca67cce0c4d4fbecf8819647baf573eef101"
$url = "https://mupdf.com/downloads/archive/mupdf-$version-source.tar.gz"
$lib = Join-Path $Dest "platform\win32\x64\Release\libmupdf.lib"
$flavorFile = Join-Path (Split-Path $lib -Parent) "mervin-build-flavor.txt"
$flavor = if ($NoLtcg) { "native-jpeg-prefix-v1" } else { "ltcg-jpeg-prefix-v1" }
$patchDir = Join-Path $PSScriptRoot "mupdf-patches"
$patchMarker = Join-Path (Split-Path $lib -Parent) "mervin-patch-set.sha256"

# Returns the full paths of the .patch files in a directory, in ordinal name order.
function Get-MuPDFPatch([string]$Directory) {
    $names = [string[]]@(Get-ChildItem -LiteralPath $Directory -Filter "*.patch" -File -ErrorAction SilentlyContinue |
        ForEach-Object Name)
    [Array]::Sort($names, [StringComparer]::Ordinal)
    foreach ($name in $names) { Join-Path $Directory $name }
}

# Returns the patch set's identity, the SHA-256 of sha256sum's lines
# ("<hash>  <name>" and LF) for the patches in name order. build-mupdf-linux.sh,
# FindMuPDF.cmake and the CI cache keys compute the same value.
function Get-PatchSetId([object[]]$Patches) {
    $manifest = -join ($Patches | ForEach-Object {
        "{0}  {1}`n" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant(), (Split-Path $_ -Leaf)
    })
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $hasher.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($manifest))
    } finally {
        $hasher.Dispose()
    }
    -join ($digest | ForEach-Object { $_.ToString("x2") })
}

# Runs git apply in $Root with the patches joined into one input, which git apply
# leaves unapplied when any part fails (separate patch files would be applied one
# by one). With --reverse, git apply takes the patches in their forward order.
# Returns git's exit code. The bytes are copied unchanged, because piping text to
# a native command would rewrite its line endings.
function Invoke-GitApply([string]$Root, [object[]]$Patches, [string[]]$Options) {
    $joined = Join-Path ([System.IO.Path]::GetTempPath()) ("mervin-mupdf-" + [guid]::NewGuid().ToString("N") + ".patch")
    try {
        $stream = [System.IO.File]::Create($joined)
        try {
            foreach ($patch in $Patches) {
                $bytes = [System.IO.File]::ReadAllBytes($patch)
                $stream.Write($bytes, 0, $bytes.Length)
            }
        } finally {
            $stream.Dispose()
        }
        & git -C $Root apply @Options $joined | Out-Host
        $LASTEXITCODE
    } finally {
        Remove-Item -LiteralPath $joined -Force -ErrorAction SilentlyContinue
    }
}

# Writes $Root\mervin-patches for the set $Id with copies of $Patches. The folder
# appears in one rename, once its contents are complete.
function Write-PatchRecord([string]$Root, [string]$Id, [object[]]$Patches) {
    $newDir = Join-Path $Root "mervin-patches.new"
    New-Item -ItemType Directory -Path $newDir | Out-Null
    foreach ($patch in $Patches) { Copy-Item -LiteralPath $patch -Destination $newDir }
    Set-Content -LiteralPath (Join-Path $newDir "patch-set.sha256") -Value $Id
    Rename-Item -LiteralPath $newDir -NewName "mervin-patches"
}

# Brings the source tree at $Root to the current patch set unless it already has
# it. $Root\mervin-patches records the set the source has; without that folder
# the source is the unchanged release. The recorded set is reverted first, and
# put back when the current set does not apply.
function Sync-MuPDFPatch([string]$Root, [object[]]$Patches, [string]$PatchSet) {
    $recordDir = Join-Path $Root "mervin-patches"
    $previousDir = Join-Path $Root "mervin-patches.previous"
    $newDir = Join-Path $Root "mervin-patches.new"
    $recordFile = Join-Path $recordDir "patch-set.sha256"
    # mervin-patches only ever appears once the source matches it, so next to it a
    # mervin-patches.previous folder is a stale leftover. Without it, an update
    # stopped while the source was changing.
    if (Test-Path $previousDir) {
        if (-not (Test-Path $recordDir)) { throw "An earlier patch update of $Root did not finish. Delete it and rerun." }
        Remove-Item -LiteralPath $previousDir -Recurse -Force
    }
    if (Test-Path $newDir) { Remove-Item -LiteralPath $newDir -Recurse -Force }
    $recorded = if (Test-Path $recordFile) { ([string](Get-Content $recordFile -Raw)).Trim() } else { "" }
    if ($recorded -eq $PatchSet) { return }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git is needed to apply the MuPDF patches" }
    # Stop git's repository search at $Root, so a $Root inside a Git checkout
    # behaves the same as one outside it.
    $savedCeiling = $env:GIT_CEILING_DIRECTORIES
    $env:GIT_CEILING_DIRECTORIES = Split-Path $Root -Parent
    $copies = Join-Path ([System.IO.Path]::GetTempPath()) ("mervin-mupdf-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $copies | Out-Null
    try {
        $old = @(Get-MuPDFPatch $recordDir)
        if ($old.Count -gt 0) {
            Write-Host "Reverting the MuPDF patch set recorded in $recordDir"
            foreach ($patch in $old) { Copy-Item -LiteralPath $patch -Destination $copies }
            $old = @(Get-MuPDFPatch $copies)
            # If this run stops while the record is renamed, the next run refuses to go on.
            Rename-Item -LiteralPath $recordDir -NewName "mervin-patches.previous"
            if ((Invoke-GitApply $Root $old @("--reverse")) -ne 0) {
                Rename-Item -LiteralPath $previousDir -NewName "mervin-patches"
                throw "Cannot revert the recorded MuPDF patches. Delete $Root and rerun."
            }
        }
        # The source is the unchanged release now, and so no record is the true one.
        foreach ($dir in @($recordDir, $previousDir)) {
            if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        }
        if ($Patches.Count -gt 0) {
            Write-Host "Applying $($Patches.Count) MuPDF patches from $patchDir"
            if ((Invoke-GitApply $Root $Patches @()) -ne 0) {
                # Put the previous set back, so the source matches a record again.
                if ($old.Count -gt 0 -and (Invoke-GitApply $Root $old @()) -eq 0) {
                    Write-PatchRecord $Root $recorded $old
                    throw "The MuPDF patches in $patchDir do not apply. $Root keeps its previous patch set."
                }
                throw "The MuPDF patches do not apply to $Root, which must hold the unchanged MuPDF $version source. Delete it and rerun."
            }
        }
        Write-PatchRecord $Root $PatchSet $Patches
    } finally {
        $env:GIT_CEILING_DIRECTORIES = $savedCeiling
        Remove-Item -LiteralPath $copies -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$patches = @(Get-MuPDFPatch $patchDir)
$patchSet = Get-PatchSetId $patches
if ($PrintPatchSet) {
    Write-Output $patchSet
    exit 0
}

if (Test-Path $lib) {
    $builtFlavor = if (Test-Path $flavorFile) { (Get-Content $flavorFile -Raw).Trim() } else { "unmarked" }
    if ($builtFlavor -ne $flavor) {
        throw "MuPDF at $Dest has build flavor $builtFlavor. Run this script with a separate -Dest to build $flavor."
    }
    $builtPatchSet = if (Test-Path $patchMarker) { ([string](Get-Content $patchMarker -Raw)).Trim() } else { "none" }
    if ($builtPatchSet -eq $patchSet) {
        Write-Host "MuPDF already built at $Dest with patch set $patchSet"
        Write-Output $Dest
        exit 0
    }
    Write-Host "MuPDF at $Dest was built with patch set $builtPatchSet, not $patchSet. Rebuilding."
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

# Until msbuild succeeds, the libraries match no patch set.
if (Test-Path $patchMarker) { Remove-Item -LiteralPath $patchMarker -Force }
$root = (Resolve-Path -LiteralPath $Dest).ProviderPath
Sync-MuPDFPatch $root $patches $patchSet

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
Set-Content -Path $patchMarker -Value $patchSet

Write-Host "Built MuPDF static libs with patch set $patchSet in $(Split-Path $lib -Parent)"
Write-Output $Dest
