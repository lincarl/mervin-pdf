"""Check the optional Qt builder's destination and archive safety contracts."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
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
        for name in ("dxcompiler.dll", "dxil.dll", "opengl32sw.dll"):
            (self.official / "bin" / name).write_bytes(b"fixture")

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


if __name__ == "__main__":
    unittest.main()
