<#
.SYNOPSIS
  Package the deployed Windows x64 application for Microsoft Store submission.

.DESCRIPTION
  Run deploy.ps1 first. This script copies its self-contained tree, adds the
  reserved Store identity and PDF association, indexes the icon variants with
  MakePri, and validates the package with MakeAppx. Microsoft Store signs the
  submitted MSIX. No local signing certificate or Microsoft.VCLibs dependency is
  needed because deployment includes the runtime.

.PARAMETER Version
  Public application version embedded in MervinPDF.exe, such as 1.64.10.

.PARAMETER DeployDirectory
  Self-contained application tree produced by deploy.ps1.

.PARAMETER OutputPath
  Destination for the unsigned MSIX. Staging and resource index directories are
  created beside it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$DeployDirectory,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\x64-release'
if (-not $DeployDirectory) { $DeployDirectory = Join-Path $build 'deploy' }
if (-not $OutputPath) { $OutputPath = Join-Path $build "MervinPDF-$Version-x64.msix" }
$deploy = (Resolve-Path -LiteralPath $DeployDirectory).Path
$output = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetExtension($output) -ne '.msix') { throw 'OutputPath must end in .msix.' }
$stage = Join-Path (Split-Path $output -Parent) ([IO.Path]::GetFileNameWithoutExtension($output) + '-msix-stage')
$pri = Join-Path (Split-Path $output -Parent) ([IO.Path]::GetFileNameWithoutExtension($output) + '-msix-pri')
if ($output.StartsWith($deploy.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    @($stage, $pri | Where-Object {
        $deploy.StartsWith($_.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $_.Equals($deploy, [StringComparison]::OrdinalIgnoreCase)
    }).Count -gt 0) {
    throw 'The MSIX output, staging and resource index paths must be separate from the deployed application tree.'
}
$exe = Join-Path $deploy 'MervinPDF.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run scripts/deploy.ps1 before building the MSIX.' }
if ((Get-Item -LiteralPath $exe).VersionInfo.FileVersion -ne $Version) {
    throw "MervinPDF.exe does not match requested version $Version. Reconfigure, rebuild and deploy it first."
}

# Recheck DLL closure so packaging a stale or manually assembled tree fails.
& python (Join-Path $PSScriptRoot 'check-windows-payload.py') --directory $deploy
if ($LASTEXITCODE -ne 0) { throw 'Windows payload dependency validation failed.' }

$makeAppx = (Get-Command MakeAppx.exe -ErrorAction SilentlyContinue).Source
if (-not $makeAppx) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $sdk = Get-ChildItem -LiteralPath $sdkRoot -Directory |
        Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
        Sort-Object { [version]$_.Name } -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64\MakeAppx.exe') } |
        Select-Object -First 1
    if ($sdk) { $makeAppx = Join-Path $sdk.FullName 'x64\MakeAppx.exe' }
}
if (-not $makeAppx) { throw 'MakeAppx.exe is missing. Install the Windows 10 or 11 SDK.' }
$makePri = Join-Path (Split-Path $makeAppx -Parent) 'makepri.exe'
if (-not (Test-Path -LiteralPath $makePri)) { throw 'MakePri.exe is missing beside MakeAppx.exe. Install the Windows 10 or 11 SDK.' }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Get-ChildItem -LiteralPath $deploy -Force | Copy-Item -Destination $stage -Recurse -Force
$assets = Join-Path $root 'packaging\windows\msix\Assets'
Copy-Item -LiteralPath $assets -Destination $stage -Recurse
$manifest = Join-Path $stage 'AppxManifest.xml'
& python (Join-Path $PSScriptRoot 'prepare-msix-manifest.py') --version $Version --output $manifest
if ($LASTEXITCODE -ne 0) { throw 'MSIX manifest generation failed.' }

# Windows finds the qualified icon files, such as
# Square44x44Logo.targetsize-24_altform-unplated.png, only through resources.pri.
# Without it the taskbar and Start menu shrink the icon onto a plate. Index only
# the images, from a separate root, so application folder and file names are
# never read as language or scale qualifiers.
if (Test-Path -LiteralPath $pri) { Remove-Item -LiteralPath $pri -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $pri 'root') -Force | Out-Null
Copy-Item -LiteralPath $assets -Destination (Join-Path $pri 'root') -Recurse
$priConfig = Join-Path $pri 'priconfig.xml'
& $makePri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o
if ($LASTEXITCODE -ne 0) { throw "MakePri createconfig failed ($LASTEXITCODE)." }
# A flat package uses one resources.pri, not split resource-package indexes.
[xml]$config = Get-Content -LiteralPath $priConfig -Raw
$packaging = $config.DocumentElement.SelectSingleNode("*[local-name()='packaging']")
if ($packaging) { [void]$config.DocumentElement.RemoveChild($packaging) }
$config.Save($priConfig)
& $makePri new /pr (Join-Path $pri 'root') /cf $priConfig /mn $manifest /of (Join-Path $stage 'resources.pri') /o
if ($LASTEXITCODE -ne 0) { throw "MakePri failed ($LASTEXITCODE)." }
if (@(Get-ChildItem -LiteralPath $stage -Filter '*.pri' -File).Count -ne 1) {
    throw 'MakePri must write exactly one resources.pri for the flat package.'
}
$dump = Join-Path $pri 'resources.pri.xml'
& $makePri dump /if (Join-Path $stage 'resources.pri') /of $dump
if ($LASTEXITCODE -ne 0) { throw "MakePri dump failed ($LASTEXITCODE)." }
[xml]$index = Get-Content -LiteralPath $dump -Raw
$candidates = @($index.SelectNodes("//*[local-name()='NamedResource' and @name='Square44x44Logo.png']/*[local-name()='Candidate']"))
$sizes = @(Get-ChildItem -LiteralPath $assets -Filter 'Square44x44Logo.targetsize-*_altform-unplated.png' -File).Count
foreach ($form in 'UNPLATED', 'LIGHTUNPLATED') {
    $found = @($candidates | Where-Object { $_.GetAttribute('qualifiers') -match "AlternateForm[-=]$form\b" }).Count
    if ($sizes -eq 0 -or $found -ne $sizes) {
        # Hosted runners discard the dump, so show what MakePri indexed in the log.
        Get-Content -LiteralPath $dump | Write-Host
        throw "resources.pri indexes $found of $sizes $form icons. The dump above is saved at $dump."
    }
}
Write-Output "resources.pri indexes $($candidates.Count) Square44x44Logo.png variants."

# Keep schema and content validation enabled. /nv would hide Store blockers.
& $makeAppx pack /d $stage /p $output /h SHA256 /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }
Write-Output "Unsigned Microsoft Store package: $output"
