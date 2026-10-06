#!/usr/bin/env python3
"""Test prerelease version allocation, tag reuse, and GitHub API failures."""

import importlib.util
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("prepare_prerelease", Path(__file__).with_name("prepare-prerelease.py"))
assert SPEC is not None and SPEC.loader is not None
prerelease = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prerelease)

COMMIT = "a" * 40
TAG_OBJECT = "b" * 40
REPOSITORY = "owner/project"


class PrereleaseTests(unittest.TestCase):
    def test_next_tag_reserves_next_numeric_version(self):
        tags = {tag: ("commit", "") for tag in ("v1.9.20", "v1.64.7", "v1.64.8-pre.1", "unrelated")}
        self.assertEqual(prerelease.next_tag(tags), "v1.64.8-rc1")
        self.assertEqual(prerelease.next_tag({}), "v0.0.1-rc1")

    def test_rc_sequence_retains_base_until_stable(self):
        tags = {tag: ("tag", COMMIT) for tag in ("v1.64.7", "v1.64.8", "v1.64.9")}
        self.assertEqual(prerelease.next_tag(tags), "v1.64.10-rc1")
        tags["v1.64.10-rc1"] = "tag", COMMIT
        tags["v1.64.10-rc9"] = "tag", COMMIT
        self.assertEqual(prerelease.next_tag(tags), "v1.64.10-rc10")
        tags["v1.64.10"] = "tag", COMMIT
        self.assertEqual(prerelease.next_tag(tags), "v1.64.11-rc1")

    def test_explicit_future_rc_target_continues(self):
        tags = {tag: ("tag", COMMIT) for tag in ("v1.64.9", "v2.0.0-rc2")}
        self.assertEqual(prerelease.next_tag(tags), "v2.0.0-rc3")

    def test_installer_version_bounds(self):
        self.assertEqual(prerelease.next_tag({"v255.255.653": ("commit", "")}), "v255.255.654-rc1")
        for tag in ("v1.64.654", "v256.0.0", "v1.256.0"):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "MSI limits"):
                prerelease.next_tag({tag: ("commit", "")})
        with self.assertRaisesRegex(ValueError, "between 1 and 98"):
            prerelease.next_tag({"v1.64.10-rc98": ("tag", COMMIT)})

    def test_reuse_requires_annotation_and_exact_commit(self):
        tags = {
            "v1.2.1-rc1": ("commit", COMMIT),
            "v1.2.1-rc2": ("tag", "c" * 40),
            "v1.2.1-rc3": ("tag", COMMIT),
        }
        with patch.object(prerelease, "command", return_value="Prerelease for CI run 42\n\n"):
            self.assertEqual(prerelease.existing_tag(tags, COMMIT, "Prerelease for CI run 42"), "v1.2.1-rc3")
        for message in ("Prerelease for CI run 43\n", "Prerelease for CI run 42\nExtra body\n", "Prerelease for CI run 42 \n"):
            with self.subTest(message=message), patch.object(prerelease, "command", return_value=message):
                self.assertIsNone(prerelease.existing_tag(tags, COMMIT, "Prerelease for CI run 42"))

    def test_new_tag_creates_annotation_before_ref(self):
        with patch.object(prerelease, "fetch_tags", return_value={}), patch.object(prerelease, "api", side_effect=[{"sha": TAG_OBJECT}, {}]) as api:
            self.assertEqual(prerelease.prepare_tag(REPOSITORY, COMMIT, "42"), "v0.0.1-rc1")
        self.assertEqual(api.call_args_list[0].args, (REPOSITORY, "tags"))
        self.assertEqual(api.call_args_list[0].kwargs, {"tag": "v0.0.1-rc1", "message": "Prerelease for CI run 42", "object": COMMIT, "type": "commit"})
        self.assertEqual(api.call_args_list[1].kwargs, {"ref": "refs/tags/v0.0.1-rc1", "sha": TAG_OBJECT})

    def test_retry_recalculates_after_concurrent_collision(self):
        collision = subprocess.CalledProcessError(1, "gh", stderr="Reference already exists")
        tags = [{"v1.2.3": ("commit", "")}, {"v1.2.3": ("commit", ""), "v1.2.4-rc1": ("tag", "c" * 40)}]
        responses = [{"sha": TAG_OBJECT}, collision, {}, {"sha": TAG_OBJECT}, {}]
        with patch.object(prerelease, "fetch_tags", side_effect=tags) as fetch, patch.object(prerelease, "api", side_effect=responses) as api:
            self.assertEqual(prerelease.prepare_tag(REPOSITORY, COMMIT, "42"), "v1.2.4-rc2")
        self.assertEqual(fetch.call_count, 2)
        self.assertEqual(api.call_args_list[3].kwargs["tag"], "v1.2.4-rc2")

    def test_retry_reuses_tag_after_lost_creation_response(self):
        error = subprocess.CalledProcessError(1, "gh", stderr="connection reset")
        with patch.object(prerelease, "fetch_tags", side_effect=[{}, {"v0.0.1-rc1": ("tag", COMMIT)}]), patch.object(prerelease, "command", return_value="Prerelease for CI run 42\n"), patch.object(prerelease, "api", side_effect=[{"sha": TAG_OBJECT}, error, {}]) as api:
            self.assertEqual(prerelease.prepare_tag(REPOSITORY, COMMIT, "42"), "v0.0.1-rc1")
        self.assertEqual(api.call_count, 3)

    def test_existing_run_tag_skips_api_mutations(self):
        with patch.object(prerelease, "fetch_tags", return_value={"v1.2.1-rc3": ("tag", COMMIT)}), patch.object(prerelease, "command", return_value="Prerelease for CI run 42\n"), patch.object(prerelease, "api") as api:
            self.assertEqual(prerelease.prepare_tag(REPOSITORY, COMMIT, "42"), "v1.2.1-rc3")
        api.assert_not_called()

    def test_permission_failure_is_not_retried(self):
        error = subprocess.CalledProcessError(1, "gh", stderr="Resource not accessible by integration")
        missing = subprocess.CalledProcessError(1, "gh", stderr="Not Found")
        with patch.object(prerelease, "fetch_tags", return_value={}) as fetch, patch.object(prerelease, "api", side_effect=[{"sha": TAG_OBJECT}, error, missing]), self.assertRaises(subprocess.CalledProcessError) as raised:
            prerelease.prepare_tag(REPOSITORY, COMMIT, "42")
        self.assertIs(raised.exception, error)
        self.assertEqual(fetch.call_count, 1)

    def test_collisions_stop_after_five_attempts(self):
        error = subprocess.CalledProcessError(1, "gh", stderr="Reference already exists")
        responses = [{"sha": TAG_OBJECT}, error, {}] * 5
        with patch.object(prerelease, "fetch_tags", return_value={}) as fetch, patch.object(prerelease, "api", side_effect=responses), self.assertRaisesRegex(RuntimeError, "five concurrent collisions"):
            prerelease.prepare_tag(REPOSITORY, COMMIT, "42")
        self.assertEqual(fetch.call_count, 5)

    def test_invalid_annotation_response_stops_before_ref(self):
        with patch.object(prerelease, "fetch_tags", return_value={}), patch.object(prerelease, "api", return_value={"sha": "invalid"}) as api, self.assertRaisesRegex(ValueError, "annotated tag SHA"):
            prerelease.prepare_tag(REPOSITORY, COMMIT, "42")
        self.assertEqual(api.call_count, 1)


if __name__ == "__main__":
    unittest.main()
