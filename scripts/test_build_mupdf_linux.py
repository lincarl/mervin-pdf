#!/usr/bin/env python3
"""Run the MuPDF build script against isolated fixtures without network or C++."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "build-mupdf-linux.sh"
SOURCE_NAME = "mupdf-1.28.6-source"
RELEASE_SHA256 = "dc85000640a45d90ca9b77b061f6ca67cce0c4d4fbecf8819647baf573eef101"
MARKER = Path("build/release/mervin-patch-set.sha256")

# Real tar, mv and make exercise filesystem behavior. Only download/hash and
# patch application use fixtures, with failures injected at individual steps.
TOOL = r'''#!{python}
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys

tool = Path(sys.argv[0]).name
args = sys.argv[1:]
real = json.loads(os.environ["MUPDF_TEST_TOOLS"])
failure = os.environ.get("MUPDF_TEST_FAILURE")
event = tool
if tool == "sha256sum":
    if "--check" not in args:
        os.execv(real[tool], [tool, *args])
    event = "checksum"
elif tool == "mv":
    if "-T" not in args:
        os.execv(real[tool], [tool, *args])
    event = "publish"
with open(os.environ["MUPDF_TEST_EVENTS"], "a") as log:
    log.write(event + "\n")
if tool == "curl":
    output = Path(args[args.index("-o") + 1])
    shutil.copyfile(os.environ["MUPDF_TEST_ARCHIVE"], output)
    if failure == "term":
        os.kill(os.getppid(), signal.SIGTERM)
elif tool == "sha256sum":
    expected, path = sys.stdin.read().strip().split("  ", 1)
    assert expected == os.environ["MUPDF_TEST_RELEASE_SHA256"]
    assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == os.environ["MUPDF_TEST_FIXTURE_SHA256"]
elif tool == "git":
    assert args == ["apply"], args
    assert "diff --git" in sys.stdin.read()
elif tool == "tar":
    if failure == event:
        (Path(args[args.index("-C") + 1]) / "partial-extraction").mkdir()
    else:
        result = subprocess.run([real[tool], *args], check=False)
        if result.returncode:
            sys.exit(result.returncode)
if event == os.environ.get("MUPDF_TEST_RACE_STAGE", "tar"):
    race = os.environ.get("MUPDF_TEST_RACE")
    if race:
        destination = Path(os.environ["MUPDF_TEST_DESTINATION"])
        if race == "symlink":
            destination.symlink_to(os.environ["MUPDF_TEST_RACE_TARGET"])
        else:
            destination.mkdir()
            (destination / "keep.txt").write_text("created during download\n")
if failure == event:
    sys.exit("injected " + event + " failure")
if tool == "mv":
    os.execv(real[tool], [tool, *args])
'''

MAKEFILE = """\
.PHONY: libs
libs:
\t@test "$$MUPDF_TEST_FAILURE" != make
\t@mkdir -p build/release
\t@printf 'fixture archive\\n' > build/release/libmupdf.a
\t@printf 'fixture codecs\\n' > build/release/libmupdf-third.a
\t@printf 'build\\n' >> build-count
"""


def snapshot(path):
    """Record a destination without following symlinks."""
    if path.is_symlink():
        return ("symlink", os.readlink(path))
    if path.is_dir():
        return {item.name: snapshot(item) for item in sorted(path.iterdir())}
    return path.read_bytes()


class DestinationTests(unittest.TestCase):
    """Exercise the real shell entry point with task-local source fixtures."""

    def setUp(self):
        """Create isolated tools, archive and temporary directories per test."""
        temporary_root = ROOT / "dev-temp" / "test-build-mupdf-linux"
        temporary_root.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        real = {name: shutil.which(name) for name in ("sha256sum", "tar", "mv")}
        for name in ("curl", "sha256sum", "git", "tar", "mv"):
            tool = self.tools / name
            tool.write_text(TOOL.format(python=sys.executable))
            tool.chmod(0o755)
        self.events = self.root / "events"
        self.temporary = self.root / "temporary"
        self.temporary.mkdir()
        self.parent = self.root / "destinations with spaces"
        self.parent.mkdir()
        self.destination = self.parent / "custom source"
        self.fixture = self.root / "fixture" / SOURCE_NAME
        self.source_tree(self.fixture)
        self.archive = self.root / "source.tar.gz"
        self.env = {
            **os.environ,
            "PATH": str(self.tools) + os.pathsep + os.environ["PATH"],
            "TMPDIR": str(self.temporary),
            "OMP_NUM_THREADS": "2",
            "MUPDF_TEST_TOOLS": json.dumps(real),
            "MUPDF_TEST_EVENTS": str(self.events),
            "MUPDF_TEST_ARCHIVE": str(self.archive),
            "MUPDF_TEST_RELEASE_SHA256": RELEASE_SHA256,
            "MUPDF_TEST_FAILURE": "",
            "MUPDF_TEST_RACE": "",
        }
        self.write_archive()
        result = self.run_script("--patch-set")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.patch_set = result.stdout.strip()

    def source_tree(self, path):
        """Create headers and a Makefile that writes harmless fixture archives."""
        (path / "include/mupdf").mkdir(parents=True)
        (path / "include/mupdf/fitz.h").write_text("/* fixture header */\n")
        (path / "Makefile").write_text(MAKEFILE)

    def write_archive(self, root_name=SOURCE_NAME):
        """Package the fixture with a real gzip tar writer."""
        with tarfile.open(self.archive, "w:gz") as archive:
            archive.add(self.fixture, arcname=root_name)
        self.env["MUPDF_TEST_FIXTURE_SHA256"] = hashlib.sha256(
            self.archive.read_bytes()).hexdigest()

    def run_script(self, destination=None, **environment):
        """Invoke the repository script unchanged and enforce a short timeout."""
        return subprocess.run(
            ["bash", str(SCRIPT), str(destination or self.destination)],
            cwd=self.root, env={**self.env, **environment},
            text=True, capture_output=True, timeout=15, check=False)

    def assert_clean(self):
        """Check that neither extraction nor patch-backup temporary files remain."""
        self.assertEqual(list(self.temporary.iterdir()), [])
        self.assertEqual(list(self.root.rglob(".mupdf-*")), [])

    def calls(self):
        """Read only the external steps that can download or change files."""
        return self.events.read_text().splitlines() if self.events.exists() else []

    def assert_built(self, result, destination=None):
        """Verify the fixture build and patch marker from a successful run."""
        destination = destination or self.destination
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, str(destination) + "\n")
        self.assertEqual((destination / MARKER).read_text(), self.patch_set + "\n")
        self.assertTrue((destination / "build/release/libmupdf-third.a").is_file())
        self.assert_clean()

    def test_occupied_destinations_are_untouched(self):
        """Refuse files, hidden contents and non-MuPDF Makefiles before download."""
        for kind in ("file", "directory", "hidden", "makefile"):
            with self.subTest(kind=kind):
                destination = self.parent / kind
                if kind == "file":
                    destination.write_text("keep file\n")
                else:
                    destination.mkdir()
                    name = ".hidden" if kind == "hidden" else "keep.txt"
                    (destination / name).write_text("keep contents\n")
                    if kind == "makefile":
                        (destination / "Makefile").write_text(MAKEFILE)
                        (destination / "mervin-patches.new").mkdir()
                        (destination / "mervin-patches.new/keep.txt").write_text("keep\n")
                before = snapshot(destination)
                result = self.run_script(destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Choose another destination", result.stderr)
                self.assertEqual(snapshot(destination), before)
                self.assertEqual(self.calls(), [])
                self.assert_clean()

    def test_unrelated_destination_symlinks_are_untouched(self):
        """Refuse dangling, file, empty-directory and occupied-directory links."""
        for kind in ("missing", "file", "empty", "directory"):
            for suffix in ("", "///"):
                with self.subTest(kind=kind, suffix=suffix):
                    base = self.parent / (kind + ("-slashes" if suffix else ""))
                    base.mkdir()
                    target = base / "target"
                    if kind == "file":
                        target.write_text("keep target\n")
                    elif kind in ("empty", "directory"):
                        target.mkdir()
                        if kind == "directory":
                            (target / "keep.txt").write_text("keep target\n")
                    destination = base / "link"
                    destination.symlink_to(target)
                    before = snapshot(base)
                    result = self.run_script(str(destination) + suffix)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(snapshot(base), before)
                    self.assertEqual(self.calls(), [])
                    self.assert_clean()

    def test_fresh_and_empty_destinations_build(self):
        """Allow a new path and a caller-created empty directory."""
        for kind in ("missing", "empty", "nested", SOURCE_NAME):
            with self.subTest(kind=kind):
                destination = self.parent / kind / "source" if kind == "nested" else self.parent / kind
                if kind == "empty":
                    destination.mkdir()
                self.assert_built(self.run_script(destination), destination)

    def test_relative_path_builds_and_cleans_up(self):
        """Keep relative destination output while cleaning absolute staging paths."""
        relative = self.destination.relative_to(self.root)
        result = self.run_script(relative)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, str(relative) + "\n")
        self.assertTrue((self.destination / MARKER).is_file())
        self.assert_clean()

    def test_versioned_siblings_are_untouched(self):
        """Extract beside an unrelated version-named file, directory or symlink."""
        for kind in ("file", "directory", "symlink"):
            with self.subTest(kind=kind):
                parent = self.parent / kind
                parent.mkdir()
                sibling = parent / SOURCE_NAME
                if kind == "file":
                    sibling.write_text("keep sibling\n")
                elif kind == "directory":
                    sibling.mkdir()
                    (sibling / "Makefile").write_text("keep sibling Makefile\n")
                else:
                    target = parent / "target"
                    target.mkdir()
                    (target / "Makefile").write_text("keep link target\n")
                    sibling.symlink_to(target)
                before = snapshot(parent)
                destination = parent / "new source"
                self.assert_built(self.run_script(destination), destination)
                after = snapshot(parent)
                del after[destination.name]
                self.assertEqual(after, before)

    def test_prepublication_failures_leave_destination_and_sibling_untouched(self):
        """Clean partial downloads and extraction without touching original paths."""
        sibling = self.parent / SOURCE_NAME
        sibling.mkdir()
        (sibling / "Makefile").write_text("keep sibling\n")
        self.destination.mkdir()
        before = snapshot(self.parent)
        for failure in ("curl", "checksum", "tar", "publish", "term"):
            with self.subTest(failure=failure):
                result = self.run_script(MUPDF_TEST_FAILURE=failure)
                self.assertNotEqual(result.returncode, 0)
                if failure == "term":
                    self.assertEqual(result.returncode, 143, result.stderr)
                else:
                    self.assertIn("injected " + failure + " failure", result.stderr)
                self.assertEqual(snapshot(self.parent), before)
                self.assert_clean()

    def test_invalid_archive_is_not_installed(self):
        """Reject missing source roots or incomplete sources inside valid tar files."""
        for invalid in ("root", "makefile", "header"):
            with self.subTest(invalid=invalid):
                if invalid == "makefile":
                    (self.fixture / "Makefile").unlink()
                elif invalid == "header":
                    (self.fixture / "Makefile").write_text(MAKEFILE)
                    (self.fixture / "include/mupdf/fitz.h").unlink()
                self.write_archive("wrong-root" if invalid == "root" else SOURCE_NAME)
                result = self.run_script()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("expected MuPDF source", result.stderr)
                self.assertFalse(self.destination.exists())
                self.assert_clean()

    def test_destination_created_during_download_is_preserved(self):
        """Recheck occupied directories and symlinks before publishing the source."""
        target = self.parent / "target"
        target.mkdir()
        (target / "keep.txt").write_text("keep race target\n")
        before = snapshot(target)
        for stage in ("tar", "publish"):
            for kind in ("directory", "symlink"):
                with self.subTest(stage=stage, kind=kind):
                    destination = self.parent / (stage + "-" + kind)
                    result = self.run_script(
                        destination, MUPDF_TEST_RACE=kind, MUPDF_TEST_RACE_STAGE=stage,
                        MUPDF_TEST_DESTINATION=str(destination),
                        MUPDF_TEST_RACE_TARGET=str(target))
                    self.assertNotEqual(result.returncode, 0)
                    if kind == "directory":
                        self.assertEqual(snapshot(destination), {"keep.txt": b"created during download\n"})
                    else:
                        self.assertTrue(destination.is_symlink())
                        self.assertEqual(os.readlink(destination), str(target))
                    self.assertEqual(snapshot(target), before)
                    self.assert_clean()

    def test_patch_and_build_failures_clean_temporary_files(self):
        """Preserve retryable sources and omit success markers after a failed build."""
        for failure in ("git", "make"):
            with self.subTest(failure=failure):
                destination = self.parent / failure
                result = self.run_script(destination, MUPDF_TEST_FAILURE=failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((destination / "Makefile").is_file())
                self.assertFalse((destination / MARKER).exists())
                self.assert_clean()
                self.assert_built(self.run_script(destination), destination)

    def test_cache_and_rebuild_paths(self):
        """Keep cache hits, stale-marker rebuilds and preexisting-source builds."""
        self.source_tree(self.destination)
        self.assert_built(self.run_script())
        self.assertEqual(self.calls(), ["git"])
        before = snapshot(self.destination)
        self.assert_built(self.run_script())
        self.assertEqual(snapshot(self.destination), before)
        self.assertEqual(self.calls(), ["git"])
        (self.destination / MARKER).write_text("old patch set\n")
        self.assert_built(self.run_script())
        self.assertEqual((self.destination / "build-count").read_text(), "build\nbuild\n")
        self.assertEqual(self.calls(), ["git"])
        # A cache with matching libraries/marker still works without source.
        (self.destination / "Makefile").unlink()
        self.assert_built(self.run_script())
        (self.destination / MARKER).write_text("old patch set\n")
        before = snapshot(self.destination)
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("without their source", result.stderr)
        self.assertEqual(snapshot(self.destination), before)

    def test_existing_source_symlink_rebuilds(self):
        """Preserve a supported link to a valid source tree during a rebuild."""
        target = self.parent / "source target"
        self.source_tree(target)
        self.destination.symlink_to(target)
        self.assert_built(self.run_script())
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual(self.calls(), ["git"])


if __name__ == "__main__":
    unittest.main()
