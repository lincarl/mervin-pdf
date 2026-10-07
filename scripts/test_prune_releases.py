#!/usr/bin/env python3
"""Test release retention and race checks without changing GitHub releases."""

import contextlib
import io
import importlib.util
import json
import subprocess
import unittest
from unittest.mock import patch
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("prune_releases", Path(__file__).with_name("prune-releases.py"))
assert SPEC is not None and SPEC.loader is not None
prune = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prune)


REPOSITORY = "owner/project"
TRIGGER = "v1.64.15"


def asset_names(tag):
    version = tag[1:]
    return {
        f"MervinPDF-{version}.msi",
        f"MervinPDF-{version}-x86_64.AppImage",
        f"mervin-pdf_{version}_ubuntu26.04_amd64.deb",
        f"mervin-pdf-{version}.x86_64.rpm",
        "SHA256SUMS",
    }


def release(identifier, tag, **changes):
    raw = {
        "id": identifier,
        "tag_name": tag,
        "draft": False,
        "prerelease": "-rc" in tag,
        "published_at": "2026-10-07T10:00:00Z",
        "assets": [
            {"name": name, "state": "uploaded", "size": 100}
            for name in sorted(asset_names(tag))
        ],
    }
    raw.update(changes)
    return raw


def parsed(releases):
    return [prune.parse_release(raw) for raw in releases]


def tags(releases):
    return {item.tag for item in releases}


def stable_history():
    return [release(index, f"v1.64.{version}") for index, version in enumerate((12, 13, 14, 15), 1)]


def is_delete(args):
    return "DELETE" in args


class PlanTests(unittest.TestCase):
    def test_retains_three_newest_stables_by_numeric_version(self):
        history = [
            release(1, "v1.9.9", published_at="2030-01-01T00:00:00Z"),
            release(2, "v1.64.10"),
            release(3, TRIGGER),
            release(4, "v1.10.0"),
            release(5, "v1.64.9", published_at="2020-01-01T00:00:00Z"),
        ]
        plan = prune.build_plan(parsed(history), TRIGGER)
        self.assertEqual(tags(plan.keep), {TRIGGER, "v1.64.10", "v1.64.9"})
        self.assertEqual(tags(plan.delete), {"v1.10.0", "v1.9.9"})
        self.assertIsNone(plan.skip_reason)

    def test_fewer_than_three_stables_keeps_each_and_removes_superseded_rc(self):
        history = [release(1, "v1.64.14"), release(2, TRIGGER), release(3, TRIGGER + "-rc1")]
        plan = prune.build_plan(parsed(history), TRIGGER)
        self.assertEqual(tags(plan.keep), {"v1.64.14", TRIGGER})
        self.assertEqual(tags(plan.delete), {TRIGGER + "-rc1"})

    def test_preserves_future_candidates_drafts_unknown_and_inconsistent_tags(self):
        preserved = [
            release(8, "v1.64.16-rc1"),
            release(9, "v2.0.0-rc1"),
            release(10, "v1.0.0", draft=True),
            release(11, "v1.64.14-rc2", draft=True),
            release(12, "nightly", prerelease=True),
            release(13, "v1.64.14-rc3", prerelease=False),
            release(14, "v1.64.11", published_at=None),
        ]
        history = stable_history() + preserved
        history += [release(20, "v1.64.14-rc1"), release(21, TRIGGER + "-rc2")]
        plan = prune.build_plan(parsed(history), TRIGGER)
        self.assertEqual(tags(plan.delete), {"v1.64.12", "v1.64.14-rc1", TRIGGER + "-rc2"})
        self.assertTrue(tags(parsed(preserved)).isdisjoint(tags(plan.delete)))

    def test_legacy_numeric_prereleases_do_not_count_as_stables(self):
        history = stable_history() + [
            release(8, "v1.64.8", prerelease=True),
            release(9, "v1.64.9", prerelease=True),
        ]
        plan = prune.build_plan(parsed(history), TRIGGER)
        self.assertEqual(tags(plan.keep), {"v1.64.13", "v1.64.14", TRIGGER})
        self.assertEqual(tags(plan.delete), {"v1.64.8", "v1.64.9", "v1.64.12"})

    def test_older_trigger_skips_when_newer_stable_exists(self):
        history = stable_history() + [release(5, "v1.64.16")]
        plan = prune.build_plan(parsed(history), TRIGGER)
        self.assertTrue(plan.skip_reason)
        self.assertEqual(plan.delete, [])

    def test_trigger_must_be_a_published_stable(self):
        cases = (
            ([], TRIGGER),
            ([release(1, TRIGGER, draft=True)], TRIGGER),
            ([release(1, TRIGGER, prerelease=True)], TRIGGER),
            ([release(1, TRIGGER, published_at=None)], TRIGGER),
            ([release(1, TRIGGER + "-rc1")], TRIGGER + "-rc1"),
            ([release(1, "nightly")], "nightly"),
        )
        for history, trigger in cases:
            with self.subTest(trigger=trigger, history=history), self.assertRaises(ValueError):
                prune.build_plan(parsed(history), trigger)

    def test_trigger_requires_all_packages_uploaded_and_nonempty(self):
        for missing in sorted(asset_names(TRIGGER)):
            for state in ("missing", "new", "empty"):
                raw = release(1, TRIGGER)
                for asset in raw["assets"]:
                    if asset["name"] == missing:
                        if state == "new":
                            asset["state"] = "new"
                        elif state == "empty":
                            asset["size"] = 0
                if state == "missing":
                    raw["assets"] = [asset for asset in raw["assets"] if asset["name"] != missing]
                with self.subTest(asset=missing, state=state), self.assertRaises(ValueError):
                    prune.build_plan(parsed([raw]), TRIGGER)

    def test_extra_trigger_assets_do_not_prevent_cleanup(self):
        history = stable_history()
        history[-1]["assets"].append({"name": "diagnostics.zip", "state": "uploaded", "size": 123})
        self.assertEqual(tags(prune.build_plan(parsed(history), TRIGGER).delete), {"v1.64.12"})
        self.assertEqual(prune.expected_assets(TRIGGER), asset_names(TRIGGER))


