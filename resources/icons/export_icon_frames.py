#!/usr/bin/env python3
"""Extract the approved ICO's PNG frames for Qt resources and Linux packages.

Run with --check to verify committed exports without changing files. This keeps
the original rasterization intact and requires only the Python standard library.
"""

import argparse
from pathlib import Path
import struct


SIZES = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256)
ICON_DIR = Path(__file__).resolve().parent


def png_frames(data):
    """Read the square, 32-bit PNG frames from the selected Windows icon."""
    if len(data) < 6 or struct.unpack_from("<HHH", data) != (0, 1, len(SIZES)):
        raise ValueError("Expected an ICO with all 15 approved sizes")
    frames = {}
    directory_end = 6 + 16 * len(SIZES)
    if len(data) < directory_end:
        raise ValueError("Truncated ICO directory")
    for index in range(len(SIZES)):
        width, height, _, _, planes, bits, length, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + index * 16
        )
        size = width or 256
        if size != (height or 256) or size not in SIZES or size in frames:
            raise ValueError("Unexpected or duplicate ICO size")
        if planes != 1 or bits != 32 or offset < directory_end or offset + length > len(data):
            raise ValueError("Invalid ICO frame")
        payload = data[offset:offset + length]
        if len(payload) < 33 or payload[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
            raise ValueError("Expected PNG-backed ICO frames")
        if struct.unpack_from(">IIBB", payload, 16) != (size, size, 8, 6):
            raise ValueError("Expected matching PNG dimensions and 8-bit RGBA")
        frames[size] = payload
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify exports without writing")
    args = parser.parse_args()
    frames = png_frames((ICON_DIR / "mervin-icon.ico").read_bytes())
    for size in SIZES:
        target = (ICON_DIR / "mervin-icon.png" if size == 256
                  else ICON_DIR / "mervin-icon" / f"{size}.png")
        if args.check:
            if not target.is_file() or target.read_bytes() != frames[size]:
                raise SystemExit(f"Icon export differs from ICO: {target}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(frames[size])
    print(f"{'Verified' if args.check else 'Exported'} all {len(SIZES)} icon frames")


if __name__ == "__main__":
    main()
