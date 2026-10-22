#!/usr/bin/env python3
"""Cancel obsolete PR CI runs using trusted controls and current GitHub state.

Run only from a default-branch checkout for pull_request_target lifecycle events
or workflow_run in_progress notifications, including reruns.
The only write this command permits is cancellation of an identified PR CI run.
It never reads PR files, downloads artifacts, or publishes commit statuses.
"""

import argparse
import json
import os
import re
import subprocess
import sys

from reviewed_ci import CIError, GitHub, WORKFLOW, runner_file, validate_number, validate_sha


ACTIONS = {"synchronize", "closed", "converted_to_draft", "reopened", "ready_for_review"}
ACTIVE_STATUSES = ("queued", "in_progress", "waiting", "pending", "requested")
RUN_TITLE = re.compile(r"PR CI #([1-9][0-9]*) @ ([0-9a-f]{40})")


class CancellationGitHub(GitHub):
    """Reuse the read-only API client and permit only the run-cancel endpoint."""

    def cancel(self, identifier):
        """Request normal cancellation of one validated run identifier."""
        identifier = validate_number(identifier)
        path = f"{self.root}/actions/runs/{identifier}/cancel"
        try:
            result = subprocess.run(
                ["gh", "api", path, "--method", "POST"],
                text=True, capture_output=True, check=False, timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CIError(f"Cancellation request failed for run {identifier}") from error
        if result.returncode:
            raise CIError(f"Cancellation request failed for run {identifier}")


def event_number(event, environment, repository):
    """Validate the lifecycle event's identity without using its stale PR state."""
    if environment.get("GITHUB_EVENT_NAME") != "pull_request_target":
        raise CIError("Cancellation requires a pull_request_target event")
    if environment.get("GITHUB_REPOSITORY") != repository:
        raise CIError("Cancellation client does not match the runner repository")
    try:
        number = validate_number(event["number"])
        if (event["action"] not in ACTIONS
                or event["repository"]["full_name"] != repository
                or validate_number(event["pull_request"]["number"]) != number
                or event["pull_request"]["base"]["repo"]["full_name"] != repository):
            raise CIError("Unexpected pull request lifecycle event")
    except (KeyError, TypeError) as error:
        raise CIError("Incomplete pull request lifecycle event") from error
    return number


def current_pr(api, number):
    """Read the live PR head and state, refusing incomplete or unrelated evidence."""
    pr = api.api(f"{api.root}/pulls/{number}")
    try:
        if (validate_number(pr["number"]) != number
                or pr["base"]["repo"]["full_name"] != api.repository
                or pr["state"] not in ("open", "closed")
                or not isinstance(pr["draft"], bool)):
            raise CIError("Invalid current pull request state")
        validate_sha(pr["head"]["sha"])
    except (KeyError, TypeError) as error:
        raise CIError("Incomplete current pull request state") from error
    return pr


def active_runs(api):
    """Collect every active page before cancellation can change pagination."""
    runs = {}
    for status in ACTIVE_STATUSES:
        path = (f"{api.root}/actions/workflows/pr-ci.yml/runs"
                f"?event=pull_request&status={status}")
        for page in range(1, 11):
            response = api.api(f"{path}&per_page=100&page={page}")
            batch = response.get("workflow_runs") if isinstance(response, dict) else None
            count = response.get("total_count") if isinstance(response, dict) else None
            if (not isinstance(batch, list) or len(batch) > 100
                    or not all(isinstance(run, dict) for run in batch)
                    or isinstance(count, bool) or not isinstance(count, int) or count < 0):
                raise CIError("GitHub returned malformed active-run pagination")
            # GitHub caps filtered workflow-run searches at 1,000 results.
            if count > 1000:
                raise CIError("Active-run search exceeds GitHub's 1,000-result limit")
            for run in batch:
                identifier = validate_number(run.get("id"))
                runs[identifier] = run
            if len(batch) < 100 or page == 10:
                break
    return list(runs.values())


def identified_run(run, number, repository):
    """Require immutable run identity and validate matching live PR associations."""
    if not isinstance(run, dict):
        return False
    try:
        title = run.get("display_title")
        match = RUN_TITLE.fullmatch(title) if isinstance(title, str) else None
        if (not match or match.group(1) != number or match.group(2) != run["head_sha"]
                or run["path"] != WORKFLOW or run["name"] != "PR CI"
                or run["event"] != "pull_request" or run["status"] not in ACTIVE_STATUSES
                or run["repository"]["full_name"] != repository):
            return False
        validate_number(run["id"])
        references = run.get("pull_requests")
        if references is not None:
            if not isinstance(references, list):
                return False
            # These are matching open PRs, not the PR that triggered the run.
            # Closing the triggering PR can remove its entry from this list.
            for pr in references:
                if validate_number(pr["number"]) != number:
                    continue
                base_repository = pr.get("base", {}).get("repo")
                if (base_repository is not None
                        and (not isinstance(base_repository, dict)
                             or base_repository.get("full_name", repository) != repository)):
                    return False
    except (CIError, KeyError, TypeError, AttributeError):
        return False
    return True


def cancel_runs(api, number, runs):
    """Recheck run metadata and current PR state before each cancellation."""
    cancelled = []
    for candidate in runs:
        if not identified_run(candidate, number, api.repository):
            continue
        identifier = validate_number(candidate["id"])
        run = api.api(f"{api.root}/actions/runs/{identifier}")
        if (not identified_run(run, number, api.repository)
                or validate_number(run["id"]) != identifier):
            continue
        # Refresh after listing and run inspection, including for delayed events.
        pr = current_pr(api, number)
        if pr["state"] == "open" and not pr["draft"] and run["head_sha"] == pr["head"]["sha"]:
            continue
        try:
            api.cancel(identifier)
        except CIError as error:
            observed = api.api(f"{api.root}/actions/runs/{identifier}")
            if (not isinstance(observed, dict) or str(observed.get("id")) != identifier
                    or observed.get("status") != "completed"):
                raise CIError(f"Failed to cancel active PR CI run {identifier}") from error
            print(f"PR CI run {identifier} already completed.")
            continue
        cancelled.append(identifier)
        print(f"Requested cancellation of PR CI run {identifier}.")
    return cancelled


def reconcile_started_run(event, environment, api):
    """Reconcile the actual starting attempt without depending on prepare reruns."""
    if environment.get("GITHUB_REPOSITORY") != api.repository:
        raise CIError("Cancellation client does not match the runner repository")
    try:
        if (event["action"] != "in_progress"
                or event["repository"]["full_name"] != api.repository):
            raise CIError("Unexpected workflow_run notification")
        identifier = validate_number(event["workflow_run"]["id"])
    except (KeyError, TypeError) as error:
        raise CIError("Incomplete workflow_run notification") from error
    # The notification selects an API run, not a branch or a PR payload entry.
    run = api.api(f"{api.root}/actions/runs/{identifier}")
    title = run.get("display_title") if isinstance(run, dict) else None
    match = RUN_TITLE.fullmatch(title) if isinstance(title, str) else None
    if (not match or not identified_run(run, match.group(1), api.repository)
            or validate_number(run["id"]) != identifier):
        return []
    return cancel_runs(api, match.group(1), [run])


def cancel_obsolete(event, environment, api):
    """Reconcile lifecycle changes or a starting attempt using current PR state."""
    if environment.get("GITHUB_EVENT_NAME") == "workflow_run":
        return reconcile_started_run(event, environment, api)
    number = event_number(event, environment, api.repository)
    current_pr(api, number)
    return cancel_runs(api, number, active_runs(api))


def main():
    """Read a runner event and cancel obsolete runs without executing PR content."""
    argparse.ArgumentParser(description=__doc__).parse_args()
    event_path = runner_file(os.environ, "GITHUB_EVENT_PATH")
    event = json.loads(event_path.read_text())
    cancel_obsolete(event, os.environ, CancellationGitHub(os.environ.get("GITHUB_REPOSITORY")))


if __name__ == "__main__":
    try:
        main()
    except (CIError, OSError, ValueError) as error:
        print(f"PR CI cancellation failed. {error}", file=sys.stderr)
        sys.exit(1)
