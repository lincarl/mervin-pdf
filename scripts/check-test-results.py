"""Fail CI if a required CTest target silently skipped its checks."""
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
cases = root.findall(".//testcase")
failed = [
    case.get("name")
    for case in cases
    if case.find("skipped") is not None or case.find("failure") is not None
]
if not cases or failed:
    raise SystemExit(f"Missing, failed, or skipped required tests: {failed}")
