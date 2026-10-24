#!/usr/bin/env python3
"""Check a bundled install's library resolution and launch it in an isolated profile.

Pass the staged prefix containing bin/MervinPDF and lib/mervin-pdf, such as
AppDir/usr. The check clears SDK search paths so the build machine cannot hide
missing bundled libraries or plugins.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def check(prefix: Path) -> None:
    """Require private application libraries and successful offscreen startup."""
    prefix = prefix.resolve(strict=True)
    runtime = prefix / "lib/mervin-pdf"
    binary = runtime / "bin/MervinPDF"
    launcher = prefix / "bin/MervinPDF"
    environment = dict(os.environ)
    for variable in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH",
                     "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME"):
        environment.pop(variable, None)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    dependencies = subprocess.check_output(["ldd", str(binary)], env=environment, text=True)
    if "not found" in dependencies:
        raise RuntimeError(f"Unresolved bundled runtime dependencies\n{dependencies}")
    required = {"libQt6Core", "libQt6Gui", "libQt6Widgets", "libQt6Network",
                "libQt6PrintSupport", "libQt6Svg", "libqpdf", "libtomlplusplus",
                "libjpeg", "libz"}
    found = set()
    for name, resolved in re.findall(r"^\s*(\S+) => (\S+)", dependencies, re.MULTILINE):
        stem = name.split(".so", 1)[0]
        if stem in required or stem.startswith("libQt6"):
            path = Path(resolved).resolve(strict=True)
            if not path.is_relative_to(runtime):
                raise RuntimeError(f"{name} resolves outside the private runtime to {path}")
            found.add(stem)
    if missing := required - found:
        raise RuntimeError(f"Missing bundled application dependencies {sorted(missing)}")
    print(dependencies, end="", flush=True)
    scratch = Path(__file__).resolve().parents[1] / "dev-temp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="linux-runtime-profile-", dir=scratch) as temporary:
        result = subprocess.run(
            [str(launcher), "--profile", temporary, "--quit-after-startup"],
            env=environment, check=True, timeout=60, capture_output=True, text=True,
        )
        print(result.stdout, end="")
        print(result.stderr, end="")
    print(f"Bundled runtime startup passed for {prefix}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    check(parser.parse_args().prefix)
