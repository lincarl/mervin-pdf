#!/usr/bin/env python3
"""Write the Store manifest using the project's numeric installer ordering."""

import argparse
from pathlib import Path

from release_version import parse_tag


ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "packaging/windows/msix/AppxManifest.xml.in"


def package_version(public_version: str) -> str:
    """Keep release candidates ordered while reserving field four for the Store."""
    version = parse_tag("v" + public_version)
    if version is None:
        raise ValueError("Expected MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rcN")
    version.validate()
    if version.major == 0:
        raise ValueError("Microsoft Store requires a nonzero major version")
    build = version.patch * 100 + (version.rc if version.rc is not None else 99)
    return f"{version.major}.{version.minor}.{build}.0"


def manifest(public_version: str) -> str:
    return TEMPLATE.read_text(encoding="utf-8").replace(
        "@MSIX_VERSION@", package_version(public_version)
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        content = manifest(args.version)
    except ValueError as error:
        parser.error(str(error))
    args.output.write_text(content, encoding="utf-8")
    print(f"MSIX package version {package_version(args.version)}")


if __name__ == "__main__":
    main()
