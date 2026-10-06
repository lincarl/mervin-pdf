#!/usr/bin/env python3
"""Check public release tags and the version ordering used by installers."""

import json
from pathlib import Path
import secrets
import shutil
import string
import subprocess
import sys
import tempfile
import unittest

from release_version import ReleaseVersion, parse_tag


ROOT = Path(__file__).resolve().parents[1]
INVALID_TAGS = (
    "1.64.10",
    "V1.64.10",
    "v1.64",
    "v1.64.10.1",
    "v01.64.10",
    "v1.064.10",
    "v1.64.010",
    "v1.64.10-rc0",
    "v1.64.10-rc01",
    "v1.64.10-RC1",
    "v1.64.10-rc",
    "v1.64.10-rc1extra",
    "v1.64.10+build",
    "v١.64.10",
    "v1.64.10-rc１",
    "v1.64.10\nINJECTED=value",
    "v1.64.10;message(FATAL_ERROR injected)",
    "v1.64.10$(echo injected)",
)
OUT_OF_BOUNDS_TAGS = ("v256.1.1", "v1.256.1", "v1.1.655", "v1.64.10-rc99")


class ReleaseVersionTests(unittest.TestCase):
    def test_public_versions_parse_and_validate(self):
        for tag, expected in (
            ("v0.0.0", ReleaseVersion(0, 0, 0)),
            ("v1.64.10", ReleaseVersion(1, 64, 10)),
            ("v1.64.10-rc1", ReleaseVersion(1, 64, 10, 1)),
            ("v1.64.10-rc98", ReleaseVersion(1, 64, 10, 98)),
            ("v255.255.654", ReleaseVersion(255, 255, 654)),
        ):
            with self.subTest(tag=tag):
                version = parse_tag(tag)
                self.assertEqual(version, expected)
                self.assertEqual(version.base, expected[:3])
                version.validate()
                result = self.run_cli(tag)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, tag[1:] + "\n")

    def test_invalid_syntax_and_installer_bounds_are_rejected(self):
        for tag in INVALID_TAGS:
            with self.subTest(tag=tag):
                self.assertIsNone(parse_tag(tag))
                result = self.run_cli(tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
        for tag in OUT_OF_BOUNDS_TAGS:
            with self.subTest(tag=tag):
                version = parse_tag(tag)
                self.assertIsNotNone(version)
                with self.assertRaises(ValueError):
                    version.validate()
                result = self.run_cli(tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")

    @staticmethod
    def run_cli(tag):
        return subprocess.run(
            [sys.executable, "-B", str(ROOT / "scripts/version-from-tag.py"), tag],
            capture_output=True,
            text=True,
            check=False,
        )


class InstallerVersionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which("cmake")
        if cls.cmake is None:
            raise RuntimeError("CMake is required to verify installer version mapping")
        cls.dpkg = shutil.which("dpkg")
        suffix = "".join(secrets.choice(string.ascii_lowercase) for _ in range(4))
        cls.session_dir = Path.home() / "dev-temp" / f"mervin-release-versions-{suffix}"
        cls.session_dir.mkdir(parents=True)

    def configure(self, version):
        with tempfile.TemporaryDirectory(prefix="cmake-", dir=self.session_dir) as temporary:
            directory = Path(temporary)
            script = directory / "version.cmake"
            manifest = directory / "package-version.json"
            details = directory / "version-details.txt"
            script.write_text(
                f'include([[{(ROOT / "cmake/Version.cmake").as_posix()}]])\n'
                f'configure_file([[{(ROOT / "cmake/package-version.json.in").as_posix()}]] '
                f'[[{manifest.as_posix()}]] @ONLY)\n'
                f'file(WRITE [[{details.as_posix()}]] '
                '"${MERVIN_VERSION_BASE}\\n${MERVIN_VERSION_STAGE}")\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                [self.cmake, f"-DMERVIN_VERSION={version}", "-P", str(script)],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                return result, None, None
            return (
                result,
                json.loads(manifest.read_text(encoding="utf-8")),
                details.read_text(encoding="utf-8").splitlines(),
            )

    def test_public_and_package_versions_remain_distinct(self):
        for public, msi, package, base, stage in (
            ("1.64.10-rc1", "1.64.1001", "1.64.10~rc1", "1.64.10", "1"),
            ("1.64.10", "1.64.1099", "1.64.10", "1.64.10", "99"),
            ("255.255.654-rc98", "255.255.65498", "255.255.654~rc98", "255.255.654", "98"),
            ("255.255.654", "255.255.65499", "255.255.654", "255.255.654", "99"),
        ):
            with self.subTest(version=public):
                result, manifest, details = self.configure(public)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    manifest,
                    {"version": public, "msi_version": msi, "package_version": package},
                )
                self.assertEqual(details, [base, stage])

    def test_cmake_rejects_invalid_versions(self):
        # CMake receives the public version without the tag prefix.
        for tag in (*INVALID_TAGS[2:], *OUT_OF_BOUNDS_TAGS):
            with self.subTest(tag=tag):
                result, manifest, _ = self.configure(tag[1:])
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIsNone(manifest)

    def test_installer_versions_upgrade_from_rc_to_stable(self):
        packages = []
        for public in (
            "1.64.10-rc1",
            "1.64.10-rc2",
            "1.64.10-rc10",
            "1.64.10-rc98",
            "1.64.10",
            "1.64.11-rc1",
        ):
            result, manifest, _ = self.configure(public)
            self.assertEqual(result.returncode, 0, result.stderr)
            packages.append(manifest)
        # Installed numeric releases used the public version directly for MSI.
        previous_msi = (1, 64, 9)
        for package in packages:
            current_msi = tuple(int(part) for part in package["msi_version"].split("."))
            self.assertLess(previous_msi, current_msi)
            previous_msi = current_msi
        if self.dpkg is not None:
            for before, after in zip(packages, packages[1:]):
                result = subprocess.run(
                    [self.dpkg, "--compare-versions", before["package_version"], "lt",
                     after["package_version"]],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
