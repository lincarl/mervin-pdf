#!/usr/bin/env python3
"""Exercise trusted cancellation against mocked PR state and Actions APIs."""

from contextlib import redirect_stdout
from copy import deepcopy
from io import StringIO
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import cancel_pr_ci as ci


REPOSITORY = "owner/project"
ROOT = f"repos/{REPOSITORY}"
HEAD = "a" * 40
OLD_HEAD = "b" * 40
NEW_HEAD = "c" * 40
ENV = {"GITHUB_EVENT_NAME": "pull_request_target", "GITHUB_REPOSITORY": REPOSITORY}
RUN_ENV = {**ENV, "GITHUB_EVENT_NAME": "workflow_run"}
# Keep the supported lifecycle events and active statuses independent of the
# controller's lists so removing one cannot remove its fixture and assertions.
EXPECTED_ACTIONS = ("synchronize", "closed", "converted_to_draft", "reopened", "ready_for_review")
EXPECTED_ACTIVE_STATUSES = ("queued", "in_progress", "waiting", "pending", "requested")


def event(action="synchronize"):
    """Create a lifecycle event whose head is deliberately stale."""
    return {
        "action": action, "number": 7, "repository": {"full_name": REPOSITORY},
        "pull_request": {
            "number": 7, "state": "open", "draft": False,
            "base": {"ref": "main", "repo": {"full_name": REPOSITORY}},
            "head": {"sha": OLD_HEAD, "ref": "feature/fix"},
        },
    }


def pull_request(sha=HEAD, state="open", draft=False):
    """Create the authoritative API response, independent of the event payload."""
    return {
        **event()["pull_request"], "state": state, "draft": draft,
        "head": {"sha": sha, "ref": "feature/fix"},
    }


def started_event(identifier=81, action="in_progress"):
    """Create a starting-attempt notification without a title or PR associations."""
    return {
        "action": action, "repository": {"full_name": REPOSITORY, "default_branch": "main"},
        "workflow_run": {
            "id": identifier, "name": "PR CI", "event": "pull_request", "path": ci.WORKFLOW,
            "head_sha": OLD_HEAD, "head_branch": "feature/fix", "run_attempt": 1,
        },
    }


def workflow_run(identifier=81, sha=OLD_HEAD, number=7, status="in_progress"):
    """Create an active automatic run with exact immutable title evidence."""
    return {
        "id": identifier, "name": "PR CI", "display_title": f"PR CI #{number} @ {sha}",
        "head_sha": sha, "head_branch": "feature/fix", "event": "pull_request",
        "path": ci.WORKFLOW, "status": status, "run_attempt": 1,
        "repository": {"full_name": REPOSITORY},
        "head_repository": {"full_name": REPOSITORY},
        "pull_requests": [{"number": number}],
    }


