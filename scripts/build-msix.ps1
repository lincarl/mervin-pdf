<#
.SYNOPSIS
  Package the deployed Windows x64 application for Microsoft Store submission.

.DESCRIPTION
  Run deploy.ps1 first. This script copies its self-contained tree, adds the
  reserved Store identity and PDF association, and validates it with MakeAppx.
  Microsoft Store signs the submitted MSIX. No local signing certificate or
  Microsoft.VCLibs dependency is needed because deployment includes the runtime.

.PARAMETER Version
  Public application version embedded in MervinPDF.exe, such as 1.64.10.

.PARAMETER DeployDirectory
  Self-contained application tree produced by deploy.ps1.

.PARAMETER OutputPath
  Destination for the unsigned MSIX. A staging directory is created beside it.
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
if ($output.StartsWith($deploy.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $deploy.StartsWith($stage.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $stage.Equals($deploy, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The MSIX output and staging paths must be separate from the deployed application tree.'
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

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Get-ChildItem -LiteralPath $deploy -Force | Copy-Item -Destination $stage -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\windows\msix\Assets') -Destination $stage -Recurse
& python (Join-Path $PSScriptRoot 'prepare-msix-manifest.py') --version $Version `
    --output (Join-Path $stage 'AppxManifest.xml')
if ($LASTEXITCODE -ne 0) { throw 'MSIX manifest generation failed.' }

# Keep schema and content validation enabled. /nv would hide Store blockers.
& $makeAppx pack /d $stage /p $output /h SHA256 /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }
Write-Output "Unsigned Microsoft Store package: $output"
