# Mervin application icon

`mervin-icon.svg` is the editable source. `mervin-icon.ico` holds the approved
raster exports, and `mervin-icon.png` is the 256 pixel preview. The smaller PNGs
in `mervin-icon/` are extracted unchanged from the ICO for Qt and Linux.

This is the selected P06 raised P design. Its artwork matches the original
`mervin-p06-raised-p` assets saved in the design-vault. The name changed when the
design became the application icon.

## Design

The transparent canvas is 256 by 256 units. The page spans x34 to x215 and y9 to
y247, with a fold from (157, 9) to (215, 67). The page gradient runs diagonally
from `#2aa7e0` to `#0a4d8c` in object bounding box coordinates. The fold is
`#91d9f5`, and the letter is white.

The P is a path with an even-odd opening, with no font dependency. Its transform
is `translate(24 24.8) scale(.8)`, placing it from (84, 76.8) to (176.8, 188.8).
The SVG preserves the exact curves. All sizes use the same original gradient.

## Export and verification

The ICO contains 32-bit PNG frames at these sizes:

`16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256`

The approved frames were rendered directly from the SVG onto a transparent
Chromium canvas at each size. No intermediate downscaling, font rendering, or
small-size recoloring was used. The original Chromium version was not recorded,
so a fresh SVG export can differ slightly in antialiasing. Preserve the supplied
ICO when exact pixels matter.

After intentionally replacing the ICO, extract its frames with:

```sh
python3 resources/icons/export_icon_frames.py
```

Verify that every committed PNG still matches its ICO frame with:

```sh
python3 resources/icons/export_icon_frames.py --check
```

This script uses only Python's standard library. It does not redraw the SVG or
change the ICO.

## Integration

Qt embeds the PNG frames and uses them for window icons and Recent file rows.
Windows embeds the ICO into the executable. File associations, Open with, and
executable shortcuts use its first icon. NSIS and WiX also use that ICO for
installer, uninstaller, and installed application entries.

Linux installs the hicolor theme's indexed PNG sizes and the scalable SVG as
`mervin-pdf`. The desktop entry and AppImage keep that stable identifier. Generic
PDF MIME icons remain controlled by the user's desktop theme.

The Lucide files in `lucide/` are separate toolbar and action glyphs. Their
license is recorded in `licenses/lucide-LICENSE.txt` at the repository root.
