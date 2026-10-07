# WiX MSI packaging

`mervin.wxs` defines the Windows installer. `scripts/deploy.ps1 -Installer` builds
one per-user MSI from the staged `deploy` directory. It works both interactively
and through deployment tools.

No administrator access is needed. The package uses `Scope="perUser"`.

```powershell
msiexec /i MervinPDF-<version>.msi          # interactive wizard
msiexec /i MervinPDF-<version>.msi /qn      # silent deployment
msiexec /x MervinPDF-<version>.msi /qn      # silent uninstall
```

The native Windows Installer wizard offers an installation folder and a checked
**Launch Mervin PDF** option on its successful completion page. It uses WiX's
standard folder, progress, files-in-use and maintenance dialogs, with a shorter
navigation sequence that omits license acceptance. License texts are installed
alongside the application. Launching is limited to successful interactive
installation, including a major upgrade. Repair, uninstall, `/passive` and `/qn`
never launch the application. The in-app updater relaunches it after its own
`/passive` installation.

The default destination is `%LOCALAPPDATA%\Mervin PDF`. Upgrades reuse the recorded
installation folder; an explicit `INSTALLFOLDER` command-line property takes
precedence. The installer adds a Start menu shortcut and an entry in Windows
Settings. It registers Mervin as an available PDF handler without changing the
user's chosen default.

Uninstall removes installed application files, empty application folders, the shortcut and Mervin's PDF
handler registration. It removes only Mervin's values from shared registry keys,
preserving other PDF handlers. Preferences, recent files and OCR languages under
`%APPDATA%\MervinPDF` are user data and remain intact. The packaged
`eng.traineddata` component is permanent and never overwrites an existing English
model. Other downloaded languages are not tracked by MSI.

Major upgrades replace the previous MSI installation and block downgrades. Keep
the `UpgradeCode` (`A1E04BD8-CF2C-4B78-9506-C72EBCD29617`) fixed across versions or
upgrades will break.

The public version can include `-rcN`. MSI stores a numeric upgrade version from
the generated CMake metadata and includes the public version in its product name.
For example, `1.64.10-rc1` uses MSI version `1.64.1001`; stable `1.64.10` uses
`1.64.1099`. This keeps candidate upgrades and the later stable upgrade in order.
See [the release policy](../../docs/RELEASING.md) for the mapping and limits.

## Toolchain

The build uses WiX Toolset 7.0.0 and its matching UI extension. The source uses
WiX v6/v7 syntax. Install the CLI and extension:

```powershell
winget install -e --id WiXToolset.WiXCLI --version 7.0.0.0
wix extension add --global WixToolset.UI.wixext/7.0.0
```

The deployment script passes `-ext WixToolset.UI.wixext` to `wix build`.
Launching the application uses a Windows Installer executable action, so the
package does not need a utility extension or a separate bootstrapper.

The navigation uses the native dialogs described in the
[WiX UI documentation](https://docs.firegiant.com/wix/tools/wixext/wixui/).

## Licensing note (OSMF)

WiX v6/v7 gate use behind the **Open Source Maintenance Fee (OSMF)** EULA. Per the
OSMF terms, organizations with **> $10,000 annual revenue** are asked to sponsor the
WiX project (~$10–60/month by org size via GitHub Sponsors); individuals and smaller
orgs are exempt. `deploy.ps1` runs `wix eula accept wix7` (a per-user, idempotent
acceptance) to encode the project's decision to accept the EULA. Evaluating /
fulfilling the sponsorship obligation is a separate compliance step.

See https://docs.firegiant.com/wix/osmf/.
