# WiX MSI packaging

`mervin.wxs` is the source for the **per-user MSI** - the managed / silent-deployment
counterpart to the friendly NSIS `.exe` (`packaging/nsis/mervin.nsi`). Both are
produced together by `scripts/deploy.ps1 -Installer` from the same staged `deploy`
tree.

Install (no admin needed - per-user, `Scope="perUser"`):

```powershell
msiexec /i MervinPDF-<version>.msi          # basic UI
msiexec /i MervinPDF-<version>.msi /qn       # fully silent (GPO / Intune / scripts)
msiexec /x MervinPDF-<version>.msi /qn       # silent uninstall
```

It installs to `%LOCALAPPDATA%\Mervin PDF`, adds a Start-menu shortcut, registers an
Apps & Features entry under HKCU, and seeds `eng.traineddata` into
`%APPDATA%\MervinPDF\tessdata`. Major-upgrade handling is built in (a newer version
replaces an older one; downgrades are blocked). The `UpgradeCode`
(`A1E04BD8-CF2C-4B78-9506-C72EBCD29617`) is fixed across versions - **do not change it**
or upgrades will break.

## Toolchain

Built with the WiX Toolset v6/v7 `wix` CLI (runs on the .NET 8 runtime; no .NET SDK
required). Install:

```powershell
winget install -e --id WiXToolset.WiXCLI
```

## Licensing note (OSMF)

WiX v6/v7 gate use behind the **Open Source Maintenance Fee (OSMF)** EULA. Per the
OSMF terms, organizations with **> $10,000 annual revenue** are asked to sponsor the
WiX project (~$10–60/month by org size via GitHub Sponsors); individuals and smaller
orgs are exempt. `deploy.ps1` runs `wix eula accept wix7` (a per-user, idempotent
acceptance) to encode the project's decision to accept the EULA. Evaluating /
fulfilling the sponsorship obligation is a separate compliance step.

See https://docs.firegiant.com/wix/osmf/.
