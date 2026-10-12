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

The PNGs in `Assets` are raster exports of the approved
`resources/icons/mervin-icon.svg` artwork. `resources/icons/export_icon_frames.py`
writes all of them, as described in the [icon notes](../../../resources/icons/README.md).
`Square44x44Logo.png`, `StoreLogo.png` and `Square150x150Logo.png` are rendered
directly at 44, 50 and 150 pixels. The package metadata is English. The executable
still embeds all existing Qt language catalogs.

Every frame of `mervin-icon.ico` is also copied unchanged as
`Square44x44Logo.targetsize-<size>.png`, `..._altform-unplated.png` and
`..._altform-lightunplated.png`. Windows uses these plain, dark-theme and
light-theme forms for the taskbar, Start menu, Alt+Tab and Explorer file icons.
Microsoft requires all three forms, even when they are identical. Without the
unplated forms, Windows shrinks the icon onto a plate, which is the accent color
on the taskbar and gray in Start.

Windows finds these qualified file names only through the package's
`resources.pri`. `build-msix.ps1` runs `makepri createconfig` and removes the
`<packaging>` element, so a flat package gets one index. It then runs
`makepri new` on a copy of `Assets` alone, so application folder names are never
read as language or scale qualifiers. The build first runs
`export_icon_frames.py --check`, so every ICO frame has its three copies. A
`makepri dump` check then fails the build unless every size is indexed in all
three forms under `Square44x44Logo.png`. The configuration, the copy and
`resources.pri.xml` remain beside the MSIX in its `-msix-pri` directory for
inspection. CI uploads that directory with the Windows verification artifacts.

The Windows shell caches packaged app icons. To check the icons on Windows 11,
install a test package that replaces any earlier Mervin package, then unpin and
pin it again. Check the taskbar in dark and light Windows modes, Start pins and
All apps, Alt+Tab, and Explorer `.pdf` icons in several views.

`Listing/StoreIcon.png` is the same SVG rendered at 300 by 300 pixels for the
Partner Center Store listing. It is uploaded with the listing and is not included
in the installed application payload. Upload it again whenever the icon changes,
or the Store page keeps showing the previous icon.

## References

- [Manual MSIX packaging](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)
- [App icon construction](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction)
- [MakePri command options](https://learn.microsoft.com/en-us/windows/uwp/app-resources/makepri-exe-command-options)
- [Store package version rules](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
