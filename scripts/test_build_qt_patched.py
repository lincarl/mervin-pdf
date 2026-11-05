"""Check the Qt builder's destination and archive safety contracts."""

import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("build_qt_patched", ROOT / "scripts/build-qt-patched.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class QtBuildTests(unittest.TestCase):
    """Reject unsafe or unverified inputs before any compiler or network access."""

    def setUp(self):
        """Use disposable files under this checkout for each test."""
        temporary = ROOT / "dev-temp/test-build-qt"
        temporary.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=temporary)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.official = self.root / "official"
        version = self.official / "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake"
        version.parent.mkdir(parents=True)
        version.write_text('set(PACKAGE_VERSION "6.12.0")\n')
        for name in ("data", "i18n", "uc"):
            (self.official / "lib" / f"libicu{name}.so.73.2").write_bytes(b"fixture")
        (self.official / "bin").mkdir()
        (self.official / "bin/opengl32sw.dll").write_bytes(b"fixture")

    def test_windows_support_uses_native_sdk_redistributables(self):
        """An official Qt SDK without DXC still supplies all required build inputs."""
        kit = self.root / "Windows Kits/10"
        redist = kit / "Redist/D3D/x64"
        redist.mkdir(parents=True)
        for name in ("dxcompiler.dll", "dxil.dll"):
            (redist / name).write_bytes(name.encode())
        destination = self.root / "prepared"
        destination.mkdir()
        with patch.object(BUILDER.sys, "platform", "win32"), \
                patch.dict(os.environ, {"WindowsSdkDir": str(kit)}):
            support = BUILDER.support_files(self.official)
            self.assertEqual(support, [self.official / "bin/opengl32sw.dll",
                                      redist / "dxcompiler.dll", redist / "dxil.dll"])
            # Stop before downloading sources, after the normal preparation path hashes its inputs.
            with patch.object(BUILDER, "fetch", side_effect=RuntimeError("reached source download")):
                with self.assertRaisesRegex(RuntimeError, "reached source download"):
                    BUILDER.prepare(destination, self.official)

    def test_windows_support_requires_a_complete_native_sdk(self):
        """A missing SDK or compiler DLL cannot be replaced with a PATH lookup."""
        with patch.object(BUILDER.sys, "platform", "win32"), patch.dict(os.environ, {}, clear=True):
            for kit in ("", "relative-sdk"):
                os.environ["WindowsSdkDir"] = kit
                with self.assertRaisesRegex(RuntimeError, "WindowsSdkDir"):
                    BUILDER.prepare(self.root / "missing", self.official)
            os.environ["WindowsSdkDir"] = str(self.root / "missing-kit")
            with self.assertRaises(FileNotFoundError):
                BUILDER.prepare(self.root / "missing", self.official)
        self.assertFalse((self.root / "missing").exists())

    def test_preserves_occupied_destination(self):
        """Refuse an existing unrelated directory without fetching anything."""
        destination = self.root / "occupied"
        destination.mkdir()
        sentinel = destination / "keep"
        sentinel.write_bytes(b"do not touch")
        with patch.object(BUILDER, "fetch") as fetch:
            with self.assertRaisesRegex(RuntimeError, "empty destination"):
                BUILDER.prepare(destination, self.official)
            fetch.assert_not_called()
        self.assertEqual(list(destination.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"do not touch")

    def test_preserves_official_sdk(self):
        """Reject a build in, around, or directly over the SDK being copied."""
        for destination in (self.official, self.official / "child", self.root):
            with self.subTest(destination=destination), self.assertRaisesRegex(RuntimeError, "separate directories"):
                BUILDER.prepare(destination, self.official)

    def test_checks_download_and_cache_hashes(self):
        """Accept a correct source and fail closed for a corrupted cached copy."""
        source = self.root / "source"
        source.write_bytes(b"verified source")
        expected = hashlib.sha256(source.read_bytes()).hexdigest()
        destination = self.root / "archive"
        BUILDER.fetch(source.as_uri(), destination, expected)
        self.assertEqual(destination.read_bytes(), source.read_bytes())
        destination.write_bytes(b"modified")
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            BUILDER.fetch(source.as_uri(), destination, expected)
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            BUILDER.fetch(source.as_uri(), self.root / "bad-download", "0" * 64)
        self.assertFalse((self.root / "bad-download").exists())

    def test_missing_security_patches_fail(self):
        """A copied helper without its required patches cannot build plain Qt."""
        with patch.object(BUILDER, "PATCH_DIR", self.root), self.assertRaisesRegex(RuntimeError, "patches are missing"):
            BUILDER.recipe()

    def test_failed_build_step_prints_its_retained_diagnostic(self):
        """CI must expose compiler output even when SDK preparation fails early."""
        diagnostic = self.root / "failure.log"
        errors = io.StringIO()
        with redirect_stderr(errors), self.assertRaises(subprocess.CalledProcessError) as failure:
            BUILDER.run([sys.executable, "-c", "print('compiler diagnostic'); raise SystemExit(7)"], diagnostic)
        self.assertEqual(failure.exception.returncode, 7)
        self.assertIn("compiler diagnostic", diagnostic.read_text())
        self.assertIn("compiler diagnostic", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