class FakeGitHub(ci.CancellationGitHub):
    """Serve mocked API reads and record every cancellation without network access."""

    def __init__(self, runs=(), current=None):
        super().__init__(REPOSITORY)
        self.calls = []
        self.cancellations = []
        self.cancel_error = None
        self.after_cancel_error = None
        self.before_read = None
        self.current = deepcopy(current if current is not None else pull_request())
        self.details = {str(run["id"]): deepcopy(run) for run in runs}
        self.responses = {}
        for status in EXPECTED_ACTIVE_STATUSES:
            matches = [deepcopy(run) for run in runs if run["status"] == status]
            # Always include the empty last page for exact multiples of 100.
            for offset in range(0, len(matches) + 1, 100):
                self.responses[self.page_path(status, offset // 100 + 1)] = {
                    "total_count": len(matches), "workflow_runs": matches[offset:offset + 100],
                }

    @staticmethod
    def page_path(status="in_progress", page=1):
        """Build the exact production search path expected by the fixture."""
        return (f"{ROOT}/actions/workflows/pr-ci.yml/runs?event=pull_request"
                f"&status={status}&per_page=100&page={page}")

    def api(self, path):
        """Return JSON fixtures, with optional state changes at a read boundary."""
        self.calls.append(("GET", path))
        if self.before_read:
            self.before_read(path)
        if path == f"{ROOT}/pulls/7":
            value = self.current
        elif path in self.responses:
            value = self.responses[path]
        elif path.startswith(f"{ROOT}/actions/runs/"):
            value = self.details[path.rsplit("/", 1)[-1]]
        else:
            raise AssertionError(f"Unexpected API read {path}")
        if isinstance(value, Exception):
            raise value
        return deepcopy(value)

    def cancel(self, identifier):
        """Record a cancellation and simulate optional completion races or errors."""
        self.calls.append(("POST", f"{ROOT}/actions/runs/{identifier}/cancel"))
        self.cancellations.append(identifier)
        if self.cancel_error:
            if self.after_cancel_error is not None:
                self.details[identifier] = deepcopy(self.after_cancel_error)
            raise self.cancel_error


class CancellationTests(unittest.TestCase):
    def execute(self, api, payload=None, environment=None):
        """Exercise the real controller without writing routine output."""
        with redirect_stdout(StringIO()):
            return ci.cancel_obsolete(payload or event(), environment or ENV, api)

    def test_cancels_only_older_heads_for_the_same_pr(self):
        runs = [workflow_run(), workflow_run(82, sha=HEAD), workflow_run(83, number=8)]
        api = FakeGitHub(runs)
        self.assertEqual(self.execute(api), ["81"])
        self.assertEqual(api.cancellations, ["81"])
        self.assertNotIn(("GET", f"{ROOT}/actions/runs/83"), api.calls)

    def test_stale_event_and_obsolete_rerun_preserve_current_live_head(self):
        runs = [workflow_run(), {**workflow_run(82, sha=HEAD), "run_attempt": 3}]
        for action in EXPECTED_ACTIONS:
            with self.subTest(action=action):
                payload = event(action)
                payload["pull_request"]["state"] = "closed"
                payload["pull_request"]["draft"] = True
                api = FakeGitHub(runs)
                self.assertEqual(self.execute(api, payload), ["81"])
                self.assertEqual(api.cancellations, ["81"])

    def test_closed_and_draft_cancel_all_heads_for_this_pr(self):
        runs = [workflow_run(), workflow_run(82, sha=HEAD), workflow_run(83, number=8)]
        for current in (pull_request(state="closed"), pull_request(draft=True)):
            with self.subTest(current=current):
                api = FakeGitHub(runs, current)
                self.assertEqual(self.execute(api), ["81", "82"])

    def test_current_head_is_refreshed_after_pagination(self):
        api = FakeGitHub([workflow_run(81, sha=NEW_HEAD)])

        def advance_head(path):
            """Advance the PR while the old controller enumerates active runs."""
            if path == f"{ROOT}/actions/runs/81":
                api.current = pull_request(sha=NEW_HEAD)

        api.before_read = advance_head
        self.assertEqual(self.execute(api), [])
        self.assertEqual(api.cancellations, [])

    def test_reopen_or_ready_transition_before_cancel_preserves_live_head(self):
        for previous in (pull_request(state="closed"), pull_request(draft=True)):
            api = FakeGitHub([workflow_run(sha=HEAD)], previous)

            def make_ready(path):
                """Reopen or ready the PR before the cancellation decision."""
                if path == f"{ROOT}/actions/runs/81":
                    api.current = pull_request()

            api.before_read = make_ready
            with self.subTest(previous=previous):
                self.assertEqual(self.execute(api, event("closed")), [])

    def test_exact_workflow_event_and_source_title_are_required(self):
        variants = {
            "name": ["CI", "Release", "PR CI "],
            "path": [".github/workflows/ci.yml", ".github/workflows/release.yml",
                     ci.WORKFLOW + "@refs/heads/main", None],
            "event": ["push", "workflow_dispatch", "pull_request_target", "release"],
            "head_sha": [HEAD, None],
            "display_title": [f"PR CI #8 @ {OLD_HEAD}", f"PR CI #07 @ {OLD_HEAD}",
                              f"PR CI #7 @ {OLD_HEAD[:7]}", f"PR CI #7 @ {OLD_HEAD.upper()}",
                              f"PR CI #7 @ {OLD_HEAD}\n", f"PR CI #7 @ {HEAD}", None],
            "repository": [{"full_name": "other/project"}, None],
            "status": ["completed", "unknown", None],
        }
        for field, values in variants.items():
            for value in values:
                candidate = {**workflow_run(), field: value}
                api = FakeGitHub([workflow_run()])
                # Inject invalid metadata even if GitHub search filters disagree.
                api.responses[api.page_path()]["workflow_runs"] = [candidate]
                api.details["81"] = candidate
                with self.subTest(field=field, value=value):
                    self.assertEqual(self.execute(api), [])
                    self.assertEqual(api.cancellations, [])

    def test_main_pushes_releases_and_manual_runs_are_ignored(self):
        runs = [workflow_run()]
        for identifier, name, path, action, branch in (
            (82, "CI", ".github/workflows/ci.yml", "push", "main"),
            (83, "Release", ".github/workflows/release.yml", "push", "main"),
            (84, "PR CI", ci.WORKFLOW, "workflow_dispatch", "main"),
        ):
            runs.append({**workflow_run(identifier), "name": name, "path": path,
                         "event": action, "head_branch": branch})
        api = FakeGitHub(runs)
        self.assertEqual(self.execute(api), ["81"])

    def test_malformed_associations_and_conflicting_matching_entries_are_rejected(self):
        references = [
            [None], [{}],
            [{"number": True}], [{"number": "07"}], "7", {"number": 7},
            [{"number": 7, "base": {"repo": {"full_name": "other/project"}}}],
            [{"number": 7, "base": None}],
        ]
        for value in references:
            api = FakeGitHub([{**workflow_run(), "pull_requests": value}])
            with self.subTest(references=value):
                self.assertEqual(self.execute(api), [])

    def test_pr_associations_can_include_other_open_prs_for_the_same_source(self):
        run = {**workflow_run(), "pull_requests": [{"number": 8}, {"number": 7}]}
        api = FakeGitHub([run])
        self.assertEqual(self.execute(api), ["81"])

    def test_closed_triggering_pr_can_be_absent_while_another_pr_stays_open(self):
        old = {**workflow_run(), "pull_requests": [{"number": 8}]}
        other = workflow_run(82, number=8)
        api = FakeGitHub([old, other], pull_request(state="closed"))
        self.assertEqual(self.execute(api, event("closed")), ["81"])
        self.assertEqual(api.cancellations, ["81"])
        self.assertNotIn(("GET", f"{ROOT}/pulls/8"), api.calls)

    def test_live_branch_associations_do_not_replace_title_or_immutable_source(self):
        references = [
            [{"number": 8}],
            [{"number": 8, "head": {"sha": NEW_HEAD, "ref": "feature/fix"}}],
            [{"number": 7, "head": {"sha": NEW_HEAD, "ref": "renamed"}}],
        ]
        for associations in references:
            old = {**workflow_run(), "pull_requests": associations}
            live = {**workflow_run(82, sha=HEAD), "pull_requests": [{"number": 8}]}
            other = {**workflow_run(83, number=8), "pull_requests": [{"number": 7}]}
            api = FakeGitHub([old, live, other])
            with self.subTest(associations=associations):
                self.assertEqual(self.execute(api), ["81"])

    def test_minimal_github_pr_association_schema_does_not_require_full_name(self):
        reference = {
            "id": 123, "number": 7, "url": f"https://api.github.com/{ROOT}/pulls/7",
            "head": {"ref": "feature/fix", "sha": OLD_HEAD,
                     "repo": {"id": 456, "name": "project", "url": f"https://api.github.com/{ROOT}"}},
            "base": {"ref": "main", "sha": HEAD,
                     "repo": {"id": 456, "name": "project", "url": f"https://api.github.com/{ROOT}"}},
        }
        api = FakeGitHub([{**workflow_run(), "pull_requests": [reference]}])
        self.assertEqual(self.execute(api), ["81"])

    def test_forks_with_missing_pr_arrays_use_exact_title_and_actual_head(self):
        for value in ([], None, "missing"):
            run = {**workflow_run(), "head_repository": {"full_name": "fork/project"}}
            if value == "missing":
                del run["pull_requests"]
            else:
                run["pull_requests"] = value
            api = FakeGitHub([run, workflow_run(82, sha=HEAD), workflow_run(83, number=8)])
            with self.subTest(references=value):
                self.assertEqual(self.execute(api), ["81"])

    def test_branch_names_neither_authorize_nor_prevent_cancellation(self):
        run = {**workflow_run(), "head_branch": "renamed-branch"}
        api = FakeGitHub([run, workflow_run(82, number=8)])
        self.assertEqual(self.execute(api), ["81"])

    def test_run_details_are_checked_again_before_cancellation(self):
        for override in ({"id": 999}, {"head_sha": HEAD}, {"event": "push"},
                         {"status": "completed"}):
            api = FakeGitHub([workflow_run()])
            api.details["81"].update(override)
            with self.subTest(override=override):
                self.assertEqual(self.execute(api), [])

    def test_every_active_status_and_page_are_collected_before_writes(self):
        runs = [workflow_run(identifier=index, status="queued") for index in range(1, 102)]
        runs += [workflow_run(identifier=200 + index, status=status)
                 for index, status in enumerate(EXPECTED_ACTIVE_STATUSES) if status != "queued"]
        api = FakeGitHub(runs)
        expected_ids = {str(run["id"]) for run in runs}
        # Each run must occur exactly once in both the result and actual POSTs.
        self.assertCountEqual(self.execute(api), expected_ids)
        self.assertCountEqual(api.cancellations, expected_ids)
        self.assertIn(("GET", api.page_path("queued", 2)), api.calls)
        first_write = next(index for index, call in enumerate(api.calls) if call[0] == "POST")
        self.assertTrue(all("/workflows/" not in path for _, path in api.calls[first_write:]))

    def test_runs_moving_between_active_statuses_are_deduplicated(self):
        api = FakeGitHub([workflow_run()])
        api.responses[api.page_path("queued")] = {
            "total_count": 1, "workflow_runs": [workflow_run(status="queued")],
        }
        self.assertEqual(self.execute(api), ["81"])
        self.assertEqual(api.cancellations, ["81"])

    def test_exact_thousand_result_boundary_does_not_request_a_capped_page(self):
        api = FakeGitHub([workflow_run(identifier=index) for index in range(1, 1001)])
        del api.responses[api.page_path(page=11)]
        self.assertEqual(len(ci.active_runs(api)), 1000)
        self.assertNotIn(("GET", api.page_path(page=11)), api.calls)

    def test_invalid_or_truncated_pagination_fails_before_any_cancellation(self):
        responses = [None, {}, {"total_count": 1, "workflow_runs": None},
                     {"total_count": 1, "workflow_runs": [None]},
                     {"total_count": True, "workflow_runs": []},
                     {"total_count": -1, "workflow_runs": []},
                     {"total_count": 1001, "workflow_runs": [workflow_run()]}]
        for response in responses:
            api = FakeGitHub([workflow_run()])
            api.responses[api.page_path("requested")] = response
            with self.subTest(response=response), self.assertRaises(ci.CIError):
                self.execute(api)
            self.assertEqual(api.cancellations, [])

    def test_invalid_event_inputs_do_not_read_api(self):
        payloads = [None, {}, [], {**event(), "action": "opened"},
                    {**event(), "number": True}, {**event(), "number": "07"},
                    {**event(), "repository": {"full_name": "other/project"}}]
        payload = event()
        payload["pull_request"]["number"] = 8
        payloads.append(payload)
        for payload in payloads:
            api = FakeGitHub([workflow_run()])
            with self.subTest(payload=payload), self.assertRaises(ci.CIError):
                ci.cancel_obsolete(payload, ENV, api)
            self.assertEqual(api.calls, [])
        for name in ("pull_request", "push", "workflow_dispatch", None):
            api = FakeGitHub([workflow_run()])
            with self.subTest(name=name), self.assertRaises(ci.CIError):
                self.execute(api, environment={**ENV, "GITHUB_EVENT_NAME": name})
            self.assertEqual(api.calls, [])
        api = FakeGitHub([workflow_run()])
        with self.assertRaises(ci.CIError):
            self.execute(api, environment={**ENV, "GITHUB_REPOSITORY": "other/project"})
        self.assertEqual(api.calls, [])

    def test_invalid_or_unavailable_current_state_fails_before_cancellation(self):
        variants = [None, {}, {**pull_request(), "number": 8},
                    {**pull_request(), "draft": "false"}, {**pull_request(), "state": "unknown"},
                    {**pull_request(), "head": {"sha": "main"}},
                    {**pull_request(), "base": {"repo": {"full_name": "other/project"}}},
                    ci.CIError("API unavailable")]
        for current in variants:
            api = FakeGitHub([workflow_run()])
            api.current = current
            with self.subTest(current=current), self.assertRaises(ci.CIError):
                self.execute(api)
            self.assertEqual(api.cancellations, [])

    def test_cancellation_failure_is_ok_only_if_reread_run_completed(self):
        api = FakeGitHub([workflow_run()])
        api.cancel_error = ci.CIError("HTTP 409")
        api.after_cancel_error = workflow_run(status="completed")
        self.assertEqual(self.execute(api), [])
        self.assertEqual(api.cancellations, ["81"])
        self.assertEqual(api.calls[-1], ("GET", f"{ROOT}/actions/runs/81"))

    def test_active_run_or_unavailable_reread_after_failed_cancel_fails_visibly(self):
        for observed in (workflow_run(), {**workflow_run(status="completed"), "id": 99},
                         {}, ci.CIError("API unavailable")):
            api = FakeGitHub([workflow_run()])
            api.cancel_error = ci.CIError("HTTP 403")
            api.after_cancel_error = observed
            with self.subTest(observed=observed), self.assertRaises(ci.CIError):
                self.execute(api)
            self.assertEqual(api.cancellations, ["81"])


class StartedRunTests(unittest.TestCase):
    def execute(self, api, payload=None):
        """Reconcile one mocked starting run through the runner-facing dispatch."""
        with redirect_stdout(StringIO()):
            return ci.cancel_obsolete(payload if payload is not None else started_event(), RUN_ENV, api)

    def test_partial_rerun_after_lifecycle_processing_does_not_need_prepare_again(self):
        original = workflow_run(status="completed")
        api = FakeGitHub([original])
        with redirect_stdout(StringIO()):
            self.assertEqual(ci.cancel_obsolete(event(), ENV, api), [])
        # The old successful prepare remains from attempt 1, while failed jobs rerun.
        api.responses[f"{ROOT}/actions/runs/81/jobs"] = {"jobs": [
            {"name": "Select PR revision", "conclusion": "success", "run_attempt": 1},
            {"name": "build / linux-tests", "status": "in_progress", "run_attempt": 2},
        ]}
        api.details["81"] = {**original, "status": "in_progress", "run_attempt": 2}
        api.calls.clear()
        self.assertEqual(self.execute(api), ["81"])
        self.assertEqual(api.cancellations, ["81"])
        self.assertFalse(any("/jobs" in path or "/workflows/" in path for _, path in api.calls))

    def test_closed_and_draft_reruns_cancel_even_for_the_current_source(self):
        rerun = {**workflow_run(sha=HEAD), "run_attempt": 2}
        for current in (pull_request(state="closed"), pull_request(draft=True)):
            api = FakeGitHub([rerun], current)
            with self.subTest(current=current):
                self.assertEqual(self.execute(api), ["81"])

    def test_active_rerun_of_the_current_live_source_is_preserved(self):
        api = FakeGitHub([{**workflow_run(sha=HEAD), "run_attempt": 2}])
        self.assertEqual(self.execute(api), [])
        self.assertEqual(api.cancellations, [])
        self.assertIn(("GET", f"{ROOT}/pulls/7"), api.calls)

    def test_delayed_old_attempt_notification_cannot_cancel_a_newer_run(self):
        old = {**workflow_run(), "run_attempt": 3}
        current = workflow_run(82, sha=HEAD)
        api = FakeGitHub([old, current])
        self.assertEqual(self.execute(api), ["81"])
        self.assertEqual(api.cancellations, ["81"])
        self.assertNotIn(("GET", f"{ROOT}/actions/runs/82"), api.calls)

    def test_notification_source_and_associations_do_not_override_actual_api_run(self):
        api = FakeGitHub([{**workflow_run(sha=HEAD), "run_attempt": 2}])
        payload = started_event()
        payload["workflow_run"]["pull_requests"] = [{"number": 8}]
        payload["workflow_run"]["display_title"] = f"PR CI #8 @ {OLD_HEAD}"
        self.assertEqual(self.execute(api, payload), [])
        self.assertEqual(api.cancellations, [])
        self.assertIn(("GET", f"{ROOT}/pulls/7"), api.calls)

    def test_current_pr_is_refreshed_after_run_metadata_inspection(self):
        api = FakeGitHub([workflow_run()])

        def restore_source(path):
            """Make the notified run the live source before the PR API read."""
            if path == f"{ROOT}/pulls/7":
                api.current = pull_request(sha=OLD_HEAD)

        api.before_read = restore_source
        self.assertEqual(self.execute(api), [])
        self.assertEqual(api.cancellations, [])

    def test_fork_attempts_without_pr_arrays_are_reconciled(self):
        for value in ([], None, "missing"):
            run = {**workflow_run(), "head_repository": {"full_name": "fork/project"}}
            if value == "missing":
                del run["pull_requests"]
            else:
                run["pull_requests"] = value
            api = FakeGitHub([run])
            with self.subTest(associations=value):
                self.assertEqual(self.execute(api), ["81"])

    def test_closed_triggering_pr_is_not_required_in_open_associations(self):
        run = {**workflow_run(), "pull_requests": [{"number": 8}], "run_attempt": 2}
        api = FakeGitHub([run], pull_request(state="closed"))
        self.assertEqual(self.execute(api), ["81"])
        self.assertNotIn(("GET", f"{ROOT}/pulls/8"), api.calls)

    def test_only_in_progress_notifications_are_accepted(self):
        for action in ("requested", "completed", "synchronize", None):
            api = FakeGitHub([workflow_run()])
            with self.subTest(action=action), self.assertRaises(ci.CIError):
                self.execute(api, started_event(action=action))
            self.assertEqual(api.calls, [])

    def test_invalid_notification_identity_fails_before_api_reads(self):
        payloads = [{}, [], {**started_event(), "repository": {"full_name": "other/project"}},
                    {**started_event(), "workflow_run": None}]
        payloads += [started_event(identifier=value) for value in (None, True, "081", "81/cancel")]
        for payload in payloads:
            api = FakeGitHub([workflow_run()])
            with self.subTest(payload=payload), self.assertRaises(ci.CIError):
                self.execute(api, payload)
            self.assertEqual(api.calls, [])
        api = FakeGitHub([workflow_run()])
        with self.assertRaises(ci.CIError):
            ci.cancel_obsolete(started_event(), {**RUN_ENV, "GITHUB_REPOSITORY": "other/project"}, api)
        self.assertEqual(api.calls, [])

    def test_invalid_or_unrelated_actual_run_metadata_cannot_authorize_cancellation(self):
        variants = [None, {}, {**workflow_run(), "id": 99}]
        changes = {
            "name": ["CI", None], "path": [".github/workflows/release.yml", ci.WORKFLOW + "@main"],
            "event": ["push", "workflow_dispatch", "pull_request_target"],
            "head_sha": [HEAD, "main", None],
            "display_title": [None, "PR CI", f"PR CI #07 @ {OLD_HEAD}", f"PR CI #7 @ {HEAD}"],
            "repository": [{"full_name": "other/project"}, None],
            "status": ["completed", "unknown"],
        }
        variants += [{**workflow_run(), field: value} for field, values in changes.items() for value in values]
        for run in variants:
            api = FakeGitHub([workflow_run()])
            api.details["81"] = run
            with self.subTest(run=run):
                self.assertEqual(self.execute(api), [])
                self.assertEqual(api.cancellations, [])
                self.assertEqual(api.calls, [("GET", f"{ROOT}/actions/runs/81")])


class TransportTests(unittest.TestCase):
    def test_transport_exposes_only_exact_gets_and_cancellation_post(self):
        api = ci.CancellationGitHub(REPOSITORY)
        with patch.object(ci.subprocess, "run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, stdout='{"number": 7}')
            self.assertEqual(api.api(f"{ROOT}/pulls/7"), {"number": 7})
            api.cancel("81")
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ["gh", "api", f"{ROOT}/pulls/7", "--method", "GET"],
            ["gh", "api", f"{ROOT}/actions/runs/81/cancel", "--method", "POST"],
        ])

    def test_cancel_transport_errors_become_visible_controller_errors(self):
        for failure in (OSError("missing gh"), subprocess.TimeoutExpired("gh", 30), None):
            with self.subTest(failure=failure), patch.object(ci.subprocess, "run") as run:
                run.side_effect = failure
                run.return_value = subprocess.CompletedProcess([], 1, stdout="", stderr="secret")
                with self.assertRaises(ci.CIError) as caught:
                    ci.CancellationGitHub(REPOSITORY).cancel("81")
                self.assertNotIn("secret", str(caught.exception))

    def test_invalid_identifiers_never_reach_transport(self):
        for identifier in (True, "081", "81/force-cancel", "81\n", 0):
            with self.subTest(identifier=identifier), patch.object(ci.subprocess, "run") as run:
                with self.assertRaises(ci.CIError):
                    ci.CancellationGitHub(REPOSITORY).cancel(identifier)
                run.assert_not_called()

    def test_runner_cli_reads_event_file_and_uses_environment(self):
        path = Path("/runner/event.json")
        api = FakeGitHub([workflow_run()])
        environment = {**ENV, "GITHUB_EVENT_PATH": str(path), "GH_TOKEN": "mock-token"}
        with patch.dict(os.environ, environment, clear=True), patch.object(ci.sys, "argv", ["cancel_pr_ci.py"]), \
                patch.object(Path, "read_text", return_value=json.dumps(event())) as read, \
                patch.object(ci, "CancellationGitHub", return_value=api) as client, redirect_stdout(StringIO()):
            ci.main()
        read.assert_called_once_with()
        client.assert_called_once_with(REPOSITORY)
        self.assertEqual(api.cancellations, ["81"])

    def test_runner_cli_accepts_starting_attempt_events(self):
        api = FakeGitHub([{**workflow_run(), "run_attempt": 2}])
        environment = {**RUN_ENV, "GITHUB_EVENT_PATH": "/runner/event.json", "GH_TOKEN": "mock-token"}
        with patch.dict(os.environ, environment, clear=True), patch.object(ci.sys, "argv", ["cancel_pr_ci.py"]), \
                patch.object(Path, "read_text", return_value=json.dumps(started_event())), \
                patch.object(ci, "CancellationGitHub", return_value=api), redirect_stdout(StringIO()):
            ci.main()
        self.assertEqual(api.cancellations, ["81"])


if __name__ == "__main__":
    unittest.main()
