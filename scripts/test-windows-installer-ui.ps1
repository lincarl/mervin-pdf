<#
.SYNOPSIS
  Exercise the native MSI wizard and its launch option on a disposable GitHub runner.
.DESCRIPTION
  Uses native controls rather than screen coordinates. Exercises Back, Cancel,
  maintenance Repair and both Finish launch choices, then removes its installation.
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
$msiEngine = New-Object -ComObject WindowsInstaller.Installer
function Get-MervinProducts {
    foreach ($product in $msiEngine.ProductsEx('', '', 7)) {
        if ($product.InstallProperty('ProductName') -like 'Mervin PDF*') { $product }
    }
}
$registeredInstall = Get-ItemPropertyValue 'HKCU:\Software\Mervin PDF' -Name InstallDir -ErrorAction SilentlyContinue
if ((Test-Path $installDir) -or (Test-Path $appDataDir) -or
    $registeredInstall -or @(Get-MervinProducts).Count -ne 0 -or (Get-Process MervinPDF -ErrorAction SilentlyContinue)) {
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
    [StructLayout(LayoutKind.Sequential)]
    private struct GuiThreadInfo
    {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretRect;
    }
    private delegate bool EnumCallback(IntPtr hwnd, IntPtr argument);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr argument);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumCallback callback, IntPtr argument);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetGUIThreadInfo(uint threadId, ref GuiThreadInfo info);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", SetLastError = true)] private static extern bool AttachThreadInput(uint source, uint target, bool attach);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SetFocus(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
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
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr ReadTextTimeout(IntPtr hwnd, uint message, IntPtr wparam, StringBuilder text, uint flags, uint timeout, out IntPtr result);

    private static WindowInfo Describe(IntPtr hwnd)
    {
        var text = new StringBuilder(4096);
        var name = new StringBuilder(256);
        GetWindowText(hwnd, text, text.Capacity);
        GetClassName(hwnd, name, name.Capacity);
        if (name.ToString() == "Edit" || name.ToString() == "RichEdit20W") {
            // GetWindowText cannot retrieve another process's edit contents.
            IntPtr result;
            ReadTextTimeout(hwnd, 0x000D, new IntPtr(text.Capacity), text, 2, 5000, out result);
        }
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
        // Replace the selection as an edit so MSI receives change notifications
        // and the modification flag is set. WM_SETTEXT clears that flag.
        if (SendMessageTimeout(hwnd, 0x00B1, IntPtr.Zero, new IntPtr(-1), 2, 5000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("The folder field did not select its contents.");
        if (SendTextTimeout(hwnd, 0x00C2, new IntPtr(1), text, 2, 5000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("The folder field did not accept its value.");
        if (SendMessageTimeout(hwnd, 0x00B8, IntPtr.Zero, IntPtr.Zero, 2, 5000, out result) == IntPtr.Zero || result == IntPtr.Zero)
            throw new InvalidOperationException("The folder field did not register its contents as modified.");
    }
    private static string DescribeFocus(uint thread, IntPtr dialog, IntPtr control)
    {
        var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
        bool available = GetGUIThreadInfo(thread, ref info);
        int error = Marshal.GetLastWin32Error();
        return String.Format("thread={0}, dialog=0x{1:X}, requested=0x{2:X}, active=0x{3:X}, focus=0x{4:X}, foreground=0x{5:X}, infoAvailable={6}, infoError={7}",
            thread, dialog.ToInt64(), control.ToInt64(), info.Active.ToInt64(), info.Focus.ToInt64(), GetForegroundWindow().ToInt64(), available, error);
    }
    public static void FocusControl(IntPtr dialog, IntPtr control)
    {
        // Attach input queues so SetFocus sends real focus gain/loss events to
        // the other process. MSI does not handle WM_NEXTDLGCTL for PathEdit.
        SetForegroundWindow(dialog);
        uint owner;
        uint thread = GetWindowThreadProcessId(control, out owner);
        uint caller = GetCurrentThreadId();
        bool attach = caller != thread;
        if (attach && !AttachThreadInput(caller, thread, true)) {
            int error = Marshal.GetLastWin32Error();
            throw new System.ComponentModel.Win32Exception(error,
                "Cannot attach input from thread " + caller + ". " + DescribeFocus(thread, dialog, control));
        }
        IntPtr previous;
        int focusError;
        try {
            SetForegroundWindow(dialog);
            previous = SetFocus(control);
            focusError = Marshal.GetLastWin32Error();
        } finally {
            if (attach && !AttachThreadInput(caller, thread, false)) {
                int error = Marshal.GetLastWin32Error();
                throw new System.ComponentModel.Win32Exception(error,
                    "Cannot detach input from thread " + caller + ". " + DescribeFocus(thread, dialog, control));
            }
        }
        var watch = System.Diagnostics.Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < 5000) {
            var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf(typeof(GuiThreadInfo)) };
            if (GetGUIThreadInfo(thread, ref info) && info.Focus == control) return;
            System.Threading.Thread.Sleep(25);
        }
        throw new InvalidOperationException(String.Format(
            "The dialog did not focus the requested control. caller={0}, previous=0x{1:X}, focusError={2}, {3}",
            caller, previous.ToInt64(), focusError, DescribeFocus(thread, dialog, control)));
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
    [MervinInstallerUi]::Children($Window.Handle) |
        Select-Object Text, ClassName, Enabled |
        ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutputDir "$Name.controls.json")
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
    Write-Host "Captured $Name."
}

function Get-InstallerState {
    foreach ($window in [MervinInstallerUi]::Windows($setup.Id)) {
        $controls = @([MervinInstallerUi]::Children($window.Handle))
        $text = ($controls | ForEach-Object { $_.Text }) -join "`n"
        [pscustomobject]@{ Window = $window; Controls = $controls; Text = $text }
    }
}

function Get-EnabledButtons($State, [string]$TextPattern) {
    $State.Controls | Where-Object {
        $_.ClassName -eq 'Button' -and $_.Enabled -and ($_.Text -replace '&', '') -match $TextPattern
    }
}

function Wait-InstallerPage([string]$Name, [string]$TextPattern, [string]$ButtonPattern, [int]$Seconds = 45) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $readyHandle = [IntPtr]::Zero
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $candidate = $null
        foreach ($state in @(Get-InstallerState)) {
            $script:currentDialog = $state.Window
            if ($state.Window.Enabled -and $state.Text -match $TextPattern -and @(Get-EnabledButtons $state $ButtonPattern).Count -eq 1) {
                $candidate = $state
                break
            }
            if ($state.Text -match 'ended prematurely|installation failed|Could not|Error [0-9]') {
                throw "The installer reported an error while waiting for $Name. $($state.Text)"
            }
        }
        # PrepareDlg shares WelcomeDlg's title but its Next button is disabled.
        # Require a ready button and a stable window before taking its screenshot.
        if ($candidate) {
            if ($candidate.Window.Handle -eq $readyHandle) { return $candidate }
            $readyHandle = $candidate.Window.Handle
        } else {
            $readyHandle = [IntPtr]::Zero
        }
        if ($setup.HasExited) { throw "The installer exited with $($setup.ExitCode) while waiting for $Name." }
        Start-Sleep -Milliseconds 150
    }
    throw "Timed out waiting for $Name. UserInteractive=$([Environment]::UserInteractive), session=$((Get-Process -Id $PID).SessionId)."
}

