#!/usr/bin/env python3
"""Exercise immutable PR selection, conservative CI reuse, and result aggregation."""

from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import reviewed_ci as ci


SHA = "a" * 40
CONTROL_SHA = "b" * 40
OTHER_SHA = "c" * 40
REPOSITORY = "owner/project"
ROOT = f"repos/{REPOSITORY}"
RUN_ID = "123"
PRIOR_RUN = "81"
ENV = {
    "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REPOSITORY": REPOSITORY,
    "GITHUB_REF": "refs/pull/7/merge", "GITHUB_RUN_ID": RUN_ID,
    "CI_WORKFLOW_SHA": CONTROL_SHA, "GITHUB_WORKFLOW_SHA": CONTROL_SHA,
    "GITHUB_SHA": OTHER_SHA,
}
# These are the five aggregates supplied by full-ci.yml, independent of the
# checker's list so dropping a required result cannot shrink the test cases.
EXPECTED_RESULT_VARIABLES = (
    "CI_REPOSITORY_RESULT", "CI_LINUX_RESULT", "CI_LINUX_SYSTEM_RESULT",
    "CI_WINDOWS_RESULT", "CI_MSIX_RESULT",
)


def changed(document, path, value):
    """Copy a fixture and replace one nested field."""
    result = deepcopy(document)
    target = result
    parts = path.split(".")
    for part in parts[:-1]:
        target = target[part]
    target[parts[-1]] = value
    return result


def event(repository=REPOSITORY):
    """Create a realistic open PR event with distinct source and control commits."""
    return {
        "number": 7, "repository": {"full_name": REPOSITORY},
        "pull_request": {
            "number": 7, "state": "open", "draft": False,
            "base": {"ref": "main", "repo": {"full_name": REPOSITORY}},
            "head": {"sha": SHA, "ref": "feature/fix", "repo": {"full_name": repository}},
        },
    }


def workflow_run():
    """Create successful run metadata with resolved full-ci control evidence."""
    return {
        "id": int(PRIOR_RUN), "event": "pull_request", "path": ".github/workflows/pr-ci.yml",
        "display_title": f"PR CI #7 @ {SHA}", "pull_requests": [],
        "status": "completed", "conclusion": "success", "head_sha": SHA,
        "head_repository": {"full_name": REPOSITORY}, "head_branch": "feature/fix",
        "repository": {"full_name": REPOSITORY}, "run_attempt": 1,
        "referenced_workflows": [{
            "path": f"{REPOSITORY}/.github/workflows/full-ci.yml@{CONTROL_SHA}", "sha": CONTROL_SHA,
        }],
    }


def full_jobs():
    """Create the seven full-CI jobs and the separate native required gate."""
    names = ["repository-checks", "linux-tests (release)", "linux-tests (sanitize)",
             "linux-system-qt-tests", "windows-tests", "windows-msix-tests", "required-jobs"]
    return [{"name": f"full-ci / {name}", "status": "completed", "conclusion": "success"}
            for name in names] + [
                {"name": "reviewed-ci", "status": "completed", "conclusion": "success"},
            ]


class EvidenceAPI(ci.GitHub):
    """Serve JSON-shaped GET fixtures through the production pagination code."""

    def __init__(self, pr_event=None):
        super().__init__(REPOSITORY)
        self.calls = []
        self.responses = {
            f"{ROOT}/pulls/7": deepcopy((event() if pr_event is None else pr_event)["pull_request"]),
            f"{ROOT}/actions/workflows/pr-ci.yml/runs?event=pull_request&status=success"
            f"&head_sha={SHA}&per_page=100&page=1": {"workflow_runs": [workflow_run()]},
            f"{ROOT}/actions/runs/{PRIOR_RUN}": workflow_run(),
            f"{ROOT}/actions/runs/{PRIOR_RUN}/jobs?filter=latest&per_page=100&page=1": {
                "jobs": full_jobs(),
            },
        }

    def api(self, path):
        """Return fixture data or the requested API failure."""
        self.calls.append(path)
        if path not in self.responses:
            raise AssertionError(f"Unexpected GET {path}")
        value = self.responses[path]
        if isinstance(value, Exception):
            raise value
        return deepcopy(value)


