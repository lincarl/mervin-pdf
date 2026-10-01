"""Generate redistributable test inputs from primitives; never uses personal PDFs."""
from pathlib import Path
import sys
import zlib

def pdf(objects):
    result = bytearray(b"%PDF-1.7\n")
    offsets = [0]
    for number, body in enumerate(objects, 1):
        offsets.append(len(result))
        result += f"{number} 0 obj\n".encode() + body + b"\nendobj\n"
    start = len(result)
    result += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode()
    for offset in offsets[1:]:
        result += f"{offset:010d} 00000 n \n".encode()
    result += f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n".encode()
    return result

def document(count=4, drawing=False, mixed=False):
    objects = [b"", b"", b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    pages = []
    for i in range(count):
        page = len(objects) + 1
        pages.append(f"{page} 0 R")
        width, height = (1190, 842) if drawing else (612, 792)
        if mixed and i % 2:
            width, height = height, width
        content = b"BT /F1 24 Tf 36 730 Td (The STANDARD TEST document) Tj ET\n"
        if drawing:
            content = b"2 w\n" + b"".join(
                f"{x} 0 m {x} {height} l S\n".encode() for x in range(10, width, 15)
            ) + b"".join(f"0 {y} m {width} {y} l S\n".encode() for y in range(10, height, 15))
        vp = ""
        if drawing:
            vp = ("/VP [<< /Type /Viewport /BBox [0 0 1190 842] "
                  "/Measure << /Subtype /RL /R (1:1) /X [<< /U (mm) /C 0.3527573 >>] >> >> "
                  "<< /Type /Viewport /BBox [289 86 953 658] "
                  "/Measure << /Subtype /RL /R (1:100) /X [<< /U (mm) /C 35.27573 >>] >> >>]")
        objects += [
            (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {width} {height}] "
             f"/Resources << /Font << /F1 3 0 R >> >> /Contents {page+1} 0 R {vp} >>").encode(),
            f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"endstream"
        ]
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objects[1] = f"<< /Type /Pages /Count {count} /Kids [{' '.join(pages)}] >>".encode()
    return pdf(objects)

def scanned_page():
    width, height = 600, 800
    pixels = bytes(0 if x % 30 < 3 or y % 30 < 3 else 255
                   for y in range(height) for x in range(width))
    image = zlib.compress(pixels)
    content = b"q 600 0 0 800 0 0 cm /Scan Do Q"
    return pdf([
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
        b"/Resources << /XObject << /Scan 5 0 R >> >> /Contents 4 0 R >>",
        f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"\nendstream",
        (f"<< /Type /XObject /Subtype /Image /Width {width} /Height {height} "
         f"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length {len(image)} >>"
         "\nstream\n").encode() + image + b"\nendstream",
    ])

def main(directory):
    directory.mkdir(parents=True, exist_ok=True)
    fixtures = {
        "scan.pdf": scanned_page(),
        "house-drawing.pdf": document(1, drawing=True),
        "drawing.pdf": document(2, drawing=True),
        "properties.pdf": document(),
        "images.pdf": document(12),
        "schematic.pdf": document(20),
        "form_comment.pdf": document(4),
        "example1.pdf": document(8, mixed=True),
        "long.pdf": document(10000, mixed=True),
    }
    for name, data in fixtures.items():
        path = directory / name
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)

if __name__ == "__main__":
    main(Path(sys.argv[1]))
