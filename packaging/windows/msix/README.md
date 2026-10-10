# Microsoft Store package

`scripts/build-msix.ps1` packages the self-contained tree from `scripts/deploy.ps1`
as an unsigned x64 MSIX. The reserved Store identity belongs to product
`9PPM4Z5G1LD0`. Microsoft Store signs the approved package. This does not sign the
standalone MSI or executable distributed on GitHub.

The manifest runs the existing desktop executable at medium integrity with
`runFullTrust`. It registers `.pdf` through the package manifest. The deployed
app-local Microsoft runtime DLLs travel inside the package, so it has no
Microsoft.VCLibs framework dependency.

The Store package version follows the MSI ordering, with a reserved fourth field
of zero. For example, public version `1.64.10-rc1` becomes `1.64.1001.0` and
`1.64.10` becomes `1.64.1099.0`. The Store requires a nonzero major version. The
executable and package filenames keep the public version.

The package requires Windows 11 build 22000 and newer, matching the project's
Windows support policy. `MaxVersionTested="10.0.22621.1555"` enables the supported
`registeredAUMID` Default apps link on updated Windows 11. That declaration does
not establish that Windows 11 testing has passed. The Windows Server 2025 CI
runner checks packaging and deployment, and Windows 11 needs a separate native
test of the Default apps page and desktop integration.

## Assets

The PNGs in `Assets` are raster exports of the existing approved
`resources/icons/mervin-icon.svg` artwork. They were rendered directly at 44,
50 and 150 pixels using librsvg and Cairo. They introduce no new icon design.
The package metadata is English. The executable still embeds all existing Qt
language catalogs.

`Listing/StoreIcon.png` is the same SVG rendered at 300 by 300 pixels for the
Partner Center Store listing. It is uploaded with the listing and is not included
in the installed application payload.

## References

- [Manual MSIX packaging](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)
- [Store package version rules](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
