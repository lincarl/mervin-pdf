#!/usr/bin/env python3
"""Generate the WiX bitmaps from the existing Mervin PDF icon frames."""

from pathlib import Path

from PIL import Image


def main() -> None:
    directory = Path(__file__).resolve().parent
    icons = directory.parents[1] / "resources" / "icons" / "mervin-icon"

    # Keep the stock WixUI dimensions and leave its text areas white.
    assets = (
        ("dialog.bmp", (493, 312), 128, (18, 92)),
        ("banner.bmp", (493, 58), 48, (437, 5)),
    )
    for name, size, icon_size, position in assets:
        with Image.open(icons / f"{icon_size}.png") as source:
            if source.size != (icon_size, icon_size):
                raise ValueError(f"Unexpected dimensions for {source.filename}")
            icon = source.convert("RGBA")
        bitmap = Image.new("RGB", size, "white")
        bitmap.paste(icon, position, icon)
        bitmap.save(directory / name, format="BMP")


if __name__ == "__main__":
    main()
