<#
.SYNOPSIS
  Verify a built MSI on a disposable Windows user account or hosted CI runner.
.DESCRIPTION
  Refuses existing Mervin installations and data. Exercises silent installation,
  installed application startup, upgrade, downgrade refusal, repair, and uninstall.
  The MSI must come from scripts/deploy.ps1 with its staging tree still present.
  Retains logs and the
  isolated application profile under C:\dev-temp. Failures retain their state.
.PARAMETER DisposableUser
  Confirm that the current Windows account is disposable when running outside CI.
#>
param(
    [Parameter(Mandatory)][string]$Msi,
    [switch]$DisposableUser
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw 'This test requires PowerShell 7 on Windows.' }
if (-not $DisposableUser -and
    -not ($env:GITHUB_ACTIONS -eq 'true' -and $env:RUNNER_ENVIRONMENT -eq 'github-hosted')) {
    throw 'Use a hosted GitHub runner or an empty disposable Windows account with -DisposableUser.'
}
$msiPath = (Resolve-Path -LiteralPath $Msi).Path
$dataDir = Join-Path $env:APPDATA 'MervinPDF'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Mervin PDF.lnk'
$installKey = 'HKCU:\Software\Mervin PDF'
$capabilitiesKey = 'HKCU:\Software\MervinPDF'
$progIdKey = 'HKCU:\Software\Classes\MervinPDF.Document'
$registeredApps = 'HKCU:\Software\RegisteredApplications'
$openWith = 'HKCU:\Software\Classes\.pdf\OpenWithProgids'
$userChoice = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice'
$uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall'

