"""Fetch the pinned SIL OFL fonts used by the translation layout tests."""

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
    (
        "Sans/OTF/TraditionalChinese/NotoSansCJKtc-Regular.otf",
        "NotoSansCJKtc-Regular.otf",
        "dce08bd4fd91aa8aa76ed8fea4b694c2dfb8550f67871e326843212ddbeb88b4",
    ),
    ("LICENSE", "LICENSE", "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2"),
    # Noto Sans covers European scripts, Arabic, Devanagari and Thai.
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans/NotoSans-Regular.ttf",
        "NotoSans-Regular.ttf",
        "b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf",
        "NotoSansArabic-Regular.ttf",
        "ceea25b464a656dc3b26849bab9356740401af62aedf1bfa8b7f0d9b75925b1b",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansArmenian/NotoSansArmenian-Regular.ttf",
        "NotoSansArmenian-Regular.ttf",
        "c3332abfe298018517d7f5b687a9c0f5c92f163ea9258f23934eaa7a9378f40e",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansGeorgian/NotoSansGeorgian-Regular.ttf",
        "NotoSansGeorgian-Regular.ttf",
        "b36ab61cbdd820ffd32f957f704f5bf0c53655e820b7a64001091058919a11a6",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansDevanagari/NotoSansDevanagari-Regular.ttf",
        "NotoSansDevanagari-Regular.ttf",
        "385e78e6359a9d88a0f243d53b1209d7548361ba2194e2b9ec779bcaa7e8949d",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansThai/NotoSansThai-Regular.ttf",
        "NotoSansThai-Regular.ttf",
        "404ddfb5ed0aaa6b6ec8a85700d682978992062d67da93903967b56cbd9a4acc",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/LICENSE",
        "NotoSans-LICENSE.txt",
        "0dab92d0544f7b233403f14b84a663bdbfa746982eda629e7f4f9ffe1b036feb",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf",
        "NotoSansCJKjp-Regular.otf",
        "68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5",
    ),
    (
        "https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/Korean/NotoSansCJKkr-Regular.otf",
        "NotoSansCJKkr-Regular.otf",
        "6bcb2a0703aa137e874fc2dffa85f6c21ba9a67fa329e81b8c801663af7e992a",
    ),
)


def fetch(directory: Path) -> None:
    """Reuse verified files; download and verify missing or changed files."""
    directory.mkdir(parents=True, exist_ok=True)
    for remote, name, expected in FILES:
        target = directory / name
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == expected:
            continue
        url = remote if remote.startswith("https://") else BASE_URL + remote
        with urlopen(url, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Checksum mismatch for {name}")
        target.write_bytes(data)
    print(directory / FILES[0][1])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python scripts/fetch-test-font.py OUTPUT_DIRECTORY")
    fetch(Path(sys.argv[1]))