class PrepareTests(unittest.TestCase):
    def test_source_is_exact_event_head_for_same_repository_and_fork(self):
        for repository in (REPOSITORY, "contributor/project"):
            with self.subTest(repository=repository):
                payload = event(repository)
                api = EvidenceAPI(payload)
                api.responses[next(path for path in api.responses if "/workflows/" in path)] = {
                    "workflow_runs": [],
                }
                self.assertEqual(ci.prepare(payload, ENV, api), {
                    "head_sha": SHA, "head_repository": repository, "head_branch": "feature/fix",
                    "pr_number": "7", "reuse_run": "",
                })
                if repository != REPOSITORY:
                    self.assertEqual(api.calls, [f"{ROOT}/pulls/7"])

    def test_rejects_wrong_runner_event_ref_repository_or_control_revision(self):
        changes = [
            {"GITHUB_EVENT_NAME": name} for name in ("push", "pull_request_target", "workflow_dispatch")
        ] + [
            {"GITHUB_REF": "refs/heads/main"}, {"GITHUB_REF": "refs/pull/8/merge"},
            {"GITHUB_REPOSITORY": "other/project"}, {"GITHUB_REPOSITORY": "owner/../project"},
            {"CI_WORKFLOW_SHA": "main"}, {"CI_WORKFLOW_SHA": OTHER_SHA},
            {"GITHUB_RUN_ID": "0123"}, {"GITHUB_RUN_ID": "123\nreuse_run=81"},
        ]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ci.CIError):
                ci.prepare(event(), {**ENV, **change}, EvidenceAPI())

    def test_rejects_invalid_event_inputs_before_api_reads(self):
        cases = [None, {}, []] + [changed(event(), "number", value)
                                  for value in (True, 0, "07", "7;exit 0", 7.0)]
        fields = {
            "pull_request.number": [8], "pull_request.state": ["closed"],
            "pull_request.draft": [True, None],
            "pull_request.base.ref": ["release"],
            "pull_request.base.repo.full_name": ["other/project"],
            "pull_request.head.sha": ["main", SHA[:7], SHA.upper(), SHA + "\n"],
            "pull_request.head.repo.full_name": [None, "../project", "a/b/c", "a/b\nx=y"],
            "pull_request.head.ref": [None, "", "-option", "fix\nreuse_run=81", "fix space",
                                      "fix..bad", "fix.lock", ".hidden", "fix/.hidden", "fix@{1}",
                                      "fix//bad", "fix\\bad", "fix/", "fix."],
        }
        cases += [changed(event(), field, value)
                  for field, values in fields.items() for value in values]
        for payload in cases:
            api = EvidenceAPI()
            with self.subTest(payload=payload), self.assertRaises(ci.CIError):
                ci.prepare(payload, ENV, api)
            self.assertEqual(api.calls, [])

    def test_rejects_events_when_current_pr_closed_or_source_changed(self):
        for field, value in (
            ("state", "closed"), ("draft", True), ("head.sha", OTHER_SHA),
            ("head.repo.full_name", "contributor/project"), ("head.ref", "renamed"),
            ("base.ref", "release"),
        ):
            api = EvidenceAPI()
            api.responses[f"{ROOT}/pulls/7"] = changed(event()["pull_request"], field, value)
            with self.subTest(field=field), self.assertRaises(ci.CIError):
                ci.prepare(event(), ENV, api)


