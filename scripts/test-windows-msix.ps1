<#
.SYNOPSIS
  Install, activate, upgrade, and remove a signed test copy of a Store MSIX.
.DESCRIPTION
  Run under Windows PowerShell 5.1 on a disposable GitHub-hosted runner. The
  unsigned submission package stays unchanged. A temporary certificate signs
  only diagnostic copies and is removed from both certificate stores afterward.
  Logs, screenshots, and an isolated profile remain under C:\dev-temp.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Msix,
    [string]$Fixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_OS -ne 'Windows' -or
    $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'MSIX lifecycle tests require a disposable GitHub-hosted Windows runner.'
}
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this script with Windows PowerShell 5.1 so Appx deployment runs natively.'
}
$admin = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $admin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The disposable runner must be elevated to trust its temporary test certificate.'
}
Import-Module Appx

$name = 'Lincarl.MervinPDF'
$publisher = 'CN=7444C5EF-8AD3-459D-A81A-9B90254807FC'
$family = 'Lincarl.MervinPDF_s4kmqx4fnhk0j'
$aumid = "$family!MervinPDF"
$packagePath = (Resolve-Path -LiteralPath $Msix).Path
$build = Split-Path $packagePath -Parent
$fixturePath = if ($Fixture) { [IO.Path]::GetFullPath($Fixture) } else { Join-Path $build 'tests\fixtures\properties.pdf' }
$documentName = [IO.Path]::GetFileName($fixturePath)
$userChoice = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice'
$certificate = $null
$app = $null
$installedByTest = $false

function Assert-Msix([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-PdfDefault {
    if (Test-Path -LiteralPath $userChoice) {
        (Get-Item -LiteralPath $userChoice).GetValue('ProgId', $null)
    }
}

Assert-Msix (Test-Path -LiteralPath $fixturePath) 'The PDF fixture is missing. Supply -Fixture or configure with MERVIN_BUILD_TESTS=ON.'
Assert-Msix (@(Get-AppxPackage -Name $name).Count -eq 0) 'An existing Mervin MSIX prevents lifecycle testing.'
Assert-Msix (-not (Get-Process MervinPDF -ErrorAction SilentlyContinue)) 'Mervin is already running.'
$originalHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
$beforeDefault = Get-PdfDefault

$suffix = -join ([char[]]'abcdefghijklmnopqrstuvwxyz' | Get-Random -Count 4)
$work = "C:\dev-temp\mervin-msix-$suffix"
Assert-Msix (-not (Test-Path -LiteralPath $work)) 'MSIX test directory already exists.'
New-Item -ItemType Directory -Path $work | Out-Null
if ($env:GITHUB_ENV) { "MERVIN_MSIX_TEST_DIR=$work" | Out-File $env:GITHUB_ENV -Append -Encoding utf8 }
Start-Transcript -Path (Join-Path $work 'test.log') | Out-Null

# Locate one SDK containing both tools so packaging and signing use matching SDKs.
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$sdk = Get-ChildItem -LiteralPath $sdkRoot -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName 'x64\MakeAppx.exe')) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'x64\signtool.exe'))
    } | Select-Object -First 1
Assert-Msix ($null -ne $sdk) 'A Windows SDK with MakeAppx and SignTool is required.'
$makeAppx = Join-Path $sdk.FullName 'x64\MakeAppx.exe'
$signTool = Join-Path $sdk.FullName 'x64\signtool.exe'

