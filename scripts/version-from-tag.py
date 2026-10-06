#!/usr/bin/env python3
import sys

from release_version import parse_tag


def version_from_tag(tag: str) -> str:
    version = parse_tag(tag)
    if version is None:
        raise ValueError(f"invalid release tag {tag!r}; expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-rcN")
    version.validate()
    return tag[1:]


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} vMAJOR.MINOR.PATCH[-rcN]", file=sys.stderr)
        return 2
    try:
        print(version_from_tag(sys.argv[1]))
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