class ReuseTests(unittest.TestCase):
    def test_valid_candidate_on_second_page_is_found(self):
        api = EvidenceAPI()
        search = next(path for path in api.responses if "/workflows/" in path)
        rejected = [{**workflow_run(), "id": 1000 + i, "head_sha": OTHER_SHA} for i in range(100)]
        api.responses[search] = {"workflow_runs": rejected}
        for run in rejected:
            api.responses[f"{ROOT}/actions/runs/{run['id']}"] = run
        second = search.replace("&page=1", "&page=2")
        api.responses[second] = {"workflow_runs": [workflow_run()]}
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], PRIOR_RUN)
        self.assertIn(second, api.calls)

    def test_failed_duplicate_job_on_second_page_prevents_reuse(self):
        api = EvidenceAPI()
        path = next(path for path in api.responses if "/jobs?" in path)
        jobs = full_jobs()
        jobs.extend({"name": f"extra-{i}", "status": "completed", "conclusion": "success"}
                    for i in range(100 - len(jobs)))
        api.responses[path] = {"jobs": jobs}
        second = path.replace("&page=1", "&page=2")
        api.responses[second] = {"jobs": [{"name": "full-ci / windows-tests",
                                         "status": "completed", "conclusion": "failure"}]}
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")
        self.assertIn(second, api.calls)

    def test_valid_run_reuses_original_after_reading_run_and_latest_jobs(self):
        api = EvidenceAPI()
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], PRIOR_RUN)
        self.assertIn(f"{ROOT}/actions/runs/{PRIOR_RUN}", api.calls)
        self.assertTrue(any("/jobs?filter=latest" in path for path in api.calls))

    def test_run_details_override_optimistic_search_results(self):
        fields = {
            "id": [999], "event": ["push", "workflow_dispatch", "workflow_run"],
            "path": [".github/workflows/ci.yml", ".github/workflows/reviewed-ci.yml"],
            "status": ["in_progress", None], "conclusion": ["failure", "cancelled", None],
            "head_sha": [OTHER_SHA], "head_repository.full_name": ["contributor/project"],
            "head_branch": ["other", None], "repository.full_name": ["other/project"],
            "display_title": [f"PR CI #8 @ {SHA}", f"PR CI #7 @ {OTHER_SHA}", "PR CI"],
            "pull_requests": [None],
            "referenced_workflows": [None, [], [{"path": "other/full-ci.yml", "sha": CONTROL_SHA}],
                                     [{"path": f"{REPOSITORY}/{ci.FULL_WORKFLOW}", "sha": CONTROL_SHA}],
                                     [{"path": f"{REPOSITORY}/{ci.FULL_WORKFLOW}@", "sha": CONTROL_SHA}],
                                     [{"path": f"{REPOSITORY}/{ci.FULL_WORKFLOW}@main",
                                       "sha": OTHER_SHA}],
                                     [{"path": f"other/project/{ci.FULL_WORKFLOW}@main",
                                       "sha": CONTROL_SHA}],
                                     workflow_run()["referenced_workflows"] * 2],
        }
        for field, values in fields.items():
            for value in values:
                api = EvidenceAPI()
                api.responses[f"{ROOT}/actions/runs/{PRIOR_RUN}"] = changed(workflow_run(), field, value)
                with self.subTest(field=field, value=value):
                    self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")

    def test_current_run_is_excluded_without_fetching_its_details(self):
        api = EvidenceAPI()
        search = next(path for path in api.responses if "/workflows/" in path)
        api.responses[search] = {"workflow_runs": [changed(workflow_run(), "id", int(RUN_ID))]}
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")
        self.assertFalse(any(f"/runs/{RUN_ID}" in path for path in api.calls))

    def test_every_required_job_and_native_gate_must_succeed_exactly_once(self):
        for index in range(8):
            original = full_jobs()
            variants = [original[:index] + original[index + 1:], original + [original[index]]]
            variants += [original[:index] + [{**original[index], "conclusion": result}]
                         + original[index + 1:] for result in ("failure", "cancelled", "skipped", None)]
            variants.append(original[:index] + [{**original[index], "status": "in_progress"}]
                            + original[index + 1:])
            for jobs in variants:
                with self.subTest(index=index, jobs=jobs):
                    self.assertFalse(ci.successful_jobs(jobs))
        self.assertFalse(ci.successful_jobs(full_jobs()[:-1] + [
            {**full_jobs()[-1], "name": "build / reviewed-ci"},
        ]))
        self.assertFalse(ci.successful_jobs([{**job, "name": job["name"].rsplit(" / ", 1)[-1]}
                                             for job in full_jobs()]))

    def test_failed_missing_or_ambiguous_jobs_build_fresh(self):
        for jobs in (full_jobs()[:-1], [*full_jobs(), full_jobs()[0]],
                     [full_jobs()[-1], {"name": "full-ci", "status": "completed", "conclusion": "skipped"}],
                     [{**job, "conclusion": "skipped"} for job in full_jobs()]):
            api = EvidenceAPI()
            path = next(path for path in api.responses if "/jobs?" in path)
            api.responses[path] = {"jobs": jobs}
            with self.subTest(jobs=jobs):
                self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")

    def test_api_errors_at_each_stage_build_fresh(self):
        for path in EvidenceAPI().responses:
            api = EvidenceAPI()
            api.responses[path] = ci.CIError("API unavailable")
            with self.subTest(path=path):
                self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")

    def test_missing_reference_or_malformed_pagination_builds_fresh(self):
        api = EvidenceAPI()
        del api.responses[f"{ROOT}/actions/runs/{PRIOR_RUN}"]["referenced_workflows"]
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")
        api = EvidenceAPI()
        search = next(path for path in api.responses if "/workflows/" in path)
        api.responses[search] = {"workflow_runs": None}
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")

    def test_incomplete_current_pr_evidence_rejects_preparation(self):
        for response in (None, {}, changed(event()["pull_request"], "head.repo", None)):
            api = EvidenceAPI()
            api.responses[f"{ROOT}/pulls/7"] = response
            with self.subTest(response=response):
                with self.assertRaises(ci.CIError):
                    ci.prepare(event(), ENV, api)
                self.assertEqual(api.calls, [f"{ROOT}/pulls/7"])

    def test_rerun_starting_during_jobs_read_builds_fresh(self):
        api = EvidenceAPI()
        original_read = api.api
        metadata_reads = 0

        def read(path):
            """Simulate an external rerun after the job evidence was read."""
            nonlocal metadata_reads
            response = original_read(path)
            if path == f"{ROOT}/actions/runs/{PRIOR_RUN}":
                metadata_reads += 1
                if metadata_reads == 2:
                    response.update(status="in_progress", conclusion=None, run_attempt=2)
            return response

        api.api = read
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")

    def test_linked_pull_request_identity_must_match_when_present(self):
        pr = {
            "number": 7,
            "head": {"sha": SHA, "ref": "feature/fix", "repo": {
                "url": f"https://api.github.com/repos/{REPOSITORY}",
            }},
            "base": {"ref": "main", "repo": {
                "url": f"https://api.github.com/repos/{REPOSITORY}",
            }},
        }
        api = EvidenceAPI()
        run_path = f"{ROOT}/actions/runs/{PRIOR_RUN}"
        api.responses[run_path]["pull_requests"] = [pr]
        self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], PRIOR_RUN)
        invalid = [[pr, pr], [{}]]
        invalid += [[changed(pr, field, value)] for field, value in (
            ("number", 8), ("head.sha", OTHER_SHA), ("head.ref", "renamed"),
            ("head.repo.url", "https://api.github.com/repos/contributor/project"),
            ("base.repo.url", "https://api.github.com/repos/other/project"), ("base.ref", "release"),
        )]
        for links in invalid:
            api.responses[run_path]["pull_requests"] = links
            with self.subTest(links=links):
                self.assertEqual(ci.prepare(event(), ENV, api)["reuse_run"], "")