function Click-Control($State, [string]$TextPattern) {
    $buttons = @(Get-EnabledButtons $State $TextPattern)
    if ($buttons.Count -ne 1) { throw "Expected one enabled button matching $TextPattern, found $($buttons.Count)." }
    [MervinInstallerUi]::FocusControl($State.Window.Handle, $buttons[0].Handle)
    if (-not [MervinInstallerUi]::PostMessage($buttons[0].Handle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw "Could not click $TextPattern."
    }
}

function Start-InstallerSession([string]$Name) {
    if ($script:setup) {
        if (-not $script:setup.HasExited) { throw 'The previous installer session is still running.' }
        $script:setup.Dispose()
        $script:setup = $null
    }
    $script:log = Join-Path $OutputDir "$Name.log"
    $script:currentDialog = $null
    $script:setup = Start-Process $msiexec -ArgumentList "/i `"$Msi`" /norestart /l*v `"$script:log`"" -PassThru
    $script:installAttempted = $true
    Write-Host "$Name installer process $($script:setup.Id)."
}

function Get-DestinationField($State) {
    $edits = @($State.Controls | Where-Object { $_.ClassName -in @('Edit', 'RichEdit20W') -and $_.Enabled })
    if ($edits.Count -ne 1) { throw "Expected one destination field, found $($edits.Count)." }
    $edits[0]
}

function Set-Destination($State) {
    # MSI commits PathEdit on focus loss. Keep real focus and edit notifications.
    $edit = Get-DestinationField $State
    [MervinInstallerUi]::FocusControl($State.Window.Handle, $edit.Handle)
    [MervinInstallerUi]::SetText($edit.Handle, "$installDir\")
    $written = [MervinInstallerUi]::Children($State.Window.Handle) | Where-Object { $_.Handle -eq $edit.Handle }
    if ($written.Text.TrimEnd('\') -ne $installDir) { throw 'The destination field did not retain the chosen path.' }
}

function Complete-InstallerSession([int]$ExpectedCode = 0) {
    if (-not $setup.WaitForExit(30000)) { throw 'The installer did not exit after Finish.' }
    if ($setup.ExitCode -ne $ExpectedCode) { throw "Installer returned $($setup.ExitCode), expected $ExpectedCode. See $log" }
}

function Assert-NoLaunch {
    # Poll for delayed launches and check the action log for short-lived ones.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt 3) {
        if (Get-Process MervinPDF -ErrorAction SilentlyContinue) { throw "Unexpected application launch. See $log" }
        Start-Sleep -Milliseconds 100
    }
    if (Select-String -LiteralPath $log -Quiet -Pattern 'Doing action: LaunchMervin\b|Action start [^\r\n]*: LaunchMervin\.') {
        throw "The installer executed LaunchMervin in a no-launch scenario. See $log"
    }
}

function Wait-SuccessfulFinish([string]$EvidencePrefix) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $capturedProgress = $false
    while ($watch.Elapsed.TotalSeconds -lt 180) {
        foreach ($state in @(Get-InstallerState)) {
            $script:currentDialog = $state.Window
            if ($state.Window.Enabled -and $state.Text -match 'Completed the .+ Setup Wizard' -and
                @(Get-EnabledButtons $state '^Finish$').Count -eq 1) {
                return $state
            }
            if (-not $capturedProgress -and $state.Text -match 'Installing Mervin|Please wait while') {
                Save-WindowEvidence $state.Window "$EvidencePrefix-progress"
                $capturedProgress = $true
                $script:progressSeen = $true
            }
            if ($state.Text -match 'ended prematurely|installation failed|Could not|Error [0-9]') {
                throw "Installation failed. $($state.Text)"
            }
        }
        if ($setup.HasExited) { throw "Installer exited before successful Finish with $($setup.ExitCode)." }
        Start-Sleep -Milliseconds 100
    }
    throw 'Timed out waiting for the successful Finish page.'
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
    Start-InstallerSession 'cancel-install'
    $state = Wait-InstallerPage 'Welcome' 'Welcome to' '^Next\s*>?$'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Destination folder' 'Destination Folder' '^Next\s*>?$'
    Set-Destination $state
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Ready to install' 'Ready to install' '^Install$'
    Click-Control $state '^<?\s*Back$'
    $state = Wait-InstallerPage 'Destination after Back' 'Destination Folder' '^Next\s*>?$'
    if ((Get-DestinationField $state).Text.TrimEnd('\') -ne $installDir) { throw 'Back lost the chosen destination.' }
    Click-Control $state '^<?\s*Back$'
    $state = Wait-InstallerPage 'Welcome after Back' 'Welcome to' '^Next\s*>?$'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Destination after Welcome' 'Destination Folder' '^Next\s*>?$'
    if ((Get-DestinationField $state).Text.TrimEnd('\') -ne $installDir) { throw 'Returning through Welcome lost the destination.' }
    Save-WindowEvidence $state.Window 'cancel-destination-retained'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Ready before cancellation' 'Ready to install' '^Install$'
    Click-Control $state '^Cancel$'
    $state = Wait-InstallerPage 'Cancel confirmation' 'Are you sure you want to cancel' '^No$'
    Click-Control $state '^No$'
    $state = Wait-InstallerPage 'Ready after declining cancellation' 'Ready to install' '^Install$'
    Click-Control $state '^Cancel$'
    $state = Wait-InstallerPage 'Cancel confirmation' 'Are you sure you want to cancel' '^Yes$'
    Click-Control $state '^Yes$'
    $state = Wait-InstallerPage 'Cancelled installation' 'Setup Wizard was interrupted' '^Finish$'
    Save-WindowEvidence $state.Window 'cancel-finish'
    Click-Control $state '^Finish$'
    Complete-InstallerSession 1602
    Assert-NoLaunch
    if ((Test-Path $appExe) -or @(Get-MervinProducts).Count -ne 0 -or
        (Get-ItemPropertyValue 'HKCU:\Software\Mervin PDF' -Name InstallDir -ErrorAction SilentlyContinue) -or
        (Test-Path (Join-Path ([Environment]::GetFolderPath('Programs')) 'Mervin PDF.lnk'))) {
        throw 'Cancelling before Install left an installation or shortcut.'
    }

    Start-InstallerSession 'unchecked-install'
    $state = Wait-InstallerPage 'Welcome' 'Welcome to' '^Next\s*>?$'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Destination folder' 'Destination Folder' '^Next\s*>?$'
    Set-Destination $state
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Ready to install' 'Ready to install' '^Install$'
    Click-Control $state '^Install$'
    $finish = Wait-SuccessfulFinish 'unchecked'
    $checkbox = @(Get-EnabledButtons $finish '^Launch Mervin PDF$')
    if ($checkbox.Count -ne 1 -or [MervinInstallerUi]::CheckState($checkbox[0].Handle) -ne 1) {
        throw 'Launch Mervin PDF must be checked by default.'
    }
    Click-Control $finish '^Launch Mervin PDF$'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ([MervinInstallerUi]::CheckState($checkbox[0].Handle) -ne 0 -and $watch.Elapsed.TotalSeconds -lt 5) {
        Start-Sleep -Milliseconds 100
    }
    if ([MervinInstallerUi]::CheckState($checkbox[0].Handle) -ne 0) { throw 'The launch checkbox did not become unchecked.' }
    Save-WindowEvidence $finish.Window 'unchecked-finish'
    Assert-NoLaunch
    Click-Control $finish '^Finish$'
    Complete-InstallerSession
    Assert-NoLaunch
    if (-not (Test-Path $appExe) -or @(Get-MervinProducts).Count -ne 1) { throw 'Unchecked Finish did not leave one installed application.' }
    if ((Get-ItemPropertyValue 'HKCU:\Software\Mervin PDF' -Name InstallDir).TrimEnd('\') -ne $installDir) {
        throw 'Unchecked installation registered the wrong directory.'
    }

    Start-InstallerSession 'maintenance-repair'
    $state = Wait-InstallerPage 'Maintenance welcome' 'Welcome' '^Next\s*>?$'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Maintenance choices' 'Change, repair, or remove' '^Repair$'
    $change = @($state.Controls | Where-Object { $_.ClassName -eq 'Button' -and ($_.Text -replace '&', '') -eq 'Change' })
    if ($change.Count -ne 1 -or $change[0].Enabled) { throw 'Maintenance must disable Change for this package.' }
    Click-Control $state '^<?\s*Back$'
    $state = Wait-InstallerPage 'Maintenance welcome after Back' 'Welcome' '^Next\s*>?$'
    Click-Control $state '^Next\s*>?$'
    $state = Wait-InstallerPage 'Maintenance choices' 'Change, repair, or remove' '^Remove$'
    Click-Control $state '^Remove$'
    $state = Wait-InstallerPage 'Ready to remove' 'Ready to remove' '^Remove$'
    Save-WindowEvidence $state.Window 'maintenance-remove-confirmation'
    Click-Control $state '^<?\s*Back$'
    $state = Wait-InstallerPage 'Maintenance choices after Back' 'Change, repair, or remove' '^Repair$'
    Click-Control $state '^Repair$'
    $state = Wait-InstallerPage 'Ready to repair' 'Ready to repair' '^Repair$'
    Save-WindowEvidence $state.Window 'maintenance-repair-confirmation'
    Click-Control $state '^Repair$'
    $finish = Wait-SuccessfulFinish 'maintenance'
    if (@($finish.Controls | Where-Object { ($_.Text -replace '&', '') -eq 'Launch Mervin PDF' }).Count -ne 0) {
        throw 'Maintenance Finish must hide the launch checkbox.'
    }
    Save-WindowEvidence $finish.Window 'maintenance-finish'
    Click-Control $finish '^Finish$'
    Complete-InstallerSession
    Assert-NoLaunch
    if (-not (Test-Path $appExe) -or @(Get-MervinProducts).Count -ne 1) { throw 'Maintenance repair did not preserve the installed product.' }
    if ((Get-ItemPropertyValue 'HKCU:\Software\Mervin PDF' -Name InstallDir).TrimEnd('\') -ne $installDir) {
        throw 'Maintenance repair changed the installation directory.'
    }

    # A second fresh installation is needed to exercise checked Finish. Running
    # the same installed product again would test maintenance instead.
    $uninstallLog = Join-Path $OutputDir 'between-cases-uninstall.log'
    $uninstall = Start-Process $msiexec -ArgumentList "/x `"$Msi`" /qn /norestart /l*v `"$uninstallLog`"" -PassThru
    Write-Host "Between-cases uninstaller process $($uninstall.Id)."
    try {
        if (-not $uninstall.WaitForExit(120000)) {
            $uninstall.Kill(); $uninstall.WaitForExit(10000) | Out-Null
            throw 'Uninstall between UI cases timed out.'
        }
        if ($uninstall.ExitCode -ne 0 -or (Test-Path $appExe) -or @(Get-MervinProducts).Count -ne 0) { throw 'Uninstall between UI cases failed.' }
    } finally { $uninstall.Dispose() }

    $progressSeen = $false
    Start-InstallerSession 'interactive-install'

    $state = Wait-InstallerPage 'Welcome' 'Welcome to' '^Next\s*>?$'
    Save-WindowEvidence $state.Window '01-welcome'
    Click-Control $state '^Next\s*>?$'

    $state = Wait-InstallerPage 'Destination folder' 'Destination Folder' '^Next\s*>?$'
    Set-Destination $state
    Save-WindowEvidence $state.Window '02-destination'
    Click-Control $state '^Next\s*>?$'

    $state = Wait-InstallerPage 'Ready to install' 'Ready to install' '^Install$'
    Save-WindowEvidence $state.Window '03-ready'
    Click-Control $state '^Install$'

    $finish = Wait-SuccessfulFinish '04'
    $checkbox = @($finish.Controls | Where-Object { ($_.Text -replace '&', '') -eq 'Launch Mervin PDF' })
    if ($checkbox.Count -ne 1 -or [MervinInstallerUi]::CheckState($checkbox[0].Handle) -ne 1) {
        throw 'Launch Mervin PDF must be checked by default.'
    }
    if (-not (Test-Path $appExe)) { throw 'The wizard did not install into the chosen destination.' }
    if (Get-Process MervinPDF -ErrorAction SilentlyContinue) { throw 'Mervin launched before Finish was clicked.' }
    Save-WindowEvidence $finish.Window '05-finish-launch-checked'
    Click-Control $finish '^Finish$'
    Complete-InstallerSession
    if (-not (Select-String -LiteralPath $log -Quiet -Pattern 'Doing action: LaunchMervin\b|Action start [^\r\n]*: LaunchMervin\.')) {
        throw 'Checked Finish did not log LaunchMervin. The negative launch-log oracle cannot be trusted.'
    }

    $watch = [Diagnostics.Stopwatch]::StartNew()
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
        uncheckedFinishNoLaunch = $true
        cancelledBeforeInstall = $true
        destinationSurvivesBack = $true
        maintenanceRepairNoLaunch = $true
        launchedExecutable = $launchedApp.Path
        launchedProcessId = $launchedApp.Id
        runtimeModules = @($runtimeModules)
        progressObserved = $progressSeen
        windowTitle = $appWindow.Text
    } | ConvertTo-Json | Set-Content (Join-Path $OutputDir 'result.json')
    Write-Host 'Native Back/Cancel, unchecked Finish, maintenance Repair, checked Finish and application launch passed.'
} catch {
    if ($setup -and -not $setup.HasExited) {
        $visibleState = Get-InstallerState | Select-Object -First 1
        if ($visibleState) { $currentDialog = $visibleState.Window }
    }
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
        if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit(10000) | Out-Null }
        $process.Dispose()
    }
    if ($launchedApp) { $launchedApp.Dispose() }
    if ($setup) {
        if (-not $setup.HasExited) { $setup.Kill(); $setup.WaitForExit(10000) | Out-Null }
        $setup.Dispose()
    }
    if ($installAttempted) {
        $log = Join-Path $OutputDir 'ui-test-uninstall.log'
        $uninstall = Start-Process $msiexec -ArgumentList "/x `"$Msi`" /qn /norestart /l*v `"$log`"" -PassThru
        Write-Host "Uninstaller process $($uninstall.Id)."
        if (-not $uninstall.WaitForExit(120000)) {
            $uninstall.Kill(); $uninstall.WaitForExit(10000) | Out-Null
            throw 'UI test cleanup timed out.'
        }
        if ($uninstall.ExitCode -notin @(0, 1605)) { throw "UI test uninstall returned $($uninstall.ExitCode)." }
        if (Test-Path $appExe) { throw 'UI test uninstall left the application installed.' }
    }
}