function Assert-Installer([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Read-RegistryValue([string]$Key, [string]$Name) {
    if (Test-Path -LiteralPath $Key) { (Get-Item -LiteralPath $Key).GetValue($Name, $null) }
}

# Per-user MSI registration can live in Installer\UserData rather than the
# conventional Uninstall key. Query Windows Installer's supported product API.
$msiEngine = New-Object -ComObject WindowsInstaller.Installer
function Find-MervinRegistration {
    foreach ($product in $msiEngine.ProductsEx('', '', 7)) {
        $name = $product.InstallProperty('ProductName')
        if ($name -like 'Mervin PDF*') {
            [pscustomobject]@{
                Name = $name
                ProductCode = $product.GetType().InvokeMember('ProductCode', 'GetProperty', $null, $product, $null)
                Context = $product.GetType().InvokeMember('Context', 'GetProperty', $null, $product, $null)
                State = $product.GetType().InvokeMember('State', 'GetProperty', $null, $product, $null)
                Version = $product.InstallProperty('VersionString')
            }
        }
    }
}

# Refuse any existing installation, state, or handler before creating test data.
foreach ($path in @($dataDir, $shortcut, $capabilitiesKey, $progIdKey,
                    (Join-Path $env:LOCALAPPDATA 'Mervin PDF'),
                    (Join-Path $env:LOCALAPPDATA 'MervinPDF'))) {
    Assert-Installer (-not (Test-Path -LiteralPath $path)) "Existing Mervin path prevents testing: $path"
}
# QSettings can create an empty key while unit tests inspect installation state.
# A recorded directory, rather than that empty key, identifies an installation.
Assert-Installer ($null -eq (Read-RegistryValue $installKey 'InstallDir')) 'Mervin already has an installation directory.'
foreach ($root in @('HKCU:', 'HKLM:', 'HKLM:\Software\WOW6432Node')) {
    $key = if ($root -like '*WOW6432Node') { 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' } else { "$root\$uninstallKey" }
    $existing = @(Get-ChildItem -LiteralPath $key -ErrorAction SilentlyContinue |
        Where-Object { $_.GetValue('DisplayName', '') -like 'Mervin PDF*' })
    Assert-Installer ($existing.Count -eq 0) "Existing Mervin uninstall entry prevents testing: $key"
}
Assert-Installer (@(Find-MervinRegistration).Count -eq 0) 'Mervin is already registered with Windows Installer.'
Assert-Installer ($null -eq (Read-RegistryValue $registeredApps 'MervinPDF')) 'Mervin is already registered.'
Assert-Installer ($null -eq (Read-RegistryValue $openWith 'MervinPDF.Document')) 'Mervin PDF handler already exists.'
Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Mervin is already running.'

$suffix = -join ([char[]]'abcdefghijklmnopqrstuvwxyz' | Get-Random -Count 4)
$work = "C:\dev-temp\mervin-msi-$suffix"
Assert-Installer (-not (Test-Path -LiteralPath $work)) 'Test directory already exists.'
New-Item -ItemType Directory -Path $work | Out-Null
if ($env:GITHUB_ENV) { "MERVIN_INSTALLER_TEST_DIR=$work" | Out-File $env:GITHUB_ENV -Append -Encoding utf8 }
Start-Transcript -Path (Join-Path $work 'test.log') | Out-Null
$installDir = Join-Path $work 'Installed application'
$exe = Join-Path $installDir 'MervinPDF.exe'

function Invoke-Msi([string]$Step, [string[]]$Options, [int[]]$ExpectedCodes = @(0, 3010)) {
    $log = Join-Path $work "$Step.log"
    $process = Start-Process "$env:SystemRoot\System32\msiexec.exe" -PassThru -ArgumentList (
        $Options + @('/qn', '/norestart', '/L*v', "`"$log`""))
    Write-Output "$Step process $($process.Id), log $log"
    if (-not $process.WaitForExit(300000)) {
        $process.Kill($true)
        throw "$Step timed out after five minutes. See $log"
    }
    Assert-Installer ($process.ExitCode -in $ExpectedCodes) "$Step returned unexpected exit code $($process.ExitCode). See $log"
}

try {
    # A verification-only newer product uses the same payload and never enters
    # the release artifact directory. Its app binary retains its real version.
    $build = Split-Path $msiPath -Parent
    $repository = Split-Path $PSScriptRoot -Parent
    $metadata = Get-Content (Join-Path $build 'generated\package-version.json') -Raw | ConvertFrom-Json
    $version = [Version]$metadata.msi_version
    Assert-Installer ($version.Build -lt 65535) 'The test needs room for a higher MSI version.'
    $upgradeVersion = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build + 1)
    $upgradeMsi = Join-Path $work 'MervinPDF-upgrade-test.msi'
    $bundledModel = Join-Path $repository 'resources\tessdata\eng.traineddata'
    & wix build -arch x64 -ext WixToolset.UI.wixext `
        -d "Version=$upgradeVersion" -d "DisplayVersion=$($metadata.version) upgrade verification" `
        -d "DeployDir=$build\deploy" -d "IconFile=$repository\resources\icons\mervin-icon.ico" `
        -d "DirectoryCleanup=$build\generated\installer-remove-folders.wxi" -d "TessData=$bundledModel" `
        -o $upgradeMsi "$repository\packaging\wix\mervin.wxs"
    Assert-Installer ($LASTEXITCODE -eq 0) 'Building the verification-only upgrade MSI failed.'

    $beforeDefault = Read-RegistryValue $userChoice 'ProgId'
    $otherHandler = "InstallerTest.OtherPdf.$suffix"
    foreach ($key in @($registeredApps, $openWith)) {
        if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force | Out-Null }
        New-ItemProperty -LiteralPath $key -Name $otherHandler -Value 'preserve-other-handler' -PropertyType String | Out-Null
    }
    $tessdata = Join-Path $dataDir 'tessdata'
    New-Item -ItemType Directory -Path $tessdata -Force | Out-Null
    $settings = Join-Path $dataDir 'config.toml'
    @'
ui_language = "sv"
color_scheme = "dark"
measurement_unit = "mm"
auto_update = false
restore_session = true
'@ | Set-Content -LiteralPath $settings -Encoding utf8
    $extraModel = Join-Path $tessdata 'eng_custom.traineddata'
    Copy-Item -LiteralPath $bundledModel -Destination $extraModel
    $settingsHash = (Get-FileHash -LiteralPath $settings).Hash
    $modelHash = (Get-FileHash -LiteralPath $extraModel).Hash

    Invoke-Msi 'install' @('/i', "`"$msiPath`"", "INSTALLFOLDER=`"$installDir`"")
    Start-Sleep -Seconds 1
    Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Silent installation launched Mervin.'
    Assert-Installer (Test-Path -LiteralPath $exe) 'Installed application is missing.'
    foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        Assert-Installer (Test-Path -LiteralPath (Join-Path $installDir $dll)) "App-local runtime is missing: $dll"
    }
    Assert-Installer (-not (Test-Path -LiteralPath (Join-Path $installDir 'vc_redist.x64.exe'))) 'Unused runtime installer was packaged.'
    Assert-Installer (Test-Path -LiteralPath $shortcut) 'Start menu shortcut is missing.'
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    Assert-Installer ($link.TargetPath -eq $exe) 'Start menu shortcut targets the wrong application.'
    Assert-Installer ((Read-RegistryValue $installKey 'InstallDir').TrimEnd('\') -eq $installDir) 'Custom installation folder was not recorded.'
    $registration = @(Find-MervinRegistration)
    Assert-Installer ($registration.Count -eq 1 -and $registration[0].Context -eq 2 -and $registration[0].State -eq 5) 'Per-user Installed apps entry is missing.'
    $registration | ConvertTo-Json | Set-Content (Join-Path $work 'installed-product.json')
    Assert-Installer ((Read-RegistryValue $registeredApps 'MervinPDF') -eq 'Software\MervinPDF\Capabilities') 'Default Apps registration is missing.'
    Assert-Installer ((Read-RegistryValue "$capabilitiesKey\Capabilities\FileAssociations" '.pdf') -eq 'MervinPDF.Document') 'PDF capabilities are missing.'
    Assert-Installer ((Read-RegistryValue "$progIdKey\shell\open\command" '') -eq "`"$exe`" `"%1`"") 'PDF open command does not quote the installed path.'
    Assert-Installer ((Read-RegistryValue $openWith 'MervinPDF.Document') -eq '') 'Open with registration is missing.'
    Assert-Installer ((Read-RegistryValue $userChoice 'ProgId') -eq $beforeDefault) 'Installation changed the default PDF handler.'
    $seededModel = Join-Path $tessdata 'eng.traineddata'
    Assert-Installer ((Get-FileHash -LiteralPath $seededModel).Hash -eq $modelHash) 'Bundled English OCR model was not seeded.'

    # Start the installed payload with its own state and a bounded normal exit.
    $launchEnvironment = @{}
    foreach ($name in @('PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')) {
        $launchEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_QPA_PLATFORM = 'windows'
        $env:QT_PLUGIN_PATH = ''
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
        $app = Start-Process $exe -PassThru `
            -RedirectStandardOutput (Join-Path $work 'app-stdout.log') `
            -RedirectStandardError (Join-Path $work 'app-stderr.log') `
            -ArgumentList @('--profile', "`"$work\profile`"", '--language', 'en', '--quit-after-startup')
    } finally {
        foreach ($name in $launchEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $launchEnvironment[$name], 'Process')
        }
    }
    Write-Output "Application process $($app.Id)"
    if (-not $app.WaitForExit(60000)) { $app.Kill($true); throw 'Installed application failed to exit after startup.' }
    Assert-Installer ($app.ExitCode -eq 0) "Installed application failed with exit code $($app.ExitCode)."

    # Preserve a valid user model, including its timestamp, across replacement.
    (Get-Item -LiteralPath $seededModel).LastWriteTimeUtc = [datetime]'2001-01-01T00:00:00Z'
    $modelTimestamp = (Get-Item -LiteralPath $seededModel).LastWriteTimeUtc
    Invoke-Msi 'upgrade' @('/i', "`"$upgradeMsi`"")
    Assert-Installer ((Read-RegistryValue $installKey 'InstallDir').TrimEnd('\') -eq $installDir) 'Upgrade lost the custom destination.'
    Assert-Installer (Test-Path -LiteralPath $exe) 'Upgrade did not install to the recorded destination.'
    Assert-Installer (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'Mervin PDF'))) 'Upgrade created a second default installation.'
    $registration = @(Find-MervinRegistration)
    Assert-Installer ($registration.Count -eq 1 -and $registration[0].Version -eq $upgradeVersion) 'Upgrade left an incorrect product registration.'
    Assert-Installer ((Get-Item -LiteralPath $seededModel).LastWriteTimeUtc -eq $modelTimestamp) 'Upgrade overwrote the existing OCR model.'
    Invoke-Msi 'downgrade' @('/i', "`"$msiPath`"") @(1603, 1638)
    Assert-Installer (Select-String -LiteralPath "$work\downgrade.log" -SimpleMatch -Quiet `
        'A newer version of Mervin PDF is already installed.') 'Downgrade failed for an unexpected reason.'

    Remove-Item -LiteralPath $exe, $shortcut
    Invoke-Msi 'repair' @('/famus', "`"$upgradeMsi`"")
    Assert-Installer (Test-Path -LiteralPath $exe) 'Repair did not restore the application.'
    Assert-Installer (Test-Path -LiteralPath $shortcut) 'Repair did not restore the shortcut.'
    Invoke-Msi 'uninstall' @('/x', "`"$upgradeMsi`"")
    Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Repair or uninstall launched Mervin.'
    foreach ($path in @($installDir, $shortcut, $capabilitiesKey, $progIdKey)) {
        Assert-Installer (-not (Test-Path -LiteralPath $path)) "Uninstall left an owned resource: $path"
    }
    Assert-Installer ($null -eq (Read-RegistryValue $installKey 'InstallDir')) 'Uninstall left its install directory registration.'
    Assert-Installer (@(Find-MervinRegistration).Count -eq 0) 'Uninstall left its Installed apps entry.'
    Assert-Installer ($null -eq (Read-RegistryValue $registeredApps 'MervinPDF')) 'Uninstall left Default Apps registration.'
    Assert-Installer ($null -eq (Read-RegistryValue $openWith 'MervinPDF.Document')) 'Uninstall left Open with registration.'
    foreach ($key in @($registeredApps, $openWith)) {
        Assert-Installer ((Read-RegistryValue $key $otherHandler) -eq 'preserve-other-handler') 'Uninstall removed another PDF handler.'
    }
    Assert-Installer ((Get-FileHash -LiteralPath $settings).Hash -eq $settingsHash) 'Settings changed or were removed.'
    foreach ($model in @($extraModel, $seededModel)) {
        Assert-Installer ((Get-FileHash -LiteralPath $model).Hash -eq $modelHash) 'An OCR model changed or was removed.'
    }
    Assert-Installer ((Get-Item -LiteralPath $seededModel).LastWriteTimeUtc -eq $modelTimestamp) 'Repair or uninstall overwrote the existing OCR model.'
    Assert-Installer ((Read-RegistryValue $userChoice 'ProgId') -eq $beforeDefault) 'Uninstall changed the default PDF handler.'

    # Remove only synthetic user data and neighboring values after success.
    foreach ($key in @($registeredApps, $openWith)) { Remove-ItemProperty -LiteralPath $key -Name $otherHandler }
    Remove-Item -LiteralPath $settings, $extraModel, $seededModel
    foreach ($dir in @($tessdata, $dataDir)) {
        if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { Remove-Item -LiteralPath $dir }
    }
    Write-Output "MSI functional verification passed. Logs and isolated profile remain in $work"
} finally {
    Stop-Transcript | Out-Null
}