class VerifyHeadTests(unittest.TestCase):
    def test_current_open_ready_head_requires_only_one_pr_read(self):
        api = EvidenceAPI()
        self.assertEqual(ci.verify_head(event(), ENV, api), {
            "head_sha": SHA, "head_repository": REPOSITORY,
            "head_branch": "feature/fix", "pr_number": "7",
        })
        self.assertEqual(api.calls, [f"{ROOT}/pulls/7"])

    def test_stale_closed_draft_or_unavailable_head_cannot_pass(self):
        for response in (changed(event()["pull_request"], "head.sha", OTHER_SHA),
                         changed(event()["pull_request"], "state", "closed"),
                         changed(event()["pull_request"], "draft", True),
                         changed(event()["pull_request"], "base.ref", "release"),
                         {}, ci.CIError("API unavailable")):
            api = EvidenceAPI()
            api.responses[f"{ROOT}/pulls/7"] = response
            with self.subTest(response=response), self.assertRaises(ci.CIError):
                ci.verify_head(event(), ENV, api)


def diagnostic_api():
    """Create a failed main producer with verified Windows/MSI tests and its payload."""
    api = EvidenceAPI()
    run_path = f"{ROOT}/actions/runs/{PRIOR_RUN}"
    run = {**workflow_run(), "event": "push", "path": ".github/workflows/ci.yml",
           "head_branch": "main", "conclusion": "failure", "pull_requests": []}
    windows = {"name": "full-ci / windows-tests", "status": "completed", "conclusion": "success",
               "steps": [{"name": name, "status": "completed", "conclusion": "success"}
                         for name in ("Build", "Test", "Stage app and build MSI",
                                      "Verify MSI install, upgrade, repair, and uninstall",
                                      "Verify MSI wizard and launch after Finish",
                                      "Build unsigned MSIX", "Upload Windows verification artifacts")]}
    api.responses = {
        run_path: run,
        f"{run_path}/jobs?filter=latest&per_page=100&page=1": {"jobs": [windows, {
            "name": "full-ci / windows-msix-tests", "status": "completed", "conclusion": "failure",
        }]},
        f"{run_path}/artifacts?per_page=100&page=1": {"artifacts": [{
            "id": 200, "name": "windows-verification", "expired": False,
            "size_in_bytes": 12345, "expires_at": "2099-01-01T00:00:00Z",
        }]},
    }
    return api


