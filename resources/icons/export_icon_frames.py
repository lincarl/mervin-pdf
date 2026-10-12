#!/usr/bin/env python3
"""Extract the approved ICO's PNG frames for Qt, Linux packages and the MSIX.

Run with --render to redraw the ICO, the standalone MSIX images and the Store
listing icon from mervin-icon.svg with headless Chrome, Chromium or Edge before
extracting. Run with --check to verify committed exports without changing files.
Only the Python standard library is required.
"""

import argparse
import base64
import html
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


SIZES = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256)
ICON_DIR = Path(__file__).resolve().parent
MSIX_DIR = ICON_DIR.parents[1] / "packaging" / "windows" / "msix"
# MSIX and Store listing images that are not ICO frames, rendered at their
# required sizes. Partner Center needs StoreIcon.png uploaded by hand.
STANDALONE = {
    MSIX_DIR / "Assets" / "Square44x44Logo.png": 44,
    MSIX_DIR / "Assets" / "StoreLogo.png": 50,
    MSIX_DIR / "Assets" / "Square150x150Logo.png": 150,
    MSIX_DIR / "Listing" / "StoreIcon.png": 300,
}
CHROME_NAMES = ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser",
                "chrome", "microsoft-edge", "microsoft-edge-stable", "msedge")


def frame_targets(size):
    """Return every committed copy of one ICO frame.

    The MSIX gets the plain, dark-theme unplated and light-theme unplated
    target-size forms. Without the unplated forms, Windows shrinks the packaged
    app's icon onto a plate in the taskbar and Start menu.
    """
    qt_frame = (ICON_DIR / "mervin-icon.png" if size == 256
                else ICON_DIR / "mervin-icon" / f"{size}.png")
    return (qt_frame, *(MSIX_DIR / "Assets" / f"Square44x44Logo.targetsize-{size}{form}.png"
                        for form in ("", "_altform-unplated", "_altform-lightunplated")))


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
        frames[size] = check_png(data[offset:offset + length], size)
    return frames


def check_png(payload, size):
    """Return a PNG unchanged after checking it is size by size, 8-bit RGBA."""
    if len(payload) < 33 or payload[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
        raise ValueError("Expected PNG-backed ICO frames")
    if struct.unpack_from(">IIBB", payload, 16) != (size, size, 8, 6):
        raise ValueError("Expected matching PNG dimensions and 8-bit RGBA")
    return payload


def ico_bytes(frames):
    """Pack PNG frames into an ICO in ascending size order."""
    directory = b""
    offset = 6 + 16 * len(SIZES)
    for size in SIZES:
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                                 len(frames[size]), offset)
        offset += len(frames[size])
    return (struct.pack("<HHH", 0, 1, len(SIZES)) + directory
            + b"".join(frames[size] for size in SIZES))


def render(chrome, sizes):
    """Draw the SVG directly onto a transparent canvas at each size.

    Each size is rasterized from the vector source, never resampled from
    another raster. Antialiasing can differ slightly between Chrome versions.
    """
    svg = base64.b64encode((ICON_DIR / "mervin-icon.svg").read_bytes()).decode()
    page = f"""<!doctype html><meta charset="utf-8"><pre id="out"></pre><script>
const image = new Image();
image.onload = () => {{
  const frames = {{}};
  for (const size of {json.dumps(sorted(sizes))}) {{
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = size;
    canvas.getContext("2d").drawImage(image, 0, 0, size, size);
    frames[size] = canvas.toDataURL("image/png");
  }}
  document.getElementById("out").textContent = JSON.stringify(frames);
}};
image.src = "data:image/svg+xml;base64,{svg}";
</script>"""
    # A data URL, not a file in /tmp, so sandboxed browsers such as the Chromium
    # snap can load the page.
    url = "data:text/html;base64," + base64.b64encode(page.encode()).decode()
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as profile:
        result = subprocess.run(
            [chrome, "--headless=new", "--disable-gpu", "--no-first-run",
             "--no-default-browser-check", f"--user-data-dir={profile}",
             "--virtual-time-budget=10000", "--dump-dom", url],
            capture_output=True, text=True, timeout=120, check=True,
        )
    match = re.search(r'<pre id="out">(.+?)</pre>', result.stdout, re.S)
    if not match:
        raise SystemExit("Chrome did not render the icon")
    urls = json.loads(html.unescape(match.group(1)))
    return {size: check_png(base64.b64decode(urls[str(size)].split(",", 1)[1]), size)
            for size in sizes}


def find_chrome(requested):
    chrome = requested or next(filter(None, map(shutil.which, CHROME_NAMES)), None)
    if not chrome:
        raise SystemExit("Chrome, Chromium or Edge is required. Pass its path with --chrome.")
    return chrome


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify exports without writing")
    mode.add_argument("--render", action="store_true",
                      help="redraw the ICO and standalone images from the SVG first")
    parser.add_argument("--chrome", help="Chrome, Chromium or Edge executable for --render")
    args = parser.parse_args()
    if args.render:
        rendered = render(find_chrome(args.chrome), set(SIZES) | set(STANDALONE.values()))
        (ICON_DIR / "mervin-icon.ico").write_bytes(ico_bytes(rendered))
        for target, size in STANDALONE.items():
            target.write_bytes(rendered[size])
    frames = png_frames((ICON_DIR / "mervin-icon.ico").read_bytes())
    for size in SIZES:
        for target in frame_targets(size):
            if args.check:
                if not target.is_file() or target.read_bytes() != frames[size]:
                    raise SystemExit(f"Icon export differs from ICO: {target}")
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(frames[size])
    print(f"{'Verified' if args.check else 'Exported'} all {len(SIZES)} icon frames")


if __name__ == "__main__":
    main()
