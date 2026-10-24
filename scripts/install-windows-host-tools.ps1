<#
.SYNOPSIS
  Install verified PowerShell and optional .NET SDK archives for Windows CI.
.DESCRIPTION
  Bootstrap with the runner's PowerShell after checkout. Later pwsh steps use
  the pinned release. The optional SDK stays isolated from runner runtimes;
  WiX steps select it through MERVIN_DOTNET_ROOT. Windows PowerShell 5.1 remains
  available for the native Appx lifecycle test.
#>
#Requires -Version 7.0
[CmdletBinding()]
param([switch]$IncludeDotNet)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows -or -not $env:GITHUB_PATH -or -not $env:GITHUB_ENV) {
    throw 'Run this bootstrap in a Windows GitHub Actions job.'
}

# A new directory prevents preinstalled runtimes, SDKs, or previews from taking
# part in host selection. Never overlay the runner's shared .NET installation.
$suffix = [Guid]::NewGuid().ToString('N')
$tools = "C:\dev-tools\mervin-ci-hosts-$suffix"
$scratch = Join-Path (Split-Path $PSScriptRoot -Parent) "dev-temp\windows-host-tools-$suffix"
New-Item -ItemType Directory -Path $tools, $scratch | Out-Null

function Install-Archive {
    <# Download and verify an official archive before extracting any files. #>
    param(
        [string]$Uri,
        [string]$Algorithm,
        [string]$Hash,
        [string]$Destination
    )
    $archive = Join-Path $scratch ([IO.Path]::GetFileName($Uri))
    Invoke-WebRequest -Uri $Uri -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm $Algorithm).Hash -ne $Hash) {
        throw "Archive checksum mismatch for $Uri"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $Destination
}

try {
    $powershell = Join-Path $tools 'powershell-7.6.8'
    # https://github.com/PowerShell/PowerShell/releases/download/v7.6.8/hashes.sha256
    Install-Archive -Uri 'https://github.com/PowerShell/PowerShell/releases/download/v7.6.8/PowerShell-7.6.8-win-x64.zip' `
        -Algorithm SHA256 -Hash 'b9f074f923c499b51ec99d313da8e9ae90d17a9f3adf00c73d6189f34d908528' `
        -Destination $powershell
    $version = & (Join-Path $powershell 'pwsh.exe') -NoLogo -NoProfile -Command '$PSVersionTable.PSVersion.ToString()'
    if ($LASTEXITCODE -ne 0 -or $version -ne '7.6.8') {
        throw "Unexpected PowerShell version $version"
    }
    Write-Host "PowerShell $version installed in $powershell"

    if ($IncludeDotNet) {
        $dotnet = Join-Path $tools 'dotnet-10.0.402'
        # https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json
        Install-Archive -Uri 'https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.402/dotnet-sdk-10.0.402-win-x64.zip' `
            -Algorithm SHA512 -Hash 'ae4fd528cb7e9f6af6aaf7de36715a69f761f183744f1bb3025d19f5a92447090ec561410e46266c1d10a63781ed6f3de771760bc76c876d87daabc367b898cd' `
            -Destination $dotnet
        $version = & (Join-Path $dotnet 'dotnet.exe') --version
        if ($LASTEXITCODE -ne 0 -or $version -ne '10.0.402') {
            throw "Unexpected .NET SDK version $version"
        }
        $runtimes = @(& (Join-Path $dotnet 'dotnet.exe') --list-runtimes)
        if ($LASTEXITCODE -ne 0 -or
            @($runtimes | Where-Object { $_ -match '^Microsoft.NETCore.App 10\.0\.13 \[' }).Count -ne 1 -or
            @($runtimes | Where-Object { $_ -notmatch '^\S+ 10\.0\.13 \[' }).Count -ne 0) {
            throw "Unexpected .NET runtimes $runtimes"
        }
        Write-Host ".NET SDK $version installed in $dotnet"
        $runtimes | Write-Host
        "MERVIN_DOTNET_ROOT=$dotnet" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    }

    $powershell | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