DIAGNOSTIC_ENV = {**ENV, "GITHUB_EVENT_NAME": "workflow_dispatch",
                  "GITHUB_REF": "refs/heads/main", "DIAGNOSTIC_RUN_ID": PRIOR_RUN}


class DiagnosticTests(unittest.TestCase):
    def test_producer_must_still_match_after_payload_checks(self):
        for change in ({"run_attempt": 2}, {"status": "in_progress"}, ci.CIError("unavailable")):
            api = diagnostic_api()
            original_read = api.api
            reads = 0

            def read(path):
                """Alter only the final producer observation."""
                nonlocal reads
                result = original_read(path)
                if path == f"{ROOT}/actions/runs/{PRIOR_RUN}":
                    reads += 1
                    if reads == 2:
                        if isinstance(change, Exception):
                            raise change
                        result.update(change)
                return result

            api.api = read
            with self.subTest(change=change), self.assertRaises(ci.CIError):
                ci.diagnostic(DIAGNOSTIC_ENV, api)
            self.assertEqual(reads, 2)

    def test_successful_step_conclusion_requires_completed_status(self):
        for status in (None, "in_progress", "queued"):
            api = diagnostic_api()
            path = next(path for path in api.responses if "/jobs?" in path)
            step = api.responses[path]["jobs"][0]["steps"][0]
            if status is None:
                del step["status"]
            else:
                step["status"] = status
            with self.subTest(status=status), self.assertRaises(ci.CIError):
                ci.diagnostic(DIAGNOSTIC_ENV, api)

    def test_failed_overall_and_msix_results_allow_verified_windows_payload(self):
        ci.diagnostic(DIAGNOSTIC_ENV, diagnostic_api())

    def test_msix_packaging_failure_keeps_valid_verified_payload_eligible(self):
        api = diagnostic_api()
        path = next(path for path in api.responses if "/jobs?" in path)
        windows = api.responses[path]["jobs"][0]
        windows["conclusion"] = "failure"
        for step in windows["steps"]:
            if step["name"] == "Build unsigned MSIX":
                step["conclusion"] = "failure"
        ci.diagnostic(DIAGNOSTIC_ENV, api)

    def test_only_main_dispatch_accepts_positive_run_identifiers(self):
        for change in ({"DIAGNOSTIC_RUN_ID": "0"}, {"DIAGNOSTIC_RUN_ID": "081"},
                       {"DIAGNOSTIC_RUN_ID": "81/../2"}, {"DIAGNOSTIC_RUN_ID": ""},
                       {"GITHUB_EVENT_NAME": "pull_request"}, {"GITHUB_EVENT_NAME": "push"},
                       {"GITHUB_REF": "refs/heads/feature"}):
            with self.subTest(change=change), self.assertRaises(ci.CIError):
                ci.diagnostic({**DIAGNOSTIC_ENV, **change}, diagnostic_api())

    def test_rejects_pr_fork_diagnostic_unrelated_or_unfinished_producers(self):
        for field, values in {
            "id": [999], "status": ["in_progress"],
            "event": ["pull_request", "workflow_dispatch", "workflow_run"],
            "path": [ci.WORKFLOW, ".github/workflows/release.yml"], "head_branch": ["feature", None],
            "head_repository.full_name": ["contributor/project"],
            "repository.full_name": ["other/project"], "head_sha": ["main", None],
        }.items():
            for value in values:
                api = diagnostic_api()
                path = f"{ROOT}/actions/runs/{PRIOR_RUN}"
                api.responses[path] = changed(api.responses[path], field, value)
                with self.subTest(field=field, value=value), self.assertRaises(ci.CIError):
                    ci.diagnostic(DIAGNOSTIC_ENV, api)

    def test_windows_build_test_msi_and_upload_steps_must_succeed(self):
        original = diagnostic_api()
        path = next(path for path in original.responses if "/jobs?" in path)
        windows = original.responses[path]["jobs"][0]
        variants = [[], [windows, windows], [{**windows, "name": "windows-tests"}],
                    [{**windows, "status": "in_progress"}]]
        variants += [[{**windows, "steps": steps}] for steps in (
            [], windows["steps"][:-1], windows["steps"] + [windows["steps"][0]],
            [{**step, "conclusion": "skipped"} for step in windows["steps"]],
        )]
        for name in ("Build", "Test", "Stage app and build MSI",
                     "Verify MSI install, upgrade, repair, and uninstall",
                     "Verify MSI wizard and launch after Finish", "Upload Windows verification artifacts"):
            for result in ("failure", "cancelled", "skipped", None):
                steps = [{**step, "conclusion": result} if step["name"] == name else step
                         for step in windows["steps"]]
                variants.append([{**windows, "steps": steps}])
        for jobs in variants:
            api = diagnostic_api()
            api.responses[path] = {"jobs": jobs}
            with self.subTest(jobs=jobs), self.assertRaises(ci.CIError):
                ci.diagnostic(DIAGNOSTIC_ENV, api)

    def test_artifact_must_be_one_retained_nonempty_unexpired_payload(self):
        original = diagnostic_api()
        path = next(path for path in original.responses if "/artifacts?" in path)
        artifact = original.responses[path]["artifacts"][0]
        variants = [[], [artifact, artifact], [{**artifact, "name": "unrelated"}]]
        variants += [[changed(artifact, field, value)] for field, values in {
            "expired": [True, None], "size_in_bytes": [0, -1, True, "123"], "id": [0],
            "expires_at": ["2000-01-01T00:00:00Z", "2099-01-01", "bad", None],
        }.items() for value in values]
        for artifacts in variants:
            api = diagnostic_api()
            api.responses[path] = {"artifacts": artifacts}
            with self.subTest(artifacts=artifacts), self.assertRaises(ci.CIError):
                ci.diagnostic(DIAGNOSTIC_ENV, api)

    def test_missing_or_unavailable_producer_evidence_fails(self):
        for path in diagnostic_api().responses:
            for response in ({}, ci.CIError("API unavailable")):
                api = diagnostic_api()
                api.responses[path] = response
                with self.subTest(path=path, response=response), self.assertRaises(ci.CIError):
                    ci.diagnostic(DIAGNOSTIC_ENV, api)