function Invoke-SdkTool([string]$Tool, [string[]]$Arguments, [string]$Step) {
    & $Tool @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $work "$Step.log") | Write-Output
    Assert-Msix ($LASTEXITCODE -eq 0) "$Step failed with exit code $LASTEXITCODE."
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class MervinMsixTest
{
    [ComImport, Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]
    private class ApplicationActivationManager { }

    [ComImport, Guid("2E941141-7F97-4756-BA1D-9DECDE894A3D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IApplicationActivationManager
    {
        [PreserveSig] int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appId,
            [MarshalAs(UnmanagedType.LPWStr)] string arguments, uint options, out uint processId);
        [PreserveSig] int ActivateForFile([MarshalAs(UnmanagedType.LPWStr)] string appId,
            IntPtr items, [MarshalAs(UnmanagedType.LPWStr)] string verb, out uint processId);
        [PreserveSig] int ActivateForProtocol([MarshalAs(UnmanagedType.LPWStr)] string appId,
            IntPtr items, out uint processId);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ShellExecuteInfo
    {
        public uint Size, Mask;
        public IntPtr Window;
        public string Verb, File, Parameters, Directory;
        public int Show;
        public IntPtr Instance, IdList;
        public string Class;
        public IntPtr ClassKey;
        public uint HotKey;
        public IntPtr Icon, Process;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetPackageFullName(IntPtr process, ref uint length, StringBuilder name);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShellExecuteEx(ref ShellExecuteInfo info);
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
    private static extern int AssocQueryString(uint flags, uint kind, string association,
        string extra, StringBuilder value, ref uint length);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y,
        int width, int height, uint flags);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    public static Process Activate(string appId, string arguments)
    {
        var manager = (IApplicationActivationManager)new ApplicationActivationManager();
        try {
            uint processId;
            Marshal.ThrowExceptionForHR(manager.ActivateApplication(appId, arguments, 2, out processId));
            var process = Process.GetProcessById((int)processId);
            // Retain the process handle so ExitCode remains available after a quick launch.
            var handle = process.Handle;
            return process;
        } finally {
            Marshal.ReleaseComObject(manager);
        }
    }

    public static string PackageName(Process process)
    {
        uint length = 0;
        int result = GetPackageFullName(process.Handle, ref length, null);
        if (result != 122) throw new Win32Exception(result, "GetPackageFullName did not report a packaged process.");
        var name = new StringBuilder((int)length);
        result = GetPackageFullName(process.Handle, ref length, name);
        if (result != 0) throw new Win32Exception(result);
        return name.ToString();
    }

    public static string AssociationAppId(string progId)
    {
        // Fixed ProgID avoids resolving the user's default instead of this handler.
        const uint fixedProgId = 0x800;
        const uint appId = 21;
        uint length = 0;
        AssocQueryString(fixedProgId, appId, progId, null, null, ref length);
        if (length == 0) return null;
        var value = new StringBuilder((int)length);
        return AssocQueryString(fixedProgId, appId, progId, null, value, ref length) == 0
            ? value.ToString() : null;
    }

    public static void OpenWithHandler(string progId, string path)
    {
        // Packaged desktop apps use Shell file associations, not the UWP
        // Windows.File activation contract used by ActivateForFile.
        var info = new ShellExecuteInfo {
            Size = (uint)Marshal.SizeOf(typeof(ShellExecuteInfo)),
            Mask = 0x001 | 0x040 | 0x100 | 0x400,
            Verb = "open", File = path, Class = progId, Show = 1
        };
        try {
            if (!ShellExecuteEx(ref info)) throw new Win32Exception(Marshal.GetLastWin32Error());
        } finally {
            if (info.Process != IntPtr.Zero) CloseHandle(info.Process);
        }
    }
}
'@

function Get-TestPackage([string]$ExpectedVersion) {
    $packages = @(Get-AppxPackage -Name $name)
    Assert-Msix ($packages.Count -eq 1) 'Expected exactly one installed Mervin package.'
    $package = $packages[0]
    Assert-Msix ($package.PackageFamilyName -eq $family) 'Installed package family differs from the reserved Store identity.'
    Assert-Msix ($package.Version.ToString() -eq $ExpectedVersion) 'Installed package version is incorrect.'
    Assert-Msix ($package.Status -eq 'Ok') 'Windows reports an unhealthy package.'
    $package
}

function Save-AppScreenshot([Diagnostics.Process]$Process, [string]$FileName) {
    Assert-Msix ([MervinMsixTest]::SetWindowPos($Process.MainWindowHandle, [IntPtr]::Zero, 0, 0, 1536, 864, 20)) 'Cannot size the application window for its screenshot.'
    Start-Sleep -Milliseconds 750
    $rect = New-Object MervinMsixTest+Rect
    Assert-Msix ([MervinMsixTest]::GetWindowRect($Process.MainWindowHandle, [ref]$rect)) 'Cannot read the application window rectangle.'
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    Assert-Msix ($width -gt 0 -and $height -gt 0) 'Application window has no drawable area.'
    @{ Width = $width; Height = $height; Dpi = [MervinMsixTest]::GetDpiForWindow($Process.MainWindowHandle) } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work "$FileName.json")
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try {
            Assert-Msix ([MervinMsixTest]::PrintWindow($Process.MainWindowHandle, $dc, 2)) 'Windows could not capture the packaged application.'
        } finally { $graphics.ReleaseHdc($dc) }
        $bitmap.Save((Join-Path $work $FileName), [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Start-PackagedApp([string]$Arguments, [string]$ExpectedPackageName) {
    $process = [MervinMsixTest]::Activate($aumid, $Arguments)
    Write-Output "Activated packaged application process $($process.Id)" | Out-Host
    Assert-Msix ([MervinMsixTest]::PackageName($process) -eq $ExpectedPackageName) 'The activated process did not receive the expected package identity.'
    $process
}

function Wait-DocumentWindow([Diagnostics.Process]$Process) {
    $deadline = (Get-Date).AddSeconds(60)
    do {
        Start-Sleep -Milliseconds 250
        $Process.Refresh()
    } while (-not $Process.HasExited -and
             ($Process.MainWindowHandle -eq [IntPtr]::Zero -or $Process.MainWindowTitle.IndexOf($documentName, [StringComparison]::OrdinalIgnoreCase) -lt 0) -and
             (Get-Date) -lt $deadline)
    Assert-Msix (-not $Process.HasExited) 'Packaged application exited before displaying its document.'
    Assert-Msix ($Process.MainWindowHandle -ne [IntPtr]::Zero -and $Process.MainWindowTitle.IndexOf($documentName, [StringComparison]::OrdinalIgnoreCase) -ge 0) 'Packaged application did not show the requested PDF.'
}

function Get-PackagePdfProgId {
    $openWith = [Microsoft.Win32.Registry]::ClassesRoot.OpenSubKey('.pdf\OpenWithProgids')
    Assert-Msix ($null -ne $openWith) 'Windows did not register any PDF Open With handlers.'
    try {
        $handlers = @($openWith.GetValueNames() | ForEach-Object {
            [pscustomobject]@{ ProgId = $_; AppId = [MervinMsixTest]::AssociationAppId($_) }
        })
    } finally { $openWith.Dispose() }
    $handlers | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'pdf-handlers.json')
    $matching = @($handlers | Where-Object { $_.AppId -eq $aumid })
    Assert-Msix ($matching.Count -eq 1) 'Expected exactly one PDF handler for the installed package. See pdf-handlers.json.'
    $matching[0].ProgId
}

try {
    Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'windows-version.json')
    if (Get-Command Set-DisplayResolution -ErrorAction SilentlyContinue) {
        Set-DisplayResolution -Width 1920 -Height 1080 -Force | Out-Host
    } else {
        Write-Warning 'Display resolution cannot be changed by this server. Review screenshot dimensions before using images in the Store.'
    }
    $unpacked = Join-Path $work 'unpacked'
    Invoke-SdkTool $makeAppx @('unpack', '/p', $packagePath, '/d', $unpacked, '/o') 'unpack'
    $manifestPath = Join-Path $unpacked 'AppxManifest.xml'
    [xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
    Assert-Msix ($manifest.Package.Identity.Name -eq $name) 'Package identity name is incorrect.'
    Assert-Msix ($manifest.Package.Identity.Publisher -eq $publisher) 'Package publisher is incorrect.'
    $version = [version]$manifest.Package.Identity.Version
    Assert-Msix ($version.Build -lt 65535) 'Test upgrade needs a higher build version.'
    $association = @($manifest.SelectNodes('//*[local-name()="FileTypeAssociation"]/*[local-name()="SupportedFileTypes"]/*[local-name()="FileType"]'))
    Assert-Msix (@($association | Where-Object { $_.InnerText -eq '.pdf' }).Count -eq 1) 'Package must declare one PDF file association.'

    $certificate = New-SelfSignedCertificate -Type Custom -Subject $publisher `
        -FriendlyName "Mervin MSIX disposable test $suffix" -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyUsage DigitalSignature `
        -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddDays(1) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}CA=false')
    $publicCertificate = Join-Path $work 'test-certificate.cer'
    Export-Certificate -Cert $certificate -FilePath $publicCertificate | Out-Null
    Import-Certificate -FilePath $publicCertificate -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null

    $signed = Join-Path $work 'MervinPDF-install-test.msix'
    Copy-Item -LiteralPath $packagePath -Destination $signed
    Invoke-SdkTool $signTool @('sign', '/fd', 'SHA256', '/sha1', $certificate.Thumbprint, '/s', 'My', $signed) 'sign-install'
    Invoke-SdkTool $signTool @('verify', '/pa', '/v', $signed) 'verify-install-signature'
    Add-AppxPackage -Path $signed
    $installedByTest = $true
    $package = Get-TestPackage $version.ToString()
    $package | Select-Object Name, PackageFullName, PackageFamilyName, Version, InstallLocation, Status |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'installed-package.json')
    Assert-Msix ((Get-PdfDefault) -eq $beforeDefault) 'MSIX installation changed the default PDF application.'

    $profile = Join-Path $work 'profile'
    New-Item -ItemType Directory -Path $profile | Out-Null
    @'
ui_language = "en"
auto_update = false
close_to_tray = false
restore_session = false
default_zoom = "fit-page"
'@ | Set-Content -LiteralPath (Join-Path $profile 'config.toml') -Encoding utf8
    $sentinel = Join-Path $profile 'retained-user-data.txt'
    'Preserve isolated user data across package upgrade and removal.' |
        Set-Content -LiteralPath $sentinel -Encoding utf8
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
    $testPdf = Join-Path $work $documentName
    Copy-Item -LiteralPath $fixturePath -Destination $testPdf
    $arguments = "--profile `"$profile`" --language en `"$testPdf`""

    $app = Start-PackagedApp $arguments $package.PackageFullName
    Wait-DocumentWindow $app
    Save-AppScreenshot $app 'installed-application.png'
    Assert-Msix ($app.CloseMainWindow()) 'The packaged application did not accept close.'
    Assert-Msix ($app.WaitForExit(60000)) 'Packaged application did not exit after closing its window.'
    Assert-Msix ($app.ExitCode -eq 0) 'Packaged application returned an error.'
    $app.Dispose()
    $app = $null

    # File activation supplies only the document path, as Explorer does. Seed the
    # unused disposable account so no welcome dialog or default-app prompt blocks it.
    $normalData = Join-Path $env:APPDATA 'MervinPDF'
    Assert-Msix (-not (Test-Path -LiteralPath $normalData)) 'File activation needs an unused disposable application profile.'
    New-Item -ItemType Directory -Path $normalData | Out-Null
    @'
ui_language = "en"
auto_update = false
close_to_tray = false
restore_session = false
prompted_set_default_app = true
default_zoom = "fit-page"
'@ | Set-Content -LiteralPath (Join-Path $normalData 'config.toml') -Encoding utf8
    $pdfProgId = Get-PackagePdfProgId
    Write-Output "Invoking registered PDF handler $pdfProgId"
    [MervinMsixTest]::OpenWithHandler($pdfProgId, $testPdf)
    $activationDeadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 250
        $processes = @(Get-Process MervinPDF -ErrorAction SilentlyContinue)
    } while ($processes.Count -eq 0 -and (Get-Date) -lt $activationDeadline)
    Assert-Msix ($processes.Count -eq 1) 'PDF Open With did not launch exactly one application process.'
    $app = $processes[0]
    $null = $app.Handle
    Write-Output "File activation process $($app.Id)"
    Assert-Msix ([MervinMsixTest]::PackageName($app) -eq $package.PackageFullName) 'PDF activation launched the wrong package.'
    Wait-DocumentWindow $app
    Save-AppScreenshot $app 'file-activation.png'
    Assert-Msix ($app.CloseMainWindow()) 'File-activated application did not accept close.'
    Assert-Msix ($app.WaitForExit(60000)) 'File-activated application did not close.'
    Assert-Msix ($app.ExitCode -eq 0) 'File-activated application returned an error.'
    $app.Dispose()
    $app = $null
    Assert-Msix ((Get-PdfDefault) -eq $beforeDefault) 'PDF activation changed the default PDF application.'

    # Repack the same payload with a newer package version solely for this test.
    $upgradeVersion = '{0}.{1}.{2}.0' -f $version.Major, $version.Minor, ($version.Build + 1)
    $manifest.Package.Identity.Version = $upgradeVersion
    $manifest.Save($manifestPath)
    foreach ($generated in @('AppxBlockMap.xml', '[Content_Types].xml', 'AppxSignature.p7x', 'AppxMetadata\CodeIntegrity.cat')) {
        $generatedPath = Join-Path $unpacked $generated
        if (Test-Path -LiteralPath $generatedPath) { Remove-Item -LiteralPath $generatedPath }
    }
    $upgrade = Join-Path $work 'MervinPDF-upgrade-test.msix'
    Invoke-SdkTool $makeAppx @('pack', '/d', $unpacked, '/p', $upgrade, '/h', 'SHA256', '/o') 'pack-upgrade'
    Invoke-SdkTool $signTool @('sign', '/fd', 'SHA256', '/sha1', $certificate.Thumbprint, '/s', 'My', $upgrade) 'sign-upgrade'
    Add-AppxPackage -Path $upgrade
    $package = Get-TestPackage $upgradeVersion
    Assert-Msix ((Get-PdfDefault) -eq $beforeDefault) 'MSIX upgrade changed the default PDF application.'
    Assert-Msix ((Get-FileHash -LiteralPath $sentinel).Hash -eq $sentinelHash) 'MSIX upgrade changed isolated user data.'

    $app = Start-PackagedApp $arguments $package.PackageFullName
    Wait-DocumentWindow $app
    Save-AppScreenshot $app 'upgraded-application.png'
    Assert-Msix ($app.CloseMainWindow()) 'Upgraded application did not accept close.'
    Assert-Msix ($app.WaitForExit(60000)) 'Upgraded application did not exit.'
    Assert-Msix ($app.ExitCode -eq 0) 'Upgraded application returned an error.'
    $app.Dispose()
    $app = $null
    # Appx properties can become unavailable as soon as the package is removed.
    # Preserve the resolved directory before invalidating the live package object.
    $removedInstallLocation = [string]$package.InstallLocation
    Assert-Msix (-not [string]::IsNullOrWhiteSpace($removedInstallLocation)) 'The upgraded package has no installation directory.'
    Remove-AppxPackage -Package $package.PackageFullName
    $installedByTest = $false
    Assert-Msix (@(Get-AppxPackage -Name $name).Count -eq 0) 'Uninstall left the MSIX registered.'
    Assert-Msix (-not (Test-Path -LiteralPath $removedInstallLocation)) 'Uninstall left the installed package directory.'
    Assert-Msix ((Get-FileHash -LiteralPath $sentinel).Hash -eq $sentinelHash) 'Uninstall removed isolated user data outside the package.'
    Assert-Msix ((Get-PdfDefault) -eq $beforeDefault) 'MSIX uninstall changed the default PDF application.'
    Assert-Msix ((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -eq $originalHash) 'Test changed the unsigned submission package.'
    Write-Output "MSIX install, package identity, document startup, file activation, upgrade, and uninstall passed. Diagnostics remain in $work"
} catch {
    $_ | Format-List -Property Exception, ScriptStackTrace -Force | Out-Host
    throw
} finally {
    if ($null -ne $app) {
        if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit(10000) | Out-Null }
        $app.Dispose()
    }
    if ($installedByTest) {
        Get-AppxPackage -Name $name | Remove-AppxPackage -ErrorAction Continue
    }
    if ($null -ne $certificate) {
        $trustedPath = "Cert:\LocalMachine\TrustedPeople\$($certificate.Thumbprint)"
        if (Test-Path -LiteralPath $trustedPath) { Remove-Item -LiteralPath $trustedPath -ErrorAction Continue }
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey -ErrorAction Continue
    }
    Stop-Transcript | Out-Null
}
