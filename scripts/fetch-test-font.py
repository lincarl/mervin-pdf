"""Fetch the pinned SIL OFL font used by the translation layout tests."""

import hashlib
from pathlib import Path
import sys
from urllib.request import urlopen


REVISION = "523d033d6cb47f4a80c58a35753646f5c3608a78"  # Noto CJK Sans 2.004
BASE_URL = f"https://raw.githubusercontent.com/notofonts/noto-cjk/{REVISION}/"
FILES = (
    (
        "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf",
        "NotoSansCJKsc-Regular.otf",
        "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b",
    ),
    ("LICENSE", "LICENSE", "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2"),
)


def fetch(directory: Path) -> None:
    """Reuse verified files; download and verify missing or changed files."""
    directory.mkdir(parents=True, exist_ok=True)
    for remote, name, expected in FILES:
        target = directory / name
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
            continue
        with urlopen(BASE_URL + remote, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Checksum mismatch for {name}")
        target.write_bytes(data)
    print(directory / FILES[0][1])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python scripts/fetch-test-font.py OUTPUT_DIRECTORY")
    fetch(Path(sys.argv[1]))
