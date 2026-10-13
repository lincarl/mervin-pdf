#!/usr/bin/env python3
"""Exercise review authorization and CI status transitions without GitHub writes."""

from copy import deepcopy
import contextlib
import io
import json
import os
import unittest
from unittest.mock import Mock, patch

import reviewed_ci as gate


SHA = "a" * 40
OLD_SHA = "b" * 40
BASE_SHA = "c" * 40
REPOSITORY = "owner/project"
ROOT = f"repos/{REPOSITORY}"
RUN_ID = "123"
URL = f"https://github.com/{REPOSITORY}/actions/runs/{RUN_ID}"
BOT = {"id": gate.BOT_ID, "login": gate.BOT_LOGIN, "type": "Bot"}
SUMMARY_MARKER = "<!-- This is an auto-generated comment: summarize by coderabbit.ai -->"
ENV = {
    "GITHUB_REF": "refs/heads/main",
    "GITHUB_EVENT_NAME": "workflow_dispatch",
    "GITHUB_RUN_ATTEMPT": "1",
    "GITHUB_RUN_ID": RUN_ID,
    "GITHUB_ACTOR": "maintainer",
}


def review_status(**changes):
    value = {
        "id": 10, "context": "CodeRabbit", "state": "success",
        "description": "Review completed", "creator": deepcopy(BOT),
    }
    value.update(changes)
    return value


def summary(sha=SHA, **changes):
    coverage = json.dumps({"kind": "reviewed", "coveredCommitId": sha})
    value = {
        "id": 20, "user": deepcopy(BOT), "updated_at": "2026-10-13T10:00:00Z",
        "body": f"<!-- final_review_risk_coverage:{coverage} -->",
    }
    value.update(changes)
    value["body"] = SUMMARY_MARKER + "\n" + value["body"]
    return value


def ci_status(state="pending", target_url=URL):
    return {
        "context": gate.CONTEXT, "state": state, "target_url": target_url,
        "created_at": "2026-10-13T10:00:00Z",
    }


def workflow_run(identifier=123, sha=SHA, number="7", **changes):
    value = {
        "id": identifier, "event": "workflow_dispatch", "status": "in_progress",
        "display_title": f"reviewed-ci PR #{number} @ {sha}",
        "path": f".github/workflows/{gate.WORKFLOW}", "head_branch": "main",
        "created_at": "2026-10-13T11:00:00Z",
    }
    value.update(changes)
    return value


class GateFixture(unittest.TestCase):
    def setUp(self):
        self.api = Mock(spec=gate.GitHub)
        self.api.repository = REPOSITORY
        self.api.root = ROOT
        self.pr = {
            "state": "open", "draft": False,
            "head": {"sha": SHA}, "base": {"ref": "main"},
        }
        self.statuses = {SHA: [review_status()], OLD_SHA: []}
        self.comments = [summary()]
        self.reviews = []
        self.threads = []
        self.decision = None
        self.permission = "write"
        self.comparison = "ahead"
        self.runs = [workflow_run()]
        self.jobs = [
            {"name": f"build / {name}", "status": "completed", "conclusion": "success"}
            for name in ("repository-checks", "linux-tests (release)", "linux-tests (sanitize)",
                         "windows-tests", "windows-msix-tests")
        ]
        self.api.api.side_effect = self.request
        self.api.pages.side_effect = self.pages

    def request(self, path, method="GET", data=None):
        if path == f"{ROOT}/pulls/7":
            return deepcopy(self.pr)
        if path == f"{ROOT}/git/ref/heads/main":
            return {"object": {"sha": BASE_SHA}}
        if path == f"{ROOT}/compare/{BASE_SHA}...{SHA}":
            return {"status": self.comparison}
        if path == f"{ROOT}/collaborators/maintainer/permission":
            return {"permission": self.permission}
        if path == "graphql":
            return {"data": {"repository": {"pullRequest": {
                "reviewDecision": self.decision,
                "reviewThreads": {
                    "nodes": deepcopy(self.threads),
                    "pageInfo": {"hasNextPage": False, "endCursor": None},
                },
            }}}}
        for run in self.runs:
            if path == f"{ROOT}/actions/runs/{run['id']}":
                return deepcopy(run)
            if path == f"{ROOT}/actions/runs/{run['id']}/cancel" and method == "POST":
                return None
        self.fail(f"Unexpected GitHub request {method} {path}")

    def pages(self, path, key=None):
        for sha, statuses in self.statuses.items():
            if path == f"{ROOT}/commits/{sha}/statuses":
                return deepcopy(statuses)
        if path == f"{ROOT}/issues/7/comments":
            return deepcopy(self.comments)
        if path == f"{ROOT}/pulls/7/reviews":
            return deepcopy(self.reviews)
        if path.startswith(f"{ROOT}/actions/workflows/{gate.WORKFLOW}/runs?event=workflow_dispatch&status="):
            self.assertEqual(key, "workflow_runs")
            state = path.rsplit("=", 1)[-1]
            return deepcopy([run for run in self.runs if run["status"] == state])
        if path == f"{ROOT}/actions/runs/{RUN_ID}/jobs?filter=latest":
            self.assertEqual(key, "jobs")
            return deepcopy(self.jobs)
        self.fail(f"Unexpected paginated request {path}")


