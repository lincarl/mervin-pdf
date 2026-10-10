#!/usr/bin/env python3
"""Generate an original, redistributable floor plan for application screenshots.

The fictional drawing uses PDF vectors and standard fonts. It has no external
assets, customer data, or third-party document content.
"""

import argparse
import math
from pathlib import Path
import zlib


SCALE = 72 / 25.4 / 50  # Drawing millimetres to PDF points at 1:50.
ORIGIN = (125, 235)
INK = "0.13 0.18 0.22"
MUTED = "0.40 0.46 0.50"
BLUE = "0.13 0.39 0.49"


class Drawing:
    def __init__(self):
        self.commands = []

    def line(self, x1, y1, x2, y2, width=0.7, color=INK):
        self.commands.append(
            f"{color} RG {width} w {x1:.3f} {y1:.3f} m {x2:.3f} {y2:.3f} l S"
        )

    def rectangle(self, x, y, width, height, fill=None, stroke=INK, weight=0.7):
        paint = "B" if fill and stroke else "f" if fill else "S"
        self.commands.append(
            f"{fill or '1 1 1'} rg {stroke or INK} RG {weight} w "
            f"{x:.3f} {y:.3f} {width:.3f} {height:.3f} re {paint}"
        )

    def text(self, x, y, value, size=10, bold=False, color=INK, centered=False, vertical=False):
        value = value.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        if centered:
            x -= len(value) * size * 0.255
        matrix = "0 1 -1 0" if vertical else "1 0 0 1"
        self.commands.append(
            f"BT {color} rg /{'F2' if bold else 'F1'} {size} Tf "
            f"{matrix} {x:.3f} {y:.3f} Tm ({value}) Tj ET"
        )

    def world(self, x, y):
        return ORIGIN[0] + x * SCALE, ORIGIN[1] + y * SCALE

    def wall(self, x1, y1, x2, y2, width=100):
        self.line(*self.world(x1, y1), *self.world(x2, y2), width * SCALE)

    def furniture(self, x, y, width, height, fill="0.96 0.97 0.97"):
        self.rectangle(*self.world(x, y), width * SCALE, height * SCALE, fill, MUTED)

    def door(self, x, y, radius=900, start=0, end=90):
        # Quarter-circle swing, with a leaf at its open position.
        points = []
        for index in range(19):
            angle = math.radians(start + (end - start) * index / 18)
            points.append(self.world(x + radius * math.cos(angle), y + radius * math.sin(angle)))
        for first, second in zip(points, points[1:]):
            self.line(*first, *second, color=MUTED)
        self.line(*self.world(x, y), *points[-1], width=1)

    def room(self, x, y, name, note):
        px, py = self.world(x, y)
        self.text(px, py, name, 12, bold=True, centered=True)
        self.text(px, py - 17, note, 9, color=MUTED, centered=True)

    def horizontal_dimension(self, start, end, y, label, extension_y):
        for x in (start, end):
            self.line(x, extension_y, x, y + 6, 0.4, MUTED)
            self.line(x - 3, y - 3, x + 3, y + 3, 0.8, INK)
        self.line(start, y, end, y, 0.4, MUTED)
        self.text((start + end) / 2, y + 6, label, 9, centered=True)