class ApiTests(unittest.TestCase):
    def setUp(self):
        # Keep expected plan output out of the unittest report.
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def test_list_reads_every_paginated_page(self):
        history = stable_history()
        pages = [history[:2], history[2:], [release(10, "v1.64.14-rc1")]]
        with patch.object(prune, "command", return_value=json.dumps(pages)) as command:
            result = prune.list_releases(REPOSITORY)
        self.assertEqual([item.id for item in result], [1, 2, 3, 4, 10])
        args = command.call_args.args
        self.assertEqual(args[:2], ("gh", "api"))
        self.assertIn("--paginate", args)
        self.assertIn("--slurp", args)
        self.assertTrue(any(f"repos/{REPOSITORY}/releases" in arg for arg in args))

    def test_duplicate_release_across_pages_aborts_for_a_fresh_snapshot(self):
        raw = release(1, TRIGGER)
        with patch.object(prune, "command", return_value=json.dumps([[raw], [raw]])):
            with self.assertRaises(ValueError):
                prune.list_releases(REPOSITORY)

    def test_invalid_response_aborts_instead_of_treating_it_as_empty(self):
        for response in ({"message": "API error"}, [None], [[{"id": 1}]]):
            with self.subTest(response=response), patch.object(prune, "command", return_value=json.dumps(response)):
                with self.assertRaises((ValueError, TypeError)):
                    prune.list_releases(REPOSITORY)

    def test_dry_run_never_fetches_or_deletes_candidates(self):
        with patch.object(prune, "list_releases", return_value=parsed(stable_history())), patch.object(prune, "command") as command:
            prune.prune_releases(REPOSITORY, TRIGGER)
        command.assert_not_called()

    def test_apply_deletes_release_by_id_without_deleting_git_tags(self):
        history = stable_history()
        with patch.object(prune, "list_releases", return_value=parsed(history)) as listing, patch.object(prune, "command", side_effect=[json.dumps(history[0]), ""]) as command:
            prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        self.assertGreaterEqual(listing.call_count, 2)
        self.assertEqual(command.call_count, 2)
        read, delete = [call.args for call in command.call_args_list]
        self.assertFalse(is_delete(read))
        self.assertTrue(is_delete(delete))
        self.assertIn(f"repos/{REPOSITORY}/releases/1", read)
        self.assertIn(f"repos/{REPOSITORY}/releases/1", delete)
        self.assertFalse(any("git/refs" in arg or "--cleanup-tag" == arg for arg in delete))

    def test_each_deletion_uses_a_fresh_inventory(self):
        remaining = stable_history() + [release(5, "v1.64.11")]

        def request(*args):
            identifier = int(args[-1].rsplit("/", 1)[1])
            raw = next(item for item in remaining if item["id"] == identifier)
            if is_delete(args):
                remaining.remove(raw)
                return ""
            return json.dumps(raw)

        with patch.object(prune, "list_releases", side_effect=lambda _: parsed(remaining)) as listing:
            with patch.object(prune, "command", side_effect=request) as command:
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        self.assertEqual(listing.call_count, 3)
        self.assertEqual(command.call_count, 4)
        self.assertEqual(tags(parsed(remaining)), {"v1.64.13", "v1.64.14", TRIGGER})

    def test_cli_defaults_to_dry_run_and_requires_explicit_apply(self):
        for arguments, apply in (([], False), (["--apply"], True)):
            argv = ["prune-releases.py", "--released-tag", TRIGGER, *arguments]
            with self.subTest(apply=apply), patch.object(prune.sys, "argv", argv):
                with patch.dict(prune.os.environ, {"GITHUB_REPOSITORY": REPOSITORY}):
                    with patch.object(prune, "prune_releases") as run:
                        self.assertEqual(prune.main(), 0)
            run.assert_called_once_with(REPOSITORY, TRIGGER, apply)

    def test_replans_when_candidate_becomes_kept_or_changes_classification(self):
        original = stable_history()
        for changes in ({"draft": True}, {"prerelease": True}, {"published_at": None}):
            changed = [dict(raw) for raw in original]
            changed[0].update(changes)
            with self.subTest(changes=changes), patch.object(prune, "list_releases", side_effect=[parsed(original), parsed(changed)]), patch.object(prune, "command", return_value=json.dumps(changed[0])) as command:
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
            self.assertFalse(any(is_delete(call.args) for call in command.call_args_list))
        # Demoting a newer release makes the previously fourth release one of
        # the three retained stables, even though its own metadata did not change.
        demoted = [dict(raw) for raw in original]
        demoted[2]["prerelease"] = True
        with patch.object(prune, "list_releases", side_effect=[parsed(original), parsed(demoted)]), patch.object(prune, "command") as command:
            prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        command.assert_not_called()

    def test_new_stable_during_apply_stops_this_older_cleanup(self):
        original = stable_history()
        updated = original + [release(5, "v1.64.16")]
        with patch.object(prune, "list_releases", side_effect=[parsed(original), parsed(updated)]), patch.object(prune, "command") as command:
            prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        command.assert_not_called()

    def test_final_get_rechecks_candidate_identity_and_classification(self):
        original = stable_history()
        for changes in (
            {"id": 100}, {"tag_name": "v1.64.11"}, {"draft": True},
            {"prerelease": True}, {"published_at": None},
        ):
            changed = dict(original[0], **changes)
            with self.subTest(changes=changes), patch.object(prune, "list_releases", return_value=parsed(original)), patch.object(prune, "command", return_value=json.dumps(changed)) as command:
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
            self.assertEqual(command.call_count, 1)
            self.assertFalse(is_delete(command.call_args.args))

    def test_api_errors_abort_before_deletion(self):
        failure = subprocess.CalledProcessError(1, "gh", stderr="HTTP 403: denied")
        with patch.object(prune, "list_releases", side_effect=failure), patch.object(prune, "command") as command:
            with self.assertRaises(subprocess.CalledProcessError) as raised:
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        self.assertIs(raised.exception, failure)
        command.assert_not_called()
        with patch.object(prune, "list_releases", return_value=parsed(stable_history())), patch.object(prune, "command", side_effect=failure) as command:
            with self.assertRaises(subprocess.CalledProcessError):
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        self.assertEqual(command.call_count, 1)
        self.assertFalse(is_delete(command.call_args.args))

    def test_404_requires_successful_list_confirming_candidate_is_absent(self):
        original = parsed(stable_history())
        missing = subprocess.CalledProcessError(1, "gh", stderr="HTTP 404: Not Found")
        for refresh in (original, original[1:]):
            with self.subTest(absent=len(refresh) == 3), patch.object(prune, "list_releases", side_effect=[original, original, refresh]), patch.object(prune, "command", side_effect=missing) as command:
                if len(refresh) == 3:
                    prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
                else:
                    with self.assertRaises(subprocess.CalledProcessError):
                        prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
            self.assertEqual(command.call_count, 1)
        denied = subprocess.CalledProcessError(1, "gh", stderr="HTTP 403: denied")
        with patch.object(prune, "list_releases", side_effect=[original, original, denied]), patch.object(prune, "command", side_effect=missing):
            with self.assertRaises(subprocess.CalledProcessError):
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)

    def test_retry_after_lost_delete_response_does_not_delete_retained_releases(self):
        history = stable_history()
        lost = subprocess.CalledProcessError(1, "gh", stderr="connection reset")
        with patch.object(prune, "list_releases", return_value=parsed(history)), patch.object(prune, "command", side_effect=[json.dumps(history[0]), lost]) as command:
            with self.assertRaises(subprocess.CalledProcessError):
                prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        self.assertTrue(is_delete(command.call_args.args))
        # The server completed DELETE, but its response was lost. The next run
        # recalculates retention using the three stable releases still present.
        with patch.object(prune, "list_releases", return_value=parsed(history[1:])), patch.object(prune, "command") as command:
            prune.prune_releases(REPOSITORY, TRIGGER, apply=True)
        command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