class ReviewEvidenceTests(unittest.TestCase):
    def test_completed_review_identifies_current_revision_and_evidence(self):
        self.assertEqual(
            gate.validate_coderabbit([review_status()], [summary()], SHA),
            {"status_id": 10, "summary_id": 20},
        )

    def test_pending_failed_missing_or_untrusted_status_never_uses_older_success(self):
        cases = [
            [],
            [review_status(state="pending"), review_status()],
            [review_status(state="failure"), review_status()],
            [review_status(state="error"), review_status()],
            [review_status(description="Review skipped")],
            [review_status(creator={**BOT, "id": 1}), review_status()],
            [review_status(creator={**BOT, "type": "User"})],
            [review_status(creator=None)],
        ]
        for statuses in cases:
            with self.subTest(statuses=statuses), self.assertRaises(gate.GateError):
                gate.validate_coderabbit(statuses, [summary()], SHA)

    def test_coverage_must_be_trusted_current_unambiguous_and_reviewed(self):
        cases = [
            [], [summary(OLD_SHA)], [summary(user={**BOT, "id": 1})], [summary(user=None)],
            [summary(body="<!-- final_review_risk_coverage:{bad} -->")],
            [summary(body=summary()["body"] * 2)],
            [summary(body=summary()["body"].replace('"reviewed"', '"skipped"'))],
        ]
        for comments in cases:
            with self.subTest(comments=comments), self.assertRaises(gate.GateError):
                gate.validate_coderabbit([review_status()], comments, SHA)

    def test_newer_skip_summary_overrides_older_completed_summary(self):
        skipped = summary(
            id=21, updated_at="2026-10-13T11:00:00Z",
            body="<!-- This is an auto-generated comment: skip review by coderabbit.ai -->",
        )
        with self.assertRaisesRegex(gate.GateError, "skipped"):
            gate.validate_coderabbit([review_status()], [summary(), skipped], SHA)

    def test_active_review_markers_reject_even_matching_coverage(self):
        for marker in ("skip review", "review paused", "reviews paused", "pause review",
                       "review in progress", "rate limited by coderabbit.ai", "rate_limit_exceeded"):
            with self.subTest(marker=marker), self.assertRaises(gate.GateError):
                gate.validate_coderabbit(
                    [review_status()], [summary(body=f"<!-- {marker} -->" + summary()["body"])], SHA,
                )

    def test_newer_incomplete_summary_cannot_reuse_older_successful_coverage(self):
        for marker in ("review in progress", "pause review", "rate limited"):
            latest = summary(
                id=21, updated_at="2026-10-13T11:00:00Z",
                body=f"<!-- This is an auto-generated comment: {marker} by coderabbit.ai -->",
            )
            with self.subTest(marker=marker), self.assertRaisesRegex(gate.GateError, "skipped, paused"):
                gate.validate_coderabbit([review_status()], [summary(), latest], SHA)

    def test_expired_notice_reply_does_not_invalidate_completed_summary(self):
        reply = summary(id=21, updated_at="2026-10-13T11:00:00Z")
        reply["body"] = "<!-- This is an auto-generated reply by CodeRabbit -->\nReview was rate limited earlier."
        gate.validate_coderabbit([review_status()], [reply, summary()], SHA)

    def test_coverage_inside_one_off_bot_reply_is_not_review_completion(self):
        reply = summary()
        reply["body"] = reply["body"].replace(SUMMARY_MARKER, "<!-- This is an auto-generated reply by CodeRabbit -->")
        with self.assertRaisesRegex(gate.GateError, "missing"):
            gate.validate_coderabbit([review_status()], [reply], SHA)