class AggregateTests(unittest.TestCase):
    def test_normal_full_build_requires_all_five_aggregates(self):
        results = {name: "success" for name in EXPECTED_RESULT_VARIABLES}
        ci.check_results({**results, "CI_DIAGNOSTIC": "false"})
        for name in EXPECTED_RESULT_VARIABLES:
            for result in ("failure", "cancelled", "skipped", "", None):
                with self.subTest(name=name, result=result), self.assertRaises(ci.CIError):
                    ci.check_results({**results, name: result, "CI_DIAGNOSTIC": "false"})
            missing = {key: value for key, value in results.items() if key != name}
            with self.subTest(missing=name), self.assertRaises(ci.CIError):
                ci.check_results({**missing, "CI_DIAGNOSTIC": "false"})

    def test_diagnostic_requires_repository_and_msix_success_and_only_expected_skips(self):
        diagnostic = dict(zip(EXPECTED_RESULT_VARIABLES, ("success", "skipped", "skipped", "skipped", "success")))
        ci.check_results({**diagnostic, "CI_DIAGNOSTIC": "true"})
        with self.assertRaises(ci.CIError):
            ci.check_results({**diagnostic, "CI_DIAGNOSTIC": "false"})
        for name in EXPECTED_RESULT_VARIABLES:
            for result in ("failure", "cancelled", "", None,
                           "skipped" if diagnostic[name] == "success" else "success"):
                with self.subTest(name=name, result=result), self.assertRaises(ci.CIError):
                    ci.check_results({**diagnostic, name: result, "CI_DIAGNOSTIC": "true"})
        jobs = [job for job in full_jobs() if job["name"].rsplit(" / ", 1)[-1] in (
            "repository-checks", "windows-msix-tests", "required-jobs", "reviewed-ci",
        )]
        self.assertFalse(ci.successful_jobs(jobs))

    def test_diagnostic_flag_must_be_explicit(self):
        for flag in (None, "", "TRUE", "1"):
            with self.subTest(flag=flag), self.assertRaises(ci.CIError):
                ci.check_results({**dict.fromkeys(EXPECTED_RESULT_VARIABLES, "success"), "CI_DIAGNOSTIC": flag})