def make_drawing():
    draw = Drawing()
    draw.text(65, 775, "Birch courtyard studio", 24, bold=True)
    draw.text(65, 752, "Ground floor plan", 13, color=MUTED)
    draw.text(1118, 775, "A-101", 15, bold=True, centered=True)
    draw.line(65, 736, 1125, 736, 0.8)

    # Light room tones keep the drawing legible with PDF Comfort mode enabled.
    draw.furniture(0, 3000, 4800, 4200, "0.94 0.97 0.98")
    draw.furniture(4800, 3000, 7200, 4200, "0.97 0.98 0.99")
    draw.furniture(0, 0, 7200, 3000, "0.99 0.98 0.95")
    draw.furniture(7200, 0, 3000, 3000, "0.95 0.97 0.95")
    draw.furniture(10200, 0, 1800, 3000, "0.97 0.97 0.97")

    # Exterior walls and windows. Openings are part of the geometry.
    for start, end in ((0, 650), (3450, 5750), (10950, 12000)):
        draw.wall(start, 7200, end, 7200, 150)
    for start, end in ((650, 3450), (5750, 10950)):
        draw.wall(start, 7200, end, 7200, 22)
        draw.wall(start, 7270, end, 7270, 15)
    draw.wall(0, 0, 0, 7200, 150)
    draw.wall(12000, 0, 12000, 7200, 150)
    draw.wall(0, 0, 2900, 0, 150)
    draw.wall(4000, 0, 12000, 0, 150)
    draw.door(2900, 0, 1100)

    # Internal partitions, with circulation from the entrance gallery.
    for start, end in ((0, 1300), (2200, 5700), (6600, 12000)):
        draw.wall(start, 3000, end, 3000)
    draw.wall(4800, 3000, 4800, 7200)
    draw.wall(7200, 0, 7200, 1050)
    draw.wall(7200, 1950, 7200, 3000)
    draw.wall(10200, 0, 10200, 1050)
    draw.wall(10200, 1950, 10200, 3000)
    draw.door(1300, 3000)
    draw.door(5700, 3000)
    draw.door(7200, 1050, start=90, end=0)
    draw.door(10200, 1050, start=90, end=0)

    # Meeting table and chairs.
    draw.furniture(1400, 5000, 2000, 950, "1 1 1")
    for x in (1550, 2650):
        for y in (4470, 6080):
            draw.furniture(x, y, 550, 460, "1 1 1")
    for x in (770, 3560):
        draw.furniture(x, 5180, 460, 550, "1 1 1")
    draw.room(3100, 4000, "Meeting room", "Shared workspace")

    # Four workstations, a sofa and storage.
    for x in (5700, 8900):
        draw.furniture(x, 5330, 1800, 780, "1 1 1")
        draw.furniture(x + 150, 5420, 600, 390, "0.87 0.91 0.93")
        draw.furniture(x + 950, 5420, 600, 390, "0.87 0.91 0.93")
        for offset in (250, 1050):
            draw.furniture(x + offset, 4680, 480, 480, "1 1 1")
    draw.furniture(11100, 3500, 500, 950)
    draw.room(9000, 3760, "Design studio", "Four workstations")

    draw.furniture(700, 550, 1450, 550)
    draw.furniture(750, 2200, 2000, 350)
    draw.room(3900, 1550, "Entrance gallery", "Display and informal seating")
    draw.furniture(7500, 2300, 2400, 500)
    draw.furniture(9200, 2450, 500, 230, "1 1 1")
    draw.room(8700, 1550, "Kitchen", "Tea point")
    draw.furniture(10900, 2450, 600, 300, "1 1 1")
    draw.furniture(10700, 650, 650, 750, "1 1 1")
    draw.room(11100, 2200, "WC", "Service")

    left, bottom = draw.world(0, 0)
    right, top = draw.world(12000, 7200)
    split, _ = draw.world(4800, 0)
    draw.horizontal_dimension(left, right, top + 59, "12 000", top + 10)
    draw.horizontal_dimension(left, split, top + 30, "4 800", top + 10)
    draw.horizontal_dimension(split, right, top + 30, "7 200", top + 10)
    for y in (bottom, top):
        draw.line(left - 5, y, left - 47, y, 0.4, MUTED)
        draw.line(left - 43, y - 3, left - 37, y + 3, 0.8)
    draw.line(left - 40, bottom, left - 40, top, 0.4, MUTED)
    draw.text(left - 46, (bottom + top) / 2 - 12, "7 200", 9, vertical=True)

    # Notes, a north arrow and a scale bar are part of the drawing itself.
    draw.line(900, 594, 900, 649, 1.2, BLUE)
    draw.line(900, 649, 892, 631, 1.2, BLUE)
    draw.line(900, 649, 908, 631, 1.2, BLUE)
    draw.text(900, 662, "N", 11, bold=True, centered=True, color=BLUE)
    draw.text(876, 536, "Drawing notes", 12, bold=True)
    for y, note in (
        (512, "Dimensions are in millimetres."),
        (494, "Scale 1:50 at A3."),
        (476, "Fictional demonstration project."),
        (458, "Not for construction."),
    ):
        draw.text(876, y, note, 10, color=MUTED)
    draw.line(876, 428, 1108, 428, 0.4, MUTED)
    draw.text(876, 402, "Plan key", 12, bold=True)
    for y, label, color in (
        (377, "Shared rooms", "0.94 0.97 0.98"),
        (354, "Circulation", "0.99 0.98 0.95"),
        (331, "Support spaces", "0.95 0.97 0.95"),
    ):
        draw.rectangle(876, y - 3, 13, 11, color, MUTED, 0.4)
        draw.text(900, y, label, 10, color=MUTED)

    draw.text(125, 171, "Ground floor", 13, bold=True)
    draw.text(125, 151, "Scale 1:50", 10, color=MUTED)
    for index in range(3):
        draw.rectangle(125 + index * 1000 * SCALE, 127, 1000 * SCALE, 5,
                       INK if index % 2 == 0 else "1 1 1", INK, 0.4)
    for index in range(4):
        draw.text(125 + index * 1000 * SCALE, 112, str(index), 8, centered=True)
    draw.text(135 + 3000 * SCALE, 112, "m", 8)

    draw.line(65, 83, 1125, 83, 0.8)
    draw.text(65, 59, "Original sample drawing for Mervin PDF", 10, bold=True)
    draw.text(65, 42, "Synthetic example. No customer or personal information.", 9, color=MUTED)
    draw.text(836, 59, "Revision 01", 10)
    draw.text(991, 59, "Sheet 1 of 1", 10)
    return "\n".join(draw.commands).encode("ascii")


def document():
    content = zlib.compress(make_drawing())
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1190.551 841.890] "
         "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R "
         "/VP [<< /Type /Viewport /BBox [125 235 805.316 643.189] "
         "/Name (Ground floor plan) /Measure << /Type /Measure /Subtype /RL "
         "/R (1:50) /X [<< /Type /NumberFormat /U (mm) /C 17.638888889 >>] >> >>] >>").encode(),
        f"<< /Length {len(content)} /Filter /FlateDecode >>\nstream\n".encode()
        + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
        b"<< /Title (Birch courtyard studio - Ground floor plan) /Creator (Mervin PDF sample generator) >>",
    ]
    result = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, body in enumerate(objects, 1):
        offsets.append(len(result))
        result += f"{number} 0 obj\n".encode() + body + b"\nendobj\n"
    start = len(result)
    result += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode()
    result += b"".join(f"{offset:010d} 00000 n \n".encode() for offset in offsets[1:])
    result += (
        f"trailer\n<< /Size {len(offsets)} /Root 1 0 R /Info 7 0 R >>\n"
        f"startxref\n{start}\n%%EOF\n"
    ).encode()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="destination PDF path")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(document())
    print(args.output)


if __name__ == "__main__":
    main()
