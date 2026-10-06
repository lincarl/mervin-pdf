"""Validate public release versions and their Windows installer limits."""

import re
from typing import NamedTuple


class ReleaseVersion(NamedTuple):
    major: int
    minor: int
    patch: int
    rc: int | None = None

    @property
    def base(self) -> tuple[int, int, int]:
        return self.major, self.minor, self.patch

    def validate(self) -> None:
        # MSI ignores a fourth field. Reserve 100 build values per patch so RCs
        # upgrade in order and the final release upgrades every candidate.
        if self.major > 255 or self.minor > 255 or self.patch > 654:
            raise ValueError("Version exceeds MSI limits of major/minor 255 and patch 654")
        if self.rc is not None and not 1 <= self.rc <= 98:
            raise ValueError("Release candidate number must be between 1 and 98")


def parse_tag(tag: str) -> ReleaseVersion | None:
    match = re.fullmatch(
        r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-rc([1-9][0-9]*))?",
        tag,
    )
    if match is None:
        return None
    major, minor, patch, rc = match.groups()
    return ReleaseVersion(int(major), int(minor), int(patch), int(rc) if rc else None)
