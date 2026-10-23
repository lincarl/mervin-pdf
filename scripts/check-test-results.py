"""Reject failed, skipped or missing required CTest results."""
import argparse
from collections import Counter
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def check(results: Path, expected: Path | None = None) -> None:
    """Validate JUnit outcomes and optional pre-run CTest membership."""
    root = ET.parse(results).getroot()
    if root.tag not in ("testsuite", "testsuites"):
        raise ValueError("Expected a JUnit testsuite or testsuites document")
    cases = root.findall(".//testcase")
    failed = [
        case.get("name")
        for case in cases
        if not case.get("name")
        or any(case.find(tag) is not None for tag in ("skipped", "failure", "error"))
        or case.get("status", "run") not in ("run", "passed")
    ]
    if not cases or failed:
        raise ValueError(f"Missing, failed, or skipped required tests: {failed}")
    for suite in root.iter():
        if suite.tag in ("testsuite", "testsuites"):
            for count in ("failures", "errors", "skipped", "disabled"):
                if int(suite.get(count, "0")) != 0:
                    raise ValueError(f"JUnit reports {count}")
    if expected is not None:
        inventory = json.loads(expected.read_text(encoding="utf-8-sig"))
        names = [test["name"] for test in inventory["tests"]]
        if not names or any(not isinstance(name, str) or not name for name in names):
            raise ValueError("CTest inventory has no valid required tests")
        wanted = Counter(names)
        actual = Counter(case.get("name") for case in cases)
        if actual != wanted:
            raise ValueError(
                f"Required test membership differs. Missing {list((wanted - actual).elements())}; "
                f"unexpected {list((actual - wanted).elements())}")


def main() -> int:
    """Read a CTest JUnit report and fail with an actionable diagnostic."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--expected", type=Path, help="Pre-run ctest --show-only=json-v1 output")
    args = parser.parse_args()
    try:
        check(args.results, args.expected)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        parser.exit(1, f"Invalid required test results: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