class InspectionTests(GateFixture):
    def test_complete_current_review_authorizes_regular_fresh_pr(self):
        self.assertEqual(gate.inspect(self.api, "7", SHA), {
            "pr_number": "7", "sha": SHA, "base_sha": BASE_SHA,
            "status_id": 10, "summary_id": 20,
        })
        self.api.status.assert_not_called()

    def test_inputs_reject_aliases_short_shas_and_shell_fragments(self):
        for number, sha in (("07", SHA), ("0", SHA), ("7;echo x", SHA),
                            ("7", SHA[:7]), ("7", SHA.upper())):
            with self.subTest(number=number, sha=sha), self.assertRaises(gate.GateError):
                gate.inspect(self.api, number, sha)
        self.api.api.assert_not_called()

    def test_closed_draft_stale_head_and_other_base_are_rejected(self):
        for replacement in ({"state": "closed"}, {"draft": True},
                            {"head": {"sha": OLD_SHA}}, {"base": {"ref": "release"}}):
            original = deepcopy(self.pr)
            self.pr.update(replacement)
            with self.subTest(replacement=replacement), self.assertRaises(gate.GateError):
                gate.inspect(self.api, "7", SHA)
            self.pr = original

    def test_behind_or_diverged_branches_require_fresh_review_after_update(self):
        for state in ("behind", "diverged"):
            self.comparison = state
            with self.subTest(state=state), self.assertRaisesRegex(gate.GateError, "main"):
                gate.inspect(self.api, "7", SHA)

    def test_head_change_during_paginated_review_reads_invalidates_authorization(self):
        original = self.api.pages.side_effect

        def pages(path, key=None):
            result = original(path, key)
            if path.endswith("/reviews"):
                self.pr["head"]["sha"] = OLD_SHA
            return result

        self.api.pages.side_effect = pages
        with self.assertRaisesRegex(gate.GateError, "changed during"):
            gate.inspect(self.api, "7", SHA)

    def test_unresolved_outdated_threads_still_block(self):
        for outdated in (False, True):
            self.threads = [{"id": "thread", "isResolved": False, "isOutdated": outdated}]
            with self.subTest(outdated=outdated), self.assertRaisesRegex(gate.GateError, "Unresolved"):
                gate.inspect(self.api, "7", SHA)
        self.threads[0]["isResolved"] = True
        gate.inspect(self.api, "7", SHA)

    def test_formal_approval_policy_requires_current_bot_approval(self):
        self.decision = "REVIEW_REQUIRED"
        with self.assertRaisesRegex(gate.GateError, "formal review"):
            gate.inspect(self.api, "7", SHA)
        self.decision = "APPROVED"
        for reviews in ([], [{"id": 30, "user": BOT, "state": "APPROVED", "commit_id": OLD_SHA}]):
            self.reviews = reviews
            with self.subTest(reviews=reviews), self.assertRaisesRegex(gate.GateError, "formal approval"):
                gate.inspect(self.api, "7", SHA)
        self.reviews[0]["commit_id"] = SHA
        gate.inspect(self.api, "7", SHA)

    def test_changes_requested_remains_blocking_until_approved_or_dismissed(self):
        self.reviews = [{"id": 30, "user": BOT, "state": "CHANGES_REQUESTED", "commit_id": OLD_SHA}]
        with self.assertRaisesRegex(gate.GateError, "requests changes"):
            gate.inspect(self.api, "7", SHA)
        self.reviews.append({"id": 31, "user": BOT, "state": "DISMISSED", "commit_id": SHA})
        gate.inspect(self.api, "7", SHA)

    def test_all_thread_pages_are_checked_and_graphql_errors_fail_closed(self):
        first = self.request("graphql")
        first["data"]["repository"]["pullRequest"]["reviewThreads"]["pageInfo"] = {
            "hasNextPage": True, "endCursor": "next",
        }
        second = self.request("graphql")
        second["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"] = [
            {"id": "late", "isResolved": False, "isOutdated": True},
        ]
        self.api.api.side_effect = [first, second]
        with self.assertRaisesRegex(gate.GateError, "Unresolved"):
            gate.review_policy(self.api, "7")
        self.assertEqual(self.api.api.call_args.args[2]["variables"]["cursor"], "next")
        self.api.api.side_effect = [{"errors": [{"message": "Access denied"}]}]
        with self.assertRaises(gate.GateError):
            gate.review_policy(self.api, "7")