class TransportTests(unittest.TestCase):
    def test_failed_gets_are_bounded_and_do_not_expose_transport_output(self):
        for failure in (OSError("private diagnostic"),
                        subprocess.TimeoutExpired("gh", 30), None):
            with self.subTest(failure=failure), patch.object(ci.subprocess, "run") as run:
                run.side_effect = failure
                run.return_value = subprocess.CompletedProcess([], 1, stdout="", stderr="private diagnostic")
                with self.assertRaises(ci.CIError) as caught:
                    ci.GitHub(REPOSITORY).api(f"{ROOT}/pulls/7")
                self.assertNotIn("private diagnostic", str(caught.exception))
                self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_malformed_get_json_is_rejected(self):
        with patch.object(ci.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout="private non-JSON response"
        )), self.assertRaises(ci.CIError) as caught:
            ci.GitHub(REPOSITORY).api(f"{ROOT}/pulls/7")
        self.assertNotIn("private non-JSON", str(caught.exception))


class CLITests(unittest.TestCase):
    def setUp(self):
        temporary_root = Path(os.environ.get("REVIEWED_CI_TEST_TEMP", Path(__file__).resolve().parents[1] / "dev-temp"))
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="reviewed-ci-", dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = self.root / "event.json"
        self.output = self.root / "output"
        self.summary = self.root / "summary"
        self.fixtures = self.root / "responses.json"
        self.payload.write_text(json.dumps(event()))
        self.fixtures.write_text(json.dumps(EvidenceAPI().responses))
        gh = self.root / "gh"
        gh.write_text(f"#!{sys.executable}\n" + '''
"""Serve read-only GitHub CLI fixtures for subprocess integration tests."""
import json
import os
from pathlib import Path
import sys

if len(sys.argv) != 5 or sys.argv[1] != "api" or sys.argv[3:] != ["--method", "GET"]:
    sys.exit(2)
responses = json.loads(Path(os.environ["GH_FIXTURES"]).read_text())
response = responses.get(sys.argv[2])
if response is None:
    sys.exit(1)
print(json.dumps(response))
''')
        gh.chmod(0o755)
        self.environment = {
            **os.environ, **ENV, "PATH": str(self.root) + os.pathsep + os.environ.get("PATH", ""),
            "GH_FIXTURES": str(self.fixtures), "GITHUB_EVENT_PATH": str(self.payload),
            "GITHUB_OUTPUT": str(self.output), "GITHUB_STEP_SUMMARY": str(self.summary),
        }

    def run_cli(self, command="prepare", **environment):
        """Execute the actual CLI with event files and a separate gh process."""
        return subprocess.run(
            [sys.executable, "-B", str(Path(ci.__file__).resolve()), command],
            env={**self.environment, **environment}, text=True, capture_output=True, check=False,
        )

    def test_prepare_emits_exact_outputs_and_original_run_summary_link(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), f"head_sha={SHA}\nhead_repository={REPOSITORY}\n"
                         f"head_branch=feature/fix\npr_number=7\nreuse_run={PRIOR_RUN}\n")
        self.assertIn(f"https://github.com/{REPOSITORY}/actions/runs/{PRIOR_RUN}", self.summary.read_text())

    def test_fork_cli_builds_event_head_fresh_and_summary_is_optional(self):
        self.payload.write_text(json.dumps(event("contributor/project")))
        self.fixtures.write_text(json.dumps(EvidenceAPI(event("contributor/project")).responses))
        result = self.run_cli(GITHUB_STEP_SUMMARY="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), f"head_sha={SHA}\nhead_repository=contributor/project\n"
                         "head_branch=feature/fix\npr_number=7\nreuse_run=\n")
        self.assertFalse(self.summary.exists())

    def test_invalid_event_or_output_path_fails_without_emitting_outputs(self):
        for environment in ({"GITHUB_REF": "refs/heads/main"}, {"GITHUB_OUTPUT": "relative"},
                            {"GITHUB_OUTPUT": str(self.output) + "\ninjected=value"}):
            result = self.run_cli(**environment)
            with self.subTest(environment=environment):
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())
        self.payload.write_text("not json")
        self.assertNotEqual(self.run_cli().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_cli_api_failure_emits_fresh_build_outputs(self):
        self.fixtures.write_text("{}")
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), f"head_sha={SHA}\nhead_repository={REPOSITORY}\n"
                         "head_branch=feature/fix\npr_number=7\nreuse_run=\n")

    def test_verify_head_cli_validates_current_head_without_any_output_files(self):
        self.fixtures.write_text(json.dumps({f"{ROOT}/pulls/7": event()["pull_request"]}))
        result = self.run_cli("verify-head", GITHUB_OUTPUT="", GITHUB_STEP_SUMMARY="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.summary.exists())
        self.fixtures.write_text(json.dumps({f"{ROOT}/pulls/7": changed(event()["pull_request"], "draft", True)}))
        result = self.run_cli("verify-head", GITHUB_OUTPUT="", GITHUB_STEP_SUMMARY="")
        self.assertNotEqual(result.returncode, 0)

    def test_diagnostic_cli_validates_payload_without_event_or_output_files(self):
        self.fixtures.write_text(json.dumps(diagnostic_api().responses))
        environment = {**DIAGNOSTIC_ENV, "GITHUB_EVENT_PATH": "", "GITHUB_OUTPUT": "",
                       "GITHUB_STEP_SUMMARY": ""}
        result = self.run_cli("diagnostic", **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.summary.exists())
        self.assertNotEqual(self.run_cli("diagnostic", **{**environment, "DIAGNOSTIC_RUN_ID": "0"}).returncode, 0)

    def test_check_results_executes_without_event_or_api_dependencies(self):
        results = dict.fromkeys(EXPECTED_RESULT_VARIABLES, "success")
        result = self.run_cli("check-results", **results, CI_DIAGNOSTIC="false", GITHUB_EVENT_PATH="")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cli("check-results", **{**results, "CI_LINUX_RESULT": "cancelled"}, CI_DIAGNOSTIC="false")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CI_LINUX_RESULT", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
