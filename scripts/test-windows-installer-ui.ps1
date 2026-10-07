<#
.SYNOPSIS
  Exercise the native MSI wizard and its launch option on a disposable GitHub runner.
.DESCRIPTION
  Uses native controls rather than screen coordinates. Captures every wizard page
  and the application window, then removes the installation created by this test.
  The runner account must have no existing Mervin installation or application data.
#>
param(
    [Parameter(Mandatory)][string]$Msi,
    [Parameter(Mandatory)][string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_OS -ne 'Windows' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This test requires a disposable GitHub-hosted Windows runner.'
}
$Msi = (Resolve-Path $Msi).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (-not $OutputDir.StartsWith('C:\dev-temp\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDir must be inside C:\dev-temp.'
}
$installDir = Join-Path $OutputDir 'installed app'
$appDataDir = Join-Path $env:APPDATA 'MervinPDF'
$appExe = Join-Path $installDir 'MervinPDF.exe'
$registeredInstall = Get-ItemPropertyValue 'HKCU:\Software\Mervin PDF' -Name InstallDir -ErrorAction SilentlyContinue
if ((Test-Path $installDir) -or (Test-Path $appDataDir) -or
    $registeredInstall -or (Get-Process MervinPDF -ErrorAction SilentlyContinue)) {
    throw 'The UI test requires an unused install directory and no existing Mervin state.'
}
New-Item -ItemType Directory -Force $OutputDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class MervinInstallerUi
{
    public sealed class WindowInfo
    {
        public IntPtr Handle;
        public string Text;
        public string ClassName;
        public bool Enabled;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumCallback(IntPtr hwnd, IntPtr argument);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr argument);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumCallback callback, IntPtr argument);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder text, int capacity);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendTextTimeout(IntPtr hwnd, uint message, IntPtr wparam, string text, uint flags, uint timeout, out IntPtr result);

    private static WindowInfo Describe(IntPtr hwnd)
    {
        var text = new StringBuilder(4096);
        var name = new StringBuilder(256);
        GetWindowText(hwnd, text, text.Capacity);
        GetClassName(hwnd, name, name.Capacity);
        return new WindowInfo { Handle = hwnd, Text = text.ToString(), ClassName = name.ToString(), Enabled = IsWindowEnabled(hwnd) };
    }
    public static WindowInfo[] Windows(int processId)
    {
        var result = new List<WindowInfo>();
        EnumWindows((hwnd, argument) => {
            uint owner;
            GetWindowThreadProcessId(hwnd, out owner);
            if (owner == processId && IsWindowVisible(hwnd)) result.Add(Describe(hwnd));
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static WindowInfo[] Children(IntPtr parent)
    {
        var result = new List<WindowInfo>();
        EnumChildWindows(parent, (hwnd, argument) => {
            if (IsWindowVisible(hwnd)) result.Add(Describe(hwnd));
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static int CheckState(IntPtr hwnd)
    {
        IntPtr result;
        if (SendMessageTimeout(hwnd, 0x00F0, IntPtr.Zero, IntPtr.Zero, 2, 5000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("The checkbox did not respond.");
        return result.ToInt32();
    }
    public static void SetText(IntPtr hwnd, string text)
    {
        IntPtr result;
        if (SendTextTimeout(hwnd, 0x000C, IntPtr.Zero, text, 2, 5000, out result) == IntPtr.Zero || result == IntPtr.Zero)
            throw new InvalidOperationException("The folder field did not accept its value.");
    }
}
'@

$setup = $null
$launchedApp = $null
$currentDialog = $null
$installAttempted = $false
$progressSeen = $false
$launchEnvironment = @{}
foreach ($name in @('PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')) {
    $launchEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

# Save the actual window and its native control text for visual and failure review.
function Save-WindowEvidence($Window, [string]$Name) {
    [MervinInstallerUi]::SetForegroundWindow($Window.Handle) | Out-Null
    $rect = New-Object MervinInstallerUi+Rect
    if (-not [MervinInstallerUi]::GetWindowRect($Window.Handle, [ref]$rect)) {
        throw "Cannot get the bounds of $Name."
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 50 -or $height -lt 50) { throw "Invalid window dimensions for $Name." }
    $bitmap = New-Object Drawing.Bitmap $width, $height
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try { $captured = [MervinInstallerUi]::PrintWindow($Window.Handle, $dc, 2) }
        finally { $graphics.ReleaseHdc($dc) }
        if (-not $captured) { throw "PrintWindow failed for $Name." }
        $bitmap.Save((Join-Path $OutputDir "$Name.png"), [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    [MervinInstallerUi]::Children($Window.Handle) |
        Select-Object Text, ClassName, Enabled |
        ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutputDir "$Name.controls.json")
    Write-Host "Captured $Name."
}

function Get-InstallerState {
    foreach ($window in [MervinInstallerUi]::Windows($setup.Id)) {
        $controls = @([MervinInstallerUi]::Children($window.Handle))
        $text = ($controls | ForEach-Object { $_.Text }) -join "`n"
        [pscustomobject]@{ Window = $window; Controls = $controls; Text = $text }
    }
}

function Wait-InstallerPage([string]$Name, [string]$TextPattern, [int]$Seconds = 45) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        foreach ($state in @(Get-InstallerState)) {
            $script:currentDialog = $state.Window
            if ($state.Text -match $TextPattern) { return $state }
            if ($state.Text -match 'ended prematurely|installation failed|Could not|Error [0-9]') {
                throw "The installer reported an error while waiting for $Name. $($state.Text)"
            }
        }
        if ($setup.HasExited) { throw "The installer exited with $($setup.ExitCode) while waiting for $Name." }
        Start-Sleep -Milliseconds 150
    }
    throw "Timed out waiting for $Name. UserInteractive=$([Environment]::UserInteractive), session=$((Get-Process -Id $PID).SessionId)."
}

function Click-Control($State, [string]$TextPattern) {
    $buttons = @($State.Controls | Where-Object {
        $_.ClassName -eq 'Button' -and $_.Enabled -and ($_.Text -replace '&', '') -match $TextPattern
    })
    if ($buttons.Count -ne 1) { throw "Expected one enabled button matching $TextPattern, found $($buttons.Count)." }
    if (-not [MervinInstallerUi]::PostMessage($buttons[0].Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw "Could not click $TextPattern."
    }
}

try {
    # The test job uses Qt's offscreen plugin and a compiler PATH for unit tests.
    # Exercise the installed Windows GUI using only its deployed dependencies.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'windows'
    Remove-Item Env:QT_PLUGIN_PATH, Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
    # Finish launches the installed app exactly as it does for a user. Seed only
    # this disposable account's fresh profile so first-run prompts do not hide it.
    New-Item -ItemType Directory $appDataDir | Out-Null
    @'
ui_language = 'en'
auto_update = false
prompted_set_default_app = true
close_to_tray = false
'@ | Set-Content (Join-Path $appDataDir 'config.toml')

    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $log = Join-Path $OutputDir 'interactive-install.log'
    $setup = Start-Process $msiexec -ArgumentList "/i `"$Msi`" /norestart /l*v `"$log`"" -PassThru
    $installAttempted = $true
    Write-Host "Installer process $($setup.Id), session $($setup.SessionId)."

    $state = Wait-InstallerPage 'Welcome' 'Welcome to'
    Save-WindowEvidence $state.Window '01-welcome'
    Click-Control $state '^Next\s*>?$'

    $state = Wait-InstallerPage 'Destination folder' 'Destination Folder'
    $edits = @($state.Controls | Where-Object { $_.ClassName -eq 'Edit' -and $_.Enabled })
    if ($edits.Count -ne 1) { throw "Expected one destination field, found $($edits.Count)." }
    [MervinInstallerUi]::SetText($edits[0].Handle, "$installDir\")
    Save-WindowEvidence $state.Window '02-destination'
    Click-Control $state '^Next\s*>?$'

    $state = Wait-InstallerPage 'Ready to install' 'Ready to install'
    Save-WindowEvidence $state.Window '03-ready'
    Click-Control $state '^Install$'

    $watch = [Diagnostics.Stopwatch]::StartNew()
    $finish = $null
    while ($watch.Elapsed.TotalSeconds -lt 180 -and -not $finish) {
        foreach ($state in @(Get-InstallerState)) {
            $currentDialog = $state.Window
            if ($state.Text -match 'Launch Mervin PDF') { $finish = $state; break }
            if (-not $progressSeen -and $state.Text -match 'Installing Mervin|Please wait while') {
                Save-WindowEvidence $state.Window '04-progress'
                $progressSeen = $true
            }
            if ($state.Text -match 'ended prematurely|installation failed|Could not|Error [0-9]') {
                throw "Installation failed. $($state.Text)"
            }
        }
        if ($setup.HasExited) { throw "Installer exited before Finish with $($setup.ExitCode)." }
        Start-Sleep -Milliseconds 100
    }
    if (-not $finish) { throw 'Timed out waiting for the successful Finish page.' }
    $checkbox = @($finish.Controls | Where-Object { ($_.Text -replace '&', '') -eq 'Launch Mervin PDF' })
    if ($checkbox.Count -ne 1 -or [MervinInstallerUi]::CheckState($checkbox[0].Handle) -ne 1) {
        throw 'Launch Mervin PDF must be checked by default.'
    }
    if (-not (Test-Path $appExe)) { throw 'The wizard did not install into the chosen destination.' }
    if (Get-Process MervinPDF -ErrorAction SilentlyContinue) { throw 'Mervin launched before Finish was clicked.' }
    Save-WindowEvidence $finish.Window '05-finish-launch-checked'
    Click-Control $finish '^Finish$'
    if (-not $setup.WaitForExit(30000)) { throw 'The installer did not exit after Finish.' }
    if ($setup.ExitCode -ne 0) { throw "Interactive installation returned $($setup.ExitCode)." }

    $watch.Restart()
    $appWindow = $null
    while ($watch.Elapsed.TotalSeconds -lt 45 -and -not $appWindow) {
        $launchedApp = Get-Process MervinPDF -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $appExe } | Select-Object -First 1
        if ($launchedApp) {
            $appWindow = [MervinInstallerUi]::Windows($launchedApp.Id) |
                Where-Object { $_.Text -match 'Mervin PDF' } | Select-Object -First 1
        }
        if (-not $appWindow) { Start-Sleep -Milliseconds 200 }
    }
    if (-not $appWindow) { throw 'Finish did not open a window from the installed application.' }
    $launchedApp.Refresh()
    $runtimeModules = foreach ($name in @('msvcp140.dll', 'vcruntime140.dll')) {
        $module = @($launchedApp.Modules | Where-Object { $_.ModuleName -eq $name })
        if ($module.Count -ne 1 -or $module[0].FileName -ne (Join-Path $installDir $name)) {
            throw "The application must load $name from its installation directory."
        }
        [pscustomobject]@{ name = $name; path = $module[0].FileName }
    }
    Start-Sleep -Milliseconds 500
    Save-WindowEvidence $appWindow '06-launched-application'
    [pscustomobject]@{
        installDirectory = $installDir
        installerExitCode = $setup.ExitCode
        launchChecked = $true
        launchedExecutable = $launchedApp.Path
        launchedProcessId = $launchedApp.Id
        runtimeModules = @($runtimeModules)
        progressObserved = $progressSeen
        windowTitle = $appWindow.Text
    } | ConvertTo-Json | Set-Content (Join-Path $OutputDir 'result.json')
    Write-Host 'Native wizard navigation, destination selection, checked launch option and application launch passed.'
} catch {
    if ($currentDialog) {
        try { Save-WindowEvidence $currentDialog 'failure' }
        catch { Write-Warning "Could not capture the failed dialog. $_" }
    }
    throw
} finally {
    foreach ($name in $launchEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $launchEnvironment[$name], 'Process')
    }
    # Only processes started by this test, identified by PID or the unique
    # installed executable path, can be stopped during teardown.
    foreach ($process in @(Get-Process MervinPDF -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $appExe })) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    if ($setup -and -not $setup.HasExited) { Stop-Process -Id $setup.Id -Force }
    if ($installAttempted) {
        $log = Join-Path $OutputDir 'ui-test-uninstall.log'
        $uninstall = Start-Process $msiexec -ArgumentList "/x `"$Msi`" /qn /norestart /l*v `"$log`"" -PassThru
        Write-Host "Uninstaller process $($uninstall.Id)."
        if (-not $uninstall.WaitForExit(120000)) {
            Stop-Process -Id $uninstall.Id -Force
            throw 'UI test cleanup timed out.'
        }
        if ($uninstall.ExitCode -notin @(0, 1605)) { throw "UI test uninstall returned $($uninstall.ExitCode)." }
        if (Test-Path $appExe) { throw 'UI test uninstall left the application installed.' }
    }
}
