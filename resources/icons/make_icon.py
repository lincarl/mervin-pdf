#!/usr/bin/env python3
"""Generate resources/icons/mervin.ico from the same design as makeAppIcon()
in src/main.cpp: a 135-degree blue gradient rounded square with a white bold
"P". Each size is rendered natively (not downscaled) so it stays crisp at 16 px.

Run from anywhere; writes mervin.ico next to this script.
    python make_icon.py
"""
import io
import os
import struct
from PIL import Image, ImageDraw, ImageFont

# Colours match the QLinearGradient stops in makeAppIcon().
C0 = (0x2a, 0xa7, 0xe0)  # top-left  (lighter)
C1 = (0x0a, 0x4d, 0x8c)  # bottom-right (darker)
SIZES = [16, 24, 32, 48, 64, 128, 256]
FONT_PATH = r"C:\Windows\Fonts\segoeuib.ttf"  # Segoe UI Bold

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render(size):
    # Supersample for clean antialiased edges and text, then downscale.
    ss = 4
    s = size * ss
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # 135-degree linear gradient: project pixel onto the (0,0)->(s,s) line.
    grad = Image.new("RGB", (s, s))
    px = grad.load()
    denom = 2 * (s - 1) if s > 1 else 1
    for y in range(s):
        for x in range(s):
            t = (x + y) / denom
            px[x, y] = lerp(C0, C1, t)

    # Rounded-square mask (radius = size/6, matching makeAppIcon()).
    radius = max(2, size // 6) * ss
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    # White bold "P", pixel size ~= size*0.55, centred.
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT_PATH, max(8, size * 55 // 100) * ss)
    draw.text((s / 2, s / 2), "P", font=font, fill=(255, 255, 255, 255), anchor="mm")

    return img.resize((size, size), Image.LANCZOS)


def write_ico(frames, path):
    """Assemble a multi-image .ico with each frame stored as PNG (supported by
    Windows Vista+). Pillow's ICO encoder only keeps one frame here, so we build
    the ICONDIR ourselves to embed every natively-rendered size."""
    payloads = []
    for f in frames:
        buf = io.BytesIO()
        f.save(buf, format="PNG")
        payloads.append(buf.getvalue())

    count = len(frames)
    header = struct.pack("<HHH", 0, 1, count)  # reserved, type=icon, count
    offset = 6 + 16 * count
    entries, blob = b"", b""
    for f, data in zip(frames, payloads):
        w = 0 if f.width >= 256 else f.width   # 0 means 256 in the ICONDIRENTRY
        h = 0 if f.height >= 256 else f.height
        entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset)
        blob += data
        offset += len(data)
    with open(path, "wb") as fh:
        fh.write(header + entries + blob)


def main():
    frames = [render(sz) for sz in SIZES]
    out = os.path.join(SCRIPT_DIR, "mervin.ico")
    write_ico(frames, out)
    print("wrote", out, "sizes:", SIZES)

    # Also emit a 256x256 PNG for the freedesktop hicolor icon theme (Linux
    # .desktop / .deb / .rpm / AppImage all reference mervin-pdf.png).
    png = os.path.join(SCRIPT_DIR, "mervin-256.png")
    render(256).save(png, format="PNG")
    print("wrote", png)


if __name__ == "__main__":
    main()
