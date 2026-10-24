# Mervin application icon

`mervin-icon.svg` is the editable source. `mervin-icon.ico` holds the approved
raster exports, and `mervin-icon.png` is the 256 pixel preview. The smaller PNGs
in `mervin-icon/` are extracted unchanged from the ICO for Qt and Linux.

This is the selected P06 raised P design. Its artwork matches the original
`mervin-p06-raised-p` assets saved in the design-vault, enlarged to fill the icon's
full height. The name changed when the design became the application icon.

## Design

The artwork is drawn on a 256 by 256 unit grid. The page spans x34 to x215 and y9
to y247, with a fold from (157, 9) to (215, 67). The page gradient runs diagonally
from `#2aa7e0` to `#0a4d8c` in object bounding box coordinates. The fold is
`#91d9f5`, and the letter is white.

The P is a path with an even-odd opening, with no font dependency. Its transform
is `translate(24 24.8) scale(.8)`, placing it from (84, 76.8) to (176.8, 188.8).
The SVG preserves the exact curves. All sizes use the same original gradient.

The transparent 256 pixel canvas uses `viewBox="4.25 9 238 238"`. This scales the
artwork by 256/238 so the page fills the full height, like other apps' icons in
the taskbar and Start menu. The page's left edge lands at x32, one eighth of the
width, so it falls on a pixel boundary at every size divisible by eight. Rendered
pixel positions therefore differ from the grid coordinates above.

## Export and verification

The ICO contains 32-bit PNG frames at these sizes:

`16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256`

Each frame is rendered directly from the SVG onto a transparent Chromium canvas
at its own size. No intermediate downscaling, font rendering, or small-size
recoloring is used. The current frames came from Google Chrome 155.0.8059.39 on
Linux x86-64. Other browser versions can differ slightly in antialiasing, so
re-render only when the artwork changes.

After changing the SVG, redraw the ICO, the standalone MSIX images and the 300
pixel Store listing icon, extract the frames, and regenerate the installer
bitmaps:

```sh
python3 resources/icons/export_icon_frames.py --render
python3 packaging/wix/generate-branding.py
```

`--render` uses Chrome, Chromium or Edge from `PATH`, or the executable given with
`--chrome`. Windows usually needs `--chrome` with the full path to `chrome.exe` or
`msedge.exe`. Without `--render`, the script only extracts the existing ICO's
frames. `generate-branding.py` needs Pillow 13.0.0. Then upload the new
`packaging/windows/msix/Listing/StoreIcon.png` in Partner Center, because the
Store listing does not take its icon from the package.

Verify that every committed frame copy still matches its ICO frame with:

```sh
python3 resources/icons/export_icon_frames.py --check
```

This script uses only Python's standard library. Extraction and `--check` do not
redraw the SVG or change the ICO.

## Integration

Qt embeds the PNG frames and uses them for window icons and Recent file rows.
Windows embeds the ICO into the executable. File associations, Open with, and
executable shortcuts use its first icon. WiX also uses that ICO for the Start
menu shortcut and Installed apps entry.

`export_icon_frames.py` also copies every ICO frame into
`packaging/windows/msix/Assets` as plain, unplated and light unplated
`Square44x44Logo` target-size images, and `scripts/build-msix.ps1` packages them.
Windows uses them for the packaged app's taskbar, Start menu and file icons, so
they match the executable's icon. See the
[MSIX notes](../../packaging/windows/msix/README.md) for the resource index that
makes Windows find them.

Linux installs the hicolor theme's indexed PNG sizes and the scalable SVG as
`mervin-pdf`. The desktop entry and AppImage keep that stable identifier. Generic
PDF MIME icons remain controlled by the user's desktop theme.

The Lucide files in `lucide/` are separate toolbar and action glyphs. Their
license is recorded in `licenses/lucide-LICENSE.txt` at the repository root.
