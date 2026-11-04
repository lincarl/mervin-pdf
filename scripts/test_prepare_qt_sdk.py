#!/usr/bin/env python3
"""Exercise cache acceptance, rejection and selection without source compilation."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_qt_sdk", ROOT / "scripts/prepare-qt-sdk.py")
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)


class SDKCacheTests(unittest.TestCase):
    """Invalid cached data must never reach a compiler or runtime check."""

    def setUp(self):
        """Create a minimal, fully inventoried SDK with its original provenance."""
        temporary = ROOT / "dev-temp/test-qt-cache"
        temporary.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=temporary)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.destination = self.root / "patched"
        self.sdk = self.destination / "sdk"
        self.sdk.mkdir(parents=True)
        self.official = self.root / "official"
        self.official.mkdir()
        self.expected = {"recipe": SDK.BUILDER.recipe(), "toolchain": {"compiler": "fixture"},
                         "support_files": {}, "selection_helper": "fixture"}
        self.file = self.sdk / "bin/Qt6Core.dll"
        self.file.parent.mkdir()
        self.file.write_bytes(b"a library")
        self.write_markers()

    def write_markers(self):
        """Record fixture provenance after constructing a test inventory."""
        inputs = self.expected["recipe"] | {"platform": sys.platform, "support_files": {},
                                            "official_sdk": "/original/build/support"}
        (self.sdk / SDK.BUILD_MARKER).write_text(json.dumps({"inputs": inputs, "files": SDK.inventory(self.sdk)}))
        (self.sdk / SDK.CACHE_MARKER).write_text(json.dumps(self.expected))

    def prepare(self):
        """Use the normal cache path while replacing expensive child processes."""
        return SDK.prepare(self.destination, self.official, self.expected, 2)

    def test_valid_hit_checks_api_without_building_or_rewriting_provenance(self):
        """A valid hit skips source compilation but still runs the current probes."""
        original = (self.sdk / SDK.BUILD_MARKER).read_bytes()
        with patch.object(SDK.subprocess, "run") as build, patch.object(SDK, "check_api") as check:
            self.assertEqual(self.prepare(), self.sdk)
            build.assert_not_called()
            check.assert_called_once_with(self.sdk, self.official, self.destination / "validation", 2)
        self.assertEqual((self.sdk / SDK.BUILD_MARKER).read_bytes(), original)

    def test_corrupted_missing_and_extra_files_fail_before_execution(self):
        """Changes to installed files stop the workflow before cached code runs."""
        for corruption in ("changed", "missing", "extra"):
            with self.subTest(corruption=corruption):
                self.file.write_bytes(b"changed" if corruption == "changed" else b"a library")
                extra = self.sdk / "injected.dll"
                if corruption == "missing":
                    self.file.unlink()
                if corruption == "extra":
                    extra.write_bytes(b"unrecorded")
                with patch.object(SDK, "check_api") as check, patch.object(SDK.subprocess, "run") as build:
                    with self.assertRaisesRegex(RuntimeError, "inventory"):
                        self.prepare()
                    check.assert_not_called()
                    build.assert_not_called()
                self.file.write_bytes(b"a library")
                extra.unlink(missing_ok=True)

    def test_recipe_and_toolchain_mismatches_fail_closed(self):
        """Reject every class of immutable cache identity mismatch."""
        for component in ("recipe", "toolchain", "support_files", "selection_helper"):
            with self.subTest(component=component), patch.object(SDK, "check_api") as check:
                expected = copy.deepcopy(self.expected)
                expected[component] = {"changed": True}
                with self.assertRaises(RuntimeError):
                    SDK.prepare(self.destination, self.official, expected, 2)
                check.assert_not_called()

    def test_incomplete_hit_never_falls_back_to_a_build(self):
        """A partially written cache cannot trigger an implicit rebuild."""
        (self.sdk / SDK.CACHE_MARKER).unlink()
        with patch.object(SDK.subprocess, "run") as build, self.assertRaises(FileNotFoundError):
            self.prepare()
        build.assert_not_called()

    def test_cache_miss_builds_then_checks_before_marking_success(self):
        """Only a successful build and verification may produce a cache marker."""
        build_manifest = (self.sdk / SDK.BUILD_MARKER).read_text()
        shutil.rmtree(self.destination)

        def build(command, **kwargs):
            """Stand in for the builder's freshly installed, inventoried SDK."""
            self.assertIn(str(ROOT / "scripts/build-qt-patched.py"), command)
            self.assertIn(str(self.official), command)
            self.file.parent.mkdir(parents=True)
            self.file.write_bytes(b"a library")
            (self.sdk / SDK.BUILD_MARKER).write_text(build_manifest)
            self.assertEqual(kwargs["env"]["CMAKE_PREFIX_PATH"], str(self.sdk))

        with patch.object(SDK.subprocess, "run", side_effect=build), patch.object(SDK, "check_api") as check:
            self.prepare()
            check.assert_called_once()
        self.assertEqual(json.loads((self.sdk / SDK.CACHE_MARKER).read_text()), self.expected)

    def test_failed_api_check_does_not_mark_a_cache(self):
        """Runtime regression failures leave incomplete caches unusable."""
        (self.sdk / SDK.CACHE_MARKER).unlink()
        with patch.object(SDK, "validate"), patch.object(SDK, "check_api", side_effect=RuntimeError("regression")):
            with self.assertRaisesRegex(RuntimeError, "regression"):
                self.prepare()
        self.assertFalse((self.sdk / SDK.CACHE_MARKER).exists())

    @unittest.skipIf(sys.platform == "win32", "Symlink creation requires Windows privileges")
    def test_safe_internal_links_work_and_external_links_fail(self):
        """Accept normal library aliases and reject links outside the SDK."""
        link = self.sdk / "bin/alias"
        link.symlink_to(self.file.name)
        self.write_markers()
        SDK.validate(self.sdk, self.expected, restored=True)
        link.unlink()
        link.symlink_to(self.official)
        with self.assertRaisesRegex(RuntimeError, "Unsafe SDK symlink"):
            SDK.validate(self.sdk, self.expected, restored=True)

    def test_support_binaries_must_match_the_official_source(self):
        """Manifest claims cannot substitute for hashing the support binaries."""
        self.expected["support_files"] = {"libicuuc.so.73.2": "0" * 64}
        built = json.loads((self.sdk / SDK.BUILD_MARKER).read_text())
        built["inputs"]["support_files"] = self.expected["support_files"]
        (self.sdk / SDK.BUILD_MARKER).write_text(json.dumps(built))
        with self.assertRaisesRegex(RuntimeError, "support file does not match"):
            SDK.validate(self.sdk, self.expected, restored=False)

    def test_environment_and_export_select_only_the_patched_sdk(self):
        """Build tools and later workflow steps must receive the selected SDK."""
        env_file, path_file = self.root / "env", self.root / "path"
        with patch.dict(os.environ, {"PATH": os.pathsep.join([str(self.official / "bin"), "/usr/bin"]),
                                     "CMAKE_PREFIX_PATH": str(self.official),
                                     "GITHUB_ENV": str(env_file), "GITHUB_PATH": str(path_file)}):
            environment = SDK.sdk_environment(self.sdk, self.official)
            self.assertNotIn(str(self.official), environment["PATH"])
            self.assertEqual(environment["CMAKE_PREFIX_PATH"], str(self.sdk))
            SDK.select(self.sdk, self.official)
        self.assertIn(f"QT_ROOT_DIR={self.sdk}\n", env_file.read_text())
        self.assertIn(f"QT6_DIR={self.sdk}\n", env_file.read_text())
        self.assertEqual(path_file.read_text(), str(self.sdk / "bin") + "\n")

    def test_windows_context_requires_the_native_x64_msvc_environment(self):
        """A Windows cache cannot be keyed from an absent or different toolchain."""
        with patch.object(SDK.sys, "platform", "win32"), patch.object(SDK, "output", return_value="version"), \
                patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "MSVC x64"):
                SDK.toolchain()
            os.environ["VSCMD_ARG_TGT_ARCH"] = "x64"
            with patch.object(SDK.shutil, "which", return_value=str(self.file)):
                with self.assertRaisesRegex(RuntimeError, "versions must be available"):
                    SDK.toolchain()
                os.environ.update({"VCToolsVersion": "14.44", "WindowsSDKVersion": "10.0.26100.0"})
                context = SDK.toolchain()
                self.assertEqual(context["compiler"], SDK.BUILDER.digest(self.file))
                self.assertEqual(context["WindowsSDKVersion"], "10.0.26100.0")

    def test_windows_identity_tracks_sdk_graphics_files(self):
        """Cache identity includes DXC from Windows Kits, while Qt supplies OpenGL."""
        version = self.official / "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake"
        version.parent.mkdir(parents=True)
        version.write_text('set(PACKAGE_VERSION "6.12.0")\n')
        opengl = self.official / "bin/opengl32sw.dll"
        opengl.parent.mkdir()
        opengl.write_bytes(b"official software OpenGL")
        kit = self.root / "Windows Kits/10"
        redist = kit / "Redist/D3D/x64"
        redist.mkdir(parents=True)
        for name in ("dxcompiler.dll", "dxil.dll"):
            (redist / name).write_bytes(name.encode())
        with patch.object(SDK.sys, "platform", "win32"), patch.object(SDK, "toolchain", return_value={}), \
                patch.dict(os.environ, {"WindowsSdkDir": str(kit)}):
            before = SDK.identity(self.official)
            self.assertEqual(before["support_files"], {
                path.name: SDK.BUILDER.digest(path)
                for path in (opengl, redist / "dxcompiler.dll", redist / "dxil.dll")})
            (redist / "dxil.dll").write_bytes(b"changed SDK redistributable")
            self.assertNotEqual(before, SDK.identity(self.official))
            (redist / "dxcompiler.dll").unlink()
            with self.assertRaises(FileNotFoundError):
                SDK.identity(self.official)


if __name__ == "__main__":
    unittest.main()
