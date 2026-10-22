#!/usr/bin/env python3
"""Select immutable PR sources and verify full CI results using read-only APIs."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys


WORKFLOW = ".github/workflows/pr-ci.yml"
FULL_WORKFLOW = ".github/workflows/full-ci.yml"
REQUIRED_JOBS = {
    "repository-checks", "linux-tests (release)", "linux-tests (sanitize)",
    "linux-system-qt-tests", "windows-tests", "windows-msix-tests", "required-jobs",
}
RESULT_VARIABLES = (
    "CI_REPOSITORY_RESULT", "CI_LINUX_RESULT", "CI_LINUX_SYSTEM_RESULT",
    "CI_WINDOWS_RESULT", "CI_MSIX_RESULT",
)
MSI_STEPS = {
    "Stage app and build MSI", "Verify MSI install, upgrade, repair, and uninstall",
    "Verify MSI wizard and launch after Finish",
}


class CIError(RuntimeError):
    """The event or job results do not satisfy the CI contract."""


def validate_repository(value):
    """Require a repository name safe for API paths and workflow outputs."""
    if (not isinstance(value, str)
            or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value)
            or any(part in (".", "..") for part in value.split("/"))):
        raise CIError("Invalid repository name")
    return value


def validate_sha(value):
    """Require a full immutable lowercase commit SHA."""
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise CIError("Expected a full lowercase commit SHA")
    return value


def validate_number(value):
    """Require a positive decimal identifier without aliases or leading zeros."""
    if isinstance(value, bool) or not re.fullmatch(r"[1-9][0-9]*", str(value)):
        raise CIError("Expected a positive decimal identifier")
    return str(value)


def validate_branch(value):
    """Accept branch names without Git ref hazards or output control characters."""
    if (not isinstance(value, str) or not value or value == "@"
            or value.startswith(("-", "/")) or value.endswith(("/", "."))
            or any(part in value for part in ("..", "//", "@{"))
            or re.search(r"[\x00-\x20\x7f~^:?*\[\\]", value)
            or any(part.startswith(".") or part.endswith(".lock") for part in value.split("/"))):
        raise CIError("Invalid PR head branch")
    return value


class GitHub:
    """Read Actions and pull request evidence through the runner's gh token."""

    def __init__(self, repository):
        self.repository = validate_repository(repository)
        self.root = f"repos/{repository}"

    def api(self, path):
        """Read one JSON response without permitting API writes."""
        try:
            result = subprocess.run(
                ["gh", "api", path, "--method", "GET"],
                text=True, capture_output=True, check=False, timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CIError("GitHub evidence is unavailable") from error
        if result.returncode:
            raise CIError(f"GitHub request failed for {path}")
        try:
            return json.loads(result.stdout)
        except ValueError as error:
            raise CIError("GitHub returned invalid JSON evidence") from error

    def pages(self, path, key):
        """Read every page, refusing malformed or incomplete evidence."""
        items = []
        separator = "&" if "?" in path else "?"
        for page in range(1, 101):
            response = self.api(f"{path}{separator}per_page=100&page={page}")
            batch = response.get(key) if isinstance(response, dict) else None
            if not isinstance(batch, list) or not all(isinstance(item, dict) for item in batch):
                raise CIError("GitHub returned malformed paginated evidence")
            items.extend(batch)
            if len(batch) < 100:
                return items
        raise CIError("GitHub pagination limit reached")


def select_source(event, environment):
    """Select the event's PR head, never the synthetic merge commit in GITHUB_SHA."""
    if environment.get("GITHUB_EVENT_NAME") != "pull_request":
        raise CIError("prepare requires a pull_request event")
    repository = validate_repository(environment.get("GITHUB_REPOSITORY"))
    try:
        number = validate_number(event["number"])
        pr = event["pull_request"]
        if (event["repository"]["full_name"] != repository
                or pr["base"]["repo"]["full_name"] != repository
                or pr["base"]["ref"] != "main" or pr["state"] != "open" or pr["draft"] is not False
                or ("number" in pr and validate_number(pr["number"]) != number)):
            raise CIError("Expected an open, ready pull request targeting this repository's main")
        source = {
            "head_sha": validate_sha(pr["head"]["sha"]),
            "head_repository": validate_repository(pr["head"]["repo"]["full_name"]),
            "head_branch": validate_branch(pr["head"]["ref"]),
            "pr_number": number,
        }
    except (KeyError, TypeError) as error:
        raise CIError("Incomplete pull request event") from error
    if environment.get("GITHUB_REF") != f"refs/pull/{number}/merge":
        raise CIError("Runner ref does not match the pull request event")
    validate_sha(environment.get("CI_WORKFLOW_SHA"))
    if (environment.get("GITHUB_WORKFLOW_SHA") is not None
            and environment["GITHUB_WORKFLOW_SHA"] != environment["CI_WORKFLOW_SHA"]):
        raise CIError("CI_WORKFLOW_SHA must be github.workflow_sha")
    validate_number(environment.get("GITHUB_RUN_ID"))
    return source


def reusable_run(run, source, repository, workflow_sha, current_run):
    """Check the actual run and exact reusable workflow revision, not search filters."""
    if not isinstance(run, dict):
        return False
    try:
        if (validate_number(run["id"]) == current_run
                or run["event"] != "pull_request" or run["path"] != WORKFLOW
                or run["status"] != "completed" or run["conclusion"] != "success"
                or run["repository"]["full_name"] != repository
                or run["display_title"] != f"PR CI #{source['pr_number']} @ {source['head_sha']}"
                or run["head_sha"] != source["head_sha"]
                or run["head_repository"]["full_name"] != source["head_repository"]
                or run["head_branch"] != source["head_branch"]):
            return False
        pull_requests = run.get("pull_requests", [])
        if not isinstance(pull_requests, list):
            return False
        if pull_requests:
            if len(pull_requests) != 1:
                return False
            pr = pull_requests[0]
            if (validate_number(pr["number"]) != source["pr_number"]
                    or pr["head"]["sha"] != source["head_sha"]
                    or pr["head"]["ref"] != source["head_branch"] or pr["base"]["ref"] != "main"):
                return False
            for side, expected in (("head", source["head_repository"]), ("base", repository)):
                repo = pr[side]["repo"]
                if (repo["url"] != f"https://api.github.com/repos/{expected}"
                        or repo.get("full_name", expected) != expected):
                    return False
        references = run["referenced_workflows"]
        if not isinstance(references, list) or not all(isinstance(item, dict) for item in references):
            return False
        full_ci = [item for item in references if isinstance(item.get("path"), str)
                   and item["path"].startswith(f"{repository}/{FULL_WORKFLOW}@")
                   and item["path"].rsplit("@", 1)[-1]]
        return len(full_ci) == 1 and full_ci[0].get("sha") == workflow_sha
    except (CIError, KeyError, TypeError):
        return False


def successful_jobs(jobs):
    """Require every full-build job and the native gate once in the latest jobs."""
    required = {f"full-ci / {name}" for name in REQUIRED_JOBS} | {"reviewed-ci"}
    found = set()
    for job in jobs:
        if not isinstance(job, dict) or not isinstance(job.get("name"), str):
            return False
        name = job["name"].rsplit(" / ", 1)[-1]
        if name not in REQUIRED_JOBS | {"reviewed-ci"}:
            continue
        if (job["name"] in found or job["name"] not in required
                or job.get("status") != "completed" or job.get("conclusion") != "success"):
            return False
        found.add(job["name"])
    return found == required


def find_reuse(api, source, workflow_sha, current_run):
    """Reuse only proven same-repository PR builds, falling back to a fresh build."""
    if source["head_repository"] != api.repository:
        return ""
    try:
        runs = api.pages(f"{api.root}/actions/workflows/pr-ci.yml/runs"
                         f"?event=pull_request&status=success&head_sha={source['head_sha']}",
                         "workflow_runs")
        for candidate in runs:
            identifier = validate_number(candidate["id"])
            if identifier == current_run:
                continue
            run = api.api(f"{api.root}/actions/runs/{identifier}")
            if (str(run.get("id")) != identifier
                    or not reusable_run(run, source, api.repository, workflow_sha, current_run)):
                continue
            jobs = api.pages(f"{api.root}/actions/runs/{identifier}/jobs?filter=latest", "jobs")
            # A rerun starting during pagination must not reuse the previous attempt.
            if successful_jobs(jobs) and api.api(f"{api.root}/actions/runs/{identifier}") == run:
                return identifier
    except (CIError, KeyError, TypeError, AttributeError):
        print("CI reuse evidence is unavailable. Building fresh.", file=sys.stderr)
    return ""


def validate_current_head(event, environment, source, current):
    """Reject a PR that changed, closed, or became a draft after the event."""
    current_event = {"number": event["number"], "repository": event["repository"],
                     "pull_request": current}
    if select_source(current_event, environment) != source:
        raise CIError("Pull request head changed since this event")


def verify_head(event, environment, api):
    """Require the event head to remain the current open, ready PR on main."""
    source = select_source(event, environment)
    validate_current_head(event, environment, source, api.api(f"{api.root}/pulls/{source['pr_number']}"))
    return source


def prepare(event, environment, api):
    """Prepare immutable checkout outputs, rejecting a known stale or unready PR."""
    source = select_source(event, environment)
    try:
        current = api.api(f"{api.root}/pulls/{source['pr_number']}")
    except CIError:
        # A fresh immutable build remains safe when optional API evidence is unavailable.
        return {**source, "reuse_run": ""}
    validate_current_head(event, environment, source, current)
    return {**source, "reuse_run": find_reuse(
        api, source, environment["CI_WORKFLOW_SHA"], environment["GITHUB_RUN_ID"],
    )}


def diagnostic(environment, api):
    """Validate a retained Windows payload from a trusted completed main-push run."""
    if (environment.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
            or environment.get("GITHUB_REF") != "refs/heads/main"):
        raise CIError("Diagnostics require a workflow dispatch on main")
    identifier = validate_number(environment.get("DIAGNOSTIC_RUN_ID"))
    run_path = f"{api.root}/actions/runs/{identifier}"
    try:
        run = api.api(run_path)
        if (validate_number(run["id"]) != identifier or run["status"] != "completed"
                or run["event"] != "push" or run["path"] != ".github/workflows/ci.yml"
                or run["head_branch"] != "main" or run["repository"]["full_name"] != api.repository
                or run["head_repository"]["full_name"] != api.repository):
            raise CIError("Diagnostic producer must be a completed same-repository main-push CI run")
        validate_sha(run["head_sha"])
        jobs = api.pages(f"{run_path}/jobs?filter=latest", "jobs")
        windows = [job for job in jobs if job["name"].rsplit(" / ", 1)[-1] == "windows-tests"]
        if (len(windows) != 1 or windows[0]["name"] != "full-ci / windows-tests"
                or windows[0]["status"] != "completed" or windows[0]["conclusion"] != "success"):
            raise CIError("Diagnostic producer requires successful latest Windows tests")
        steps = [step for step in windows[0]["steps"] if step["name"] in MSI_STEPS]
        if (len(steps) != len(MSI_STEPS) or {step["name"] for step in steps} != MSI_STEPS
                or any(step["status"] != "completed" or step["conclusion"] != "success" for step in steps)):
            raise CIError("Diagnostic producer requires successful MSI verification flows")
        artifacts = api.pages(f"{run_path}/artifacts", "artifacts")
        payloads = [artifact for artifact in artifacts if artifact["name"] == "windows-verification"]
        if len(payloads) != 1:
            raise CIError("Diagnostic producer requires one windows-verification artifact")
        artifact = payloads[0]
        validate_number(artifact["id"])
        expires = datetime.fromisoformat(artifact["expires_at"].replace("Z", "+00:00"))
        if (artifact["expired"] is not False or type(artifact["size_in_bytes"]) is not int
                or artifact["size_in_bytes"] <= 0 or expires.tzinfo is None
                or expires <= datetime.now(timezone.utc)):
            raise CIError("Diagnostic artifact must be retained, nonexpired, and nonempty")
        if api.api(run_path) != run:
            raise CIError("Diagnostic producer changed during validation")
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        raise CIError("Incomplete diagnostic producer evidence") from error


def check_results(environment):
    """Reject incomplete builds and keep MSIX-only diagnostics separate from full CI."""
    diagnostic = environment.get("CI_DIAGNOSTIC")
    if diagnostic not in ("true", "false"):
        raise CIError("CI_DIAGNOSTIC must be true or false")
    expected = ("success", "skipped", "skipped", "skipped", "success") if diagnostic == "true" else (
        "success", "success", "success", "success", "success",
    )
    for variable, result in zip(RESULT_VARIABLES, expected):
        actual = environment.get(variable)
        if actual != result:
            raise CIError(f"{variable} must be {result}, got {actual or 'missing'}")


def runner_file(environment, name, required=True):
    """Validate runner file paths before appending single-line outputs or summaries."""
    value = environment.get(name)
    if not value and not required:
        return None
    if (not isinstance(value, str) or not value or re.search(r"[\x00\r\n]", value)
            or not Path(value).is_absolute()):
        raise CIError(f"{name} must be an absolute runner file path")
    return Path(value)


def main():
    """Run a read-only CI selection or validation command."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "verify-head", "diagnostic", "check-results"))
    args = parser.parse_args()
    if args.command == "check-results":
        check_results(os.environ)
        return
    api = GitHub(os.environ.get("GITHUB_REPOSITORY"))
    if args.command == "diagnostic":
        diagnostic(os.environ, api)
        return
    event_path = runner_file(os.environ, "GITHUB_EVENT_PATH")
    event = json.loads(event_path.read_text())
    if args.command == "verify-head":
        verify_head(event, os.environ, api)
        return
    output = runner_file(os.environ, "GITHUB_OUTPUT")
    summary = runner_file(os.environ, "GITHUB_STEP_SUMMARY", required=False)
    values = prepare(event, os.environ, api)
    if values["reuse_run"] and summary:
        url = f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{values['reuse_run']}"
        with summary.open("a") as stream:
            stream.write(f"Reusing successful full PR CI from [run {values['reuse_run']}]({url}).\n")
    with output.open("a") as stream:
        stream.write("".join(f"{key}={value}\n" for key, value in values.items()))


if __name__ == "__main__":
    try:
        main()
    except (CIError, OSError, ValueError) as error:
        print(f"CI validation failed. {error}", file=sys.stderr)
        sys.exit(1)
