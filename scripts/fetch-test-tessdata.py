"""Fetch a pinned English best model for OCR tests, never for packaging."""

import hashlib
from pathlib import Path
import sys
from urllib.request import urlopen


REVISION = "e12c65a915945e4c28e237a9b52bc4a8f39a0cec"
URL = f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_best/{REVISION}/eng.traineddata"
SHA256 = "8280aed0782fe27257a68ea10fe7ef324ca0f8d85bd2fd145d1c2b560bcb66ba"


def fetch(directory: Path) -> None:
    """Reuse a verified model or download and verify its replacement."""
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / "eng.traineddata"
    if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != SHA256:
        with urlopen(URL, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise ValueError("Checksum mismatch for eng.traineddata")
        target.write_bytes(data)
    print(target)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python scripts/fetch-test-tessdata.py OUTPUT_DIRECTORY")
    fetch(Path(sys.argv[1]))
