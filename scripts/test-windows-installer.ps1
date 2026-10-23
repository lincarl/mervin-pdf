<#
.SYNOPSIS
  Verify a built MSI on a disposable Windows user account or hosted CI runner.
.DESCRIPTION
  Refuses existing Mervin installations and data. Exercises passive installation,
  accepted and rejected documents, silent upgrades, downgrade refusal, repair,
  and uninstall while preserving user files.
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
. (Join-Path $PSScriptRoot 'windows-document-test-helpers.ps1')
if (-not $IsWindows) { throw 'This test requires PowerShell 7 on Windows.' }
if (-not $DisposableUser -and
    -not ($env:GITHUB_ACTIONS -eq 'true' -and $env:RUNNER_ENVIRONMENT -eq 'github-hosted')) {
    throw 'Use a hosted GitHub runner or an empty disposable Windows account with -DisposableUser.'
}
$msiPath = (Resolve-Path -LiteralPath $Msi).Path
$build = Split-Path $msiPath -Parent
$fixture = Join-Path $build 'tests\fixtures\properties.pdf'
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

Assert-Installer (Test-Path -LiteralPath $fixture) 'Generated PDF fixture is missing. Configure with MERVIN_BUILD_TESTS=ON.'

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

function Invoke-Msi([string]$Step, [string[]]$Options, [int[]]$ExpectedCodes = @(0, 3010),
                    [ValidateSet('/qn', '/passive')][string]$UiMode = '/qn') {
    $log = Join-Path $work "$Step.log"
    $process = Start-Process "$env:SystemRoot\System32\msiexec.exe" -PassThru -ArgumentList (
        $Options + @($UiMode, '/norestart', '/L*v', "`"$log`""))
    Write-Output "$Step process $($process.Id), log $log"
    try {
        if (-not $process.WaitForExit(300000)) {
            $process.Kill($true)
            $process.WaitForExit(10000) | Out-Null
            throw "$Step timed out after five minutes. See $log"
        }
        Assert-Installer ($process.ExitCode -in $ExpectedCodes) "$Step returned unexpected exit code $($process.ExitCode). See $log"
        Assert-Installer (-not (Select-String -LiteralPath $log -Quiet -Pattern 'Doing action: LaunchMervin\b|Action start [^\r\n]*: LaunchMervin\.')) "$Step executed the full-UI launch action. See $log"
    } finally { $process.Dispose() }
}

function Test-InstalledDocument([string]$Document, [string]$Case, [bool]$ExpectedLoaded) {
    $profile = Join-Path $work "profile-$Case"
    New-DocumentTestProfile $profile
    $launchEnvironment = @{}
    foreach ($name in @('PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')) {
        $launchEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $app = $null
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_QPA_PLATFORM = 'windows'
        $env:QT_PLUGIN_PATH = ''
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
        $app = Start-Process $exe -PassThru -WorkingDirectory $work `
            -RedirectStandardOutput (Join-Path $work "$Case-stdout.log") `
            -RedirectStandardError (Join-Path $work "$Case-stderr.log") `
            -ArgumentList @('--profile', "`"$profile`"", '--language', 'en', '--quit-after-startup', "`"$Document`"")
        Write-Output "$Case application process $($app.Id)"
        Assert-Installer ($app.WaitForExit(60000)) "$Case application did not finish its load attempt."
        Assert-Installer ($app.ExitCode -eq 0) "$Case application failed with exit code $($app.ExitCode)."
        Assert-DocumentLoad $profile $Document $ExpectedLoaded
    } finally {
        foreach ($name in $launchEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $launchEnvironment[$name], 'Process')
        }
        if ($null -ne $app) {
            if (-not $app.HasExited) { $app.Kill($true); $app.WaitForExit(10000) | Out-Null }
            $app.Dispose()
        }
    }
}

try {
    # Synthetic versions retain the real application binary. The intermediate
    # payload owns an extra file so the next upgrade must remove obsolete files.
    $repository = Split-Path $PSScriptRoot -Parent
    $metadata = Get-Content (Join-Path $build 'generated\package-version.json') -Raw | ConvertFrom-Json
    $version = [Version]$metadata.msi_version
    Assert-Installer ($version.Build -lt 65534) 'The test needs room for two higher MSI versions.'
    $upgradeVersion = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build + 1)
    $explicitVersion = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build + 2)
    $upgradeMsi = Join-Path $work 'MervinPDF-upgrade-test.msi'
    $explicitMsi = Join-Path $work 'MervinPDF-explicit-upgrade-test.msi'
    $oldPayload = Join-Path $work 'intermediate-payload'
    Copy-Item -LiteralPath (Join-Path $build 'deploy') -Destination $oldPayload -Recurse
    $obsoleteName = 'installer-test-obsolete.txt'
    'Owned only by the intermediate MSI.' | Set-Content -LiteralPath (Join-Path $oldPayload $obsoleteName)
    foreach ($package in @(
        @{ Version = $upgradeVersion; Path = $upgradeMsi; Payload = $oldPayload },
        @{ Version = $explicitVersion; Path = $explicitMsi; Payload = (Join-Path $build 'deploy') }
    )) {
        & wix build -arch x64 -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext `
            -d "Version=$($package.Version)" -d "DisplayVersion=$($metadata.version) upgrade verification" `
            -d "DeployDir=$($package.Payload)" -d "IconFile=$repository\resources\icons\mervin-icon.ico" `
            -d "DirectoryCleanup=$build\generated\installer-remove-folders.wxi" `
            -o $package.Path "$repository\packaging\wix\mervin.wxs"
        Assert-Installer ($LASTEXITCODE -eq 0) 'Building the verification-only upgrade MSI failed.'
    }

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
    # Opaque user bytes are enough to test preservation; this account never runs OCR.
    [IO.File]::WriteAllBytes($extraModel, [byte[]](1, 3, 5, 7, 9))
    $settingsHash = (Get-FileHash -LiteralPath $settings).Hash
    $modelHash = (Get-FileHash -LiteralPath $extraModel).Hash

    Invoke-Msi 'install' @('/i', "`"$msiPath`"", "INSTALLFOLDER=`"$installDir`"") -UiMode '/passive'
    Start-Sleep -Seconds 1
    Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Passive installation launched Mervin.'
    Assert-Installer (Test-Path -LiteralPath $exe) 'Installed application is missing.'
    & python (Join-Path $PSScriptRoot 'check-windows-payload.py') --directory $installDir `
        --report (Join-Path $work 'installed-payload.json')
    Assert-Installer ($LASTEXITCODE -eq 0) 'Installed payload dependency validation failed.'
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
    $englishModel = Join-Path $tessdata 'eng.traineddata'
    Assert-Installer (-not (Test-Path -LiteralPath $englishModel)) 'Installer unexpectedly supplied an English OCR model.'
    Assert-Installer (@(Get-ChildItem -LiteralPath $installDir -Filter '*.traineddata' -Recurse).Count -eq 0) 'Application payload contains an OCR model.'

    # Startup completion includes failed loads. Check fresh history after each
    # bounded run instead of treating --quit-after-startup as a success signal.
    $testPdf = Join-Path $work 'generated-test.pdf'
    Copy-Item -LiteralPath $fixture -Destination $testPdf
    $corruptPdf = Join-Path $work 'corrupt-test.pdf'
    [IO.File]::WriteAllText($corruptPdf, "%PDF-1.7`nnot a valid document`n")
    Test-InstalledDocument $testPdf 'loaded-document' $true
    Test-InstalledDocument $corruptPdf 'corrupt-document' $false

    $retainedFiles = @{}
    foreach ($relative in @('user-notes.txt', 'platforms\user-notes.txt')) {
        $path = Join-Path $installDir $relative
        Assert-Installer (-not (Test-Path -LiteralPath $path)) "User preservation fixture collides with payload: $path"
        'Keep user bytes inside an installation directory.' | Set-Content -LiteralPath $path
        $retainedFiles[$path] = (Get-FileHash -LiteralPath $path).Hash
    }

    # Simulate an existing English model and preserve its bytes and timestamp.
    Copy-Item -LiteralPath $extraModel -Destination $englishModel
    (Get-Item -LiteralPath $englishModel).LastWriteTimeUtc = [datetime]'2001-01-01T00:00:00Z'
    $modelTimestamp = (Get-Item -LiteralPath $englishModel).LastWriteTimeUtc
    Invoke-Msi 'upgrade' @('/i', "`"$upgradeMsi`"")
    Assert-Installer ((Read-RegistryValue $installKey 'InstallDir').TrimEnd('\') -eq $installDir) 'Upgrade lost the custom destination.'
    Assert-Installer (Test-Path -LiteralPath $exe) 'Upgrade did not install to the recorded destination.'
    Assert-Installer (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'Mervin PDF'))) 'Upgrade created a second default installation.'
    $registration = @(Find-MervinRegistration)
    Assert-Installer ($registration.Count -eq 1 -and $registration[0].Version -eq $upgradeVersion) 'Upgrade left an incorrect product registration.'
    Assert-Installer ((Get-Item -LiteralPath $englishModel).LastWriteTimeUtc -eq $modelTimestamp) 'Upgrade overwrote the existing OCR model.'
    Assert-Installer (Test-Path -LiteralPath (Join-Path $installDir $obsoleteName)) 'Intermediate package did not install its distinct payload.'
    foreach ($path in $retainedFiles.Keys) {
        Assert-Installer ((Get-FileHash -LiteralPath $path).Hash -eq $retainedFiles[$path]) 'Upgrade changed a user file inside the installation.'
    }
    $previousDir = $installDir
    $installDir = Join-Path $work 'Explicit upgrade destination'
    $exe = Join-Path $installDir 'MervinPDF.exe'
    Invoke-Msi 'explicit-upgrade' @('/i', "`"$explicitMsi`"", "INSTALLFOLDER=`"$installDir`"")
    Assert-Installer ((Read-RegistryValue $installKey 'InstallDir').TrimEnd('\') -eq $installDir) 'Explicit upgrade destination did not override the previous folder.'
    Assert-Installer (Test-Path -LiteralPath $exe) 'Explicit upgrade did not install the application.'
    Assert-Installer (@(Get-ChildItem -LiteralPath $previousDir -File -Recurse).Count -eq $retainedFiles.Count) 'Explicit upgrade left old payload files beside retained user files.'
    Assert-Installer (-not (Test-Path -LiteralPath (Join-Path $installDir $obsoleteName))) 'Explicit upgrade retained an obsolete payload file.'
    $registration = @(Find-MervinRegistration)
    Assert-Installer ($registration.Count -eq 1 -and $registration[0].Version -eq $explicitVersion) 'Explicit upgrade left an incorrect product registration.'
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    Assert-Installer ($link.TargetPath -eq $exe) 'Explicit upgrade did not retarget the Start menu shortcut.'
    Assert-Installer ((Read-RegistryValue "$progIdKey\shell\open\command" '') -eq "`"$exe`" `"%1`"") 'Explicit upgrade did not retarget PDF activation.'
    Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Silent upgrade launched Mervin.'
    $newUserFile = Join-Path $installDir 'platforms\user-notes.txt'
    'Keep this user file after uninstall.' | Set-Content -LiteralPath $newUserFile
    $retainedFiles[$newUserFile] = (Get-FileHash -LiteralPath $newUserFile).Hash
    Invoke-Msi 'downgrade' @('/i', "`"$msiPath`"") @(1603, 1638)
    Assert-Installer (Select-String -LiteralPath "$work\downgrade.log" -SimpleMatch -Quiet `
        'A newer version of Mervin PDF is already installed.') 'Downgrade failed for an unexpected reason.'

    Remove-Item -LiteralPath $exe, $shortcut
    Invoke-Msi 'repair' @('/famus', "`"$explicitMsi`"")
    Assert-Installer (Test-Path -LiteralPath $exe) 'Repair did not restore the application.'
    Assert-Installer (Test-Path -LiteralPath $shortcut) 'Repair did not restore the shortcut.'
    Invoke-Msi 'uninstall' @('/x', "`"$explicitMsi`"")
    Assert-Installer (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Repair or uninstall launched Mervin.'
    foreach ($path in @($exe, $shortcut, $capabilitiesKey, $progIdKey)) {
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
    foreach ($model in @($extraModel, $englishModel)) {
        Assert-Installer ((Get-FileHash -LiteralPath $model).Hash -eq $modelHash) 'An OCR model changed or was removed.'
    }
    Assert-Installer ((Get-Item -LiteralPath $englishModel).LastWriteTimeUtc -eq $modelTimestamp) 'Repair or uninstall overwrote the existing OCR model.'
    Assert-Installer ((Read-RegistryValue $userChoice 'ProgId') -eq $beforeDefault) 'Uninstall changed the default PDF handler.'
    foreach ($path in $retainedFiles.Keys) {
        Assert-Installer ((Get-FileHash -LiteralPath $path).Hash -eq $retainedFiles[$path]) "Upgrade, repair or uninstall changed a user file: $path"
    }
    $remaining = @(Get-ChildItem -LiteralPath $previousDir, $installDir -File -Recurse)
    Assert-Installer ($remaining.Count -eq $retainedFiles.Count) 'Uninstall left application files in a directory containing user data.'
    $remainingDirectories = @(Get-ChildItem -LiteralPath $previousDir, $installDir -Directory -Recurse)
    Assert-Installer ($remainingDirectories.Count -eq 2 -and @($remainingDirectories | Where-Object { $_.Name -ne 'platforms' }).Count -eq 0) 'Uninstall left empty payload directories.'

    # Remove only synthetic user data and neighboring values after success.
    foreach ($key in @($registeredApps, $openWith)) { Remove-ItemProperty -LiteralPath $key -Name $otherHandler }
    Remove-Item -LiteralPath $settings, $extraModel, $englishModel
    foreach ($dir in @($tessdata, $dataDir)) {
        if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { Remove-Item -LiteralPath $dir }
    }
    Write-Output "MSI functional verification passed. Logs and isolated profile remain in $work"
} finally {
    Stop-Transcript | Out-Null
}