@patch.dict(os.environ, ENV)
class LifecycleTests(GateFixture):
    def test_changed_eligibility_during_deduplication_rejects_authorization(self):
        def previous_status(*_args):
            self.comments = [summary(OLD_SHA)]
            return None

        with patch.object(gate, "latest_status", side_effect=previous_status):
            with self.assertRaisesRegex(gate.GateError, "current PR commit"):
                gate.start(self.api, "7", SHA)
        self.api.status.assert_not_called()

    def test_authorized_dispatch_publishes_pending_on_pr_sha(self):
        self.assertEqual(gate.start(self.api, "7", SHA)["sha"], SHA)
        self.api.status.assert_called_once_with(
            SHA, "pending", "Authorized revision is running full CI", URL,
        )

    def test_non_main_automatic_events_native_reruns_and_readers_are_rejected(self):
        for env in ({"GITHUB_REF": "refs/heads/feature"},
                    {"GITHUB_EVENT_NAME": "pull_request"}, {"GITHUB_RUN_ATTEMPT": "2"}):
            with self.subTest(env=env), patch.dict(os.environ, env), self.assertRaises(gate.GateError):
                gate.start(self.api, "7", SHA)
        self.permission = "read"
        with self.assertRaisesRegex(gate.GateError, "write permission"):
            gate.start(self.api, "7", SHA)
        self.api.status.assert_not_called()

    def test_duplicate_requests_do_not_start_another_build(self):
        for state in ("pending", "success", "failure", "error"):
            self.statuses[SHA] = [ci_status(state), review_status()]
            with self.subTest(state=state), contextlib.redirect_stdout(io.StringIO()):
                self.assertIsNone(gate.start(self.api, "7", SHA))
        self.api.status.assert_not_called()

    def test_explicit_retry_revalidates_reviews_then_claims_failed_attempt(self):
        self.statuses[SHA] = [ci_status("failure", URL + "0"), review_status()]
        self.assertIsNotNone(gate.start(self.api, "7", SHA, retry=True))
        self.api.status.assert_called_once_with(
            SHA, "pending", "Authorized revision is running full CI", URL,
        )
        self.api.status.reset_mock()
        self.comments = [summary(OLD_SHA)]
        with self.assertRaises(gate.GateError):
            gate.start(self.api, "7", SHA, retry=True)
        self.api.status.assert_not_called()

    def test_overlapping_explicit_retries_are_deduplicated(self):
        for state in ("pending", "success", "failure"):
            previous = ci_status(state, URL + "0")
            previous["created_at"] = "2026-10-13T11:30:00Z"
            self.statuses[SHA] = [previous, review_status()]
            with self.subTest(state=state), contextlib.redirect_stdout(io.StringIO()):
                self.assertIsNone(gate.start(self.api, "7", SHA, retry=True))
        self.api.status.assert_not_called()

    def test_explicit_retry_does_not_replace_active_pending_attempt(self):
        self.statuses[SHA].insert(0, ci_status(target_url=URL + "0"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(gate.start(self.api, "7", SHA, retry=True))
        self.api.status.assert_not_called()

    def test_success_requires_owned_pending_status_and_fresh_evidence(self):
        self.statuses[SHA].insert(0, ci_status())
        gate.finish(self.api, "7", SHA, "success")
        self.api.status.assert_called_once_with(
            SHA, "success", "Reviewed commit passed all required full CI jobs", URL,
        )

    def test_failed_cancelled_or_skipped_builds_never_succeed(self):
        self.statuses[SHA].insert(0, ci_status())
        for result in ("failure", "cancelled", "skipped", ""):
            self.api.status.reset_mock()
            with self.subTest(result=result), self.assertRaises(gate.GateError):
                gate.finish(self.api, "7", SHA, result)
            self.assertEqual(self.api.status.call_args.args[:2], (SHA, "failure"))

    def test_aggregate_success_cannot_hide_skipped_missing_or_failed_required_jobs(self):
        self.statuses[SHA].insert(0, ci_status())
        originals = deepcopy(self.jobs)
        for replacement in (None, {"status": "completed", "conclusion": "skipped"},
                            {"status": "completed", "conclusion": "failure"},
                            {"status": "in_progress", "conclusion": None}):
            self.jobs = deepcopy(originals)
            if replacement is None:
                self.jobs.pop()
            else:
                self.jobs[-1].update(replacement)
            self.api.status.reset_mock()
            with self.subTest(replacement=replacement), self.assertRaisesRegex(gate.GateError, "jobs"):
                gate.finish(self.api, "7", SHA, "success")
            self.assertEqual(self.api.status.call_args.args[:2], (SHA, "failure"))

    def test_push_after_start_turns_previous_revision_failure(self):
        self.statuses[SHA].insert(0, ci_status())
        self.pr["head"]["sha"] = OLD_SHA
        with self.assertRaisesRegex(gate.GateError, "no longer matches"):
            gate.finish(self.api, "7", SHA, "success")
        self.assertEqual(self.api.status.call_args.args[:2], (SHA, "failure"))

    def test_late_finish_cannot_overwrite_newer_attempt_or_terminal_status(self):
        for previous in (ci_status(target_url=URL + "0"), ci_status("success"), ci_status("failure")):
            self.statuses[SHA] = [previous, review_status()]
            with self.subTest(previous=previous), self.assertRaisesRegex(gate.GateError, "owns"):
                gate.finish(self.api, "7", SHA, "success")
        self.api.status.assert_not_called()

    def test_invalidation_cancels_only_obsolete_active_runs_for_this_pr(self):
        self.runs = [
            workflow_run(123, OLD_SHA), workflow_run(124),
            workflow_run(125, OLD_SHA, number="8"),
            workflow_run(126, OLD_SHA, status="completed"),
            workflow_run(127, OLD_SHA, event="push"),
        ]
        self.statuses[OLD_SHA] = [ci_status()]
        gate.invalidate(self.api, "7")
        self.api.status.assert_called_once_with(
            OLD_SHA, "failure", "PR revision changed or PR is no longer ready", URL,
        )
        cancellations = [call.args[0] for call in self.api.api.call_args_list
                         if call.args[0].endswith("/cancel")]
        self.assertEqual(cancellations, [f"{ROOT}/actions/runs/123/cancel"])

    def test_draft_or_closed_pr_cancels_current_run_without_clobbering_newer_status(self):
        self.runs = [workflow_run()]
        self.statuses[SHA].insert(0, ci_status(target_url=URL + "0"))
        for change in ({"draft": True}, {"state": "closed"}):
            self.pr.update(change)
            self.api.api.reset_mock()
            gate.invalidate(self.api, "7")
            self.api.api.assert_any_call(f"{ROOT}/actions/runs/123/cancel", "POST")
        self.api.status.assert_not_called()

    def test_cancel_race_is_ignored_only_if_run_has_finished(self):
        self.runs = [workflow_run(123, OLD_SHA)]
        original = self.api.api.side_effect
        completed = False

        def request(path, method="GET", data=None):
            if path.endswith("/cancel"):
                raise gate.GateError("Cannot cancel this run")
            if path == f"{ROOT}/actions/runs/123" and completed:
                return workflow_run(123, OLD_SHA, status="completed")
            return original(path, method, data)

        self.api.api.side_effect = request
        with self.assertRaisesRegex(gate.GateError, "Cannot cancel"):
            gate.invalidate(self.api, "7")
        completed = True
        gate.invalidate(self.api, "7")

    def test_reconcile_closes_abandoned_pending_even_when_overall_run_succeeded(self):
        self.statuses[SHA].insert(0, ci_status())
        for conclusion in ("success", "failure", "cancelled", "skipped"):
            self.api.status.reset_mock()
            self.runs = [workflow_run(status="completed", conclusion=conclusion)]
            gate.reconcile(self.api, RUN_ID)
            self.api.status.assert_called_once_with(
                SHA, "failure", "CI ended without a verified successful result", URL,
            )

    def test_reconcile_does_not_touch_success_newer_run_or_untrusted_workflow(self):
        self.runs = [workflow_run(status="completed")]
        for previous in (ci_status("success"), ci_status(target_url=URL + "0")):
            self.statuses[SHA] = [previous]
            gate.reconcile(self.api, RUN_ID)
        self.statuses[SHA] = [ci_status()]
        for change in ({"head_branch": "feature"}, {"event": "push"},
                       {"path": ".github/workflows/ci.yml"}, {"status": "in_progress"},
                       {"display_title": "reviewed-ci PR #07 @ " + SHA}):
            self.runs = [workflow_run(**{"status": "completed", **change})]
            gate.reconcile(self.api, RUN_ID)
        self.api.status.assert_not_called()


class PaginationTests(unittest.TestCase):
    def test_pages_preserves_query_and_reads_past_first_page(self):
        api = gate.GitHub(REPOSITORY)
        with patch.object(api, "api", side_effect=[{"items": list(range(100))}, {"items": [100]}]) as request:
            self.assertEqual(api.pages("items?status=active", "items"), list(range(101)))
        self.assertEqual(request.call_args.args[0], "items?status=active&per_page=100&page=2")

    def test_pagination_limit_fails_instead_of_authorizing_partial_evidence(self):
        api = gate.GitHub(REPOSITORY)
        with patch.object(api, "api", return_value=list(range(100))):
            with self.assertRaisesRegex(gate.GateError, "incomplete evidence"):
                api.pages("items")


if __name__ == "__main__":
    unittest.main()
