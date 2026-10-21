#!/usr/bin/env python3
"""Authorize and report explicit PR builds using trusted GitHub review evidence.

Write operations run only from the default-branch workflow. PR code runs in
separate jobs without this controller's write token. The inspect command is
read-only and also supports the initial workflow installation.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


CONTEXT = "reviewed-ci"
WORKFLOW = "reviewed-ci.yml"
BOT_ID = 136622811
BOT_LOGIN = "coderabbitai[bot]"
SUMMARY_MARKER = "<!-- This is an auto-generated comment: summarize by coderabbit.ai -->"
# Since 2026-10-20, CodeRabbit starts its comments with a version marker such as
# <!-- coderabbit-ai-generated:v1 --> before the marker that names the comment kind.
VERSION_MARKER = re.compile(r"\s*<!--\s*coderabbit-ai-generated:v[0-9]+\s*-->")
RUN_TITLE = re.compile(r"reviewed-ci PR #([1-9][0-9]*) @ ([0-9a-f]{40})")
ACTIVE_STATES = {"queued", "in_progress", "waiting", "pending", "requested"}
REQUIRED_JOBS = {
    "repository-checks", "linux-tests (release)", "linux-tests (sanitize)",
    "linux-system-qt-tests", "windows-tests", "windows-msix-tests",
}


class GateError(RuntimeError):
    """A request lacks the evidence needed to run or approve CI."""


class GitHub:
    def __init__(self, repository):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise GateError("Invalid repository")
        self.repository = repository
        self.root = f"repos/{repository}"

    def api(self, path, method="GET", data=None):
        command = ["gh", "api", path, "--method", method]
        if data is not None:
            command += ["--input", "-"]
        result = subprocess.run(
            command, input=json.dumps(data) if data is not None else None,
            text=True, capture_output=True, check=False,
        )
        if result.returncode:
            raise GateError(f"GitHub request failed for {path}: {result.stderr.strip()}")
        return json.loads(result.stdout) if result.stdout.strip() else None

    def pages(self, path, key=None):
        items = []
        separator = "&" if "?" in path else "?"
        for page in range(1, 101):
            response = self.api(f"{path}{separator}per_page=100&page={page}")
            batch = response[key] if key else response
            items.extend(batch)
            if len(batch) < 100:
                return items
        raise GateError("GitHub pagination limit reached; refusing incomplete evidence")

    def status(self, sha, state, description, target_url):
        self.api(f"{self.root}/statuses/{sha}", "POST", {
            "context": CONTEXT, "state": state,
            "description": description[:140], "target_url": target_url,
        })


def validate_inputs(number, sha):
    if not re.fullmatch(r"[1-9][0-9]*", str(number)):
        raise GateError("PR number must be a positive decimal without leading zeros")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise GateError("Expected SHA must be a full lowercase commit SHA")


def is_bot(user):
    return (isinstance(user, dict) and user.get("id") == BOT_ID and user.get("login") == BOT_LOGIN
            and user.get("type") == "Bot")


def is_summary(body):
    """Whether a CodeRabbit comment body is the PR summary that the bot edits in place.

    The summary marker must open the comment, after at most one version marker, so a
    reply that quotes it does not count.
    """
    version = VERSION_MARKER.match(body)
    return body[version.end() if version else 0:].lstrip().startswith(SUMMARY_MARKER)


def latest_status(api, sha, context):
    statuses = api.pages(f"{api.root}/commits/{sha}/statuses")
    return next((item for item in statuses if item["context"] == context), None)


def validate_coderabbit(statuses, comments, sha):
    """Return the IDs of CodeRabbit's status and summary that prove a completed review of sha.

    Raise GateError for the first check that fails, such as an untrusted status, an
    unfinished review, no recognised summary, or coverage of another commit.
    """
    # Inspect the newest context, including an unexpected publisher. An older
    # success must never hide a pending, failed, or spoofed replacement.
    status = next((item for item in statuses if item["context"] == "CodeRabbit"), None)
    if not status or not is_bot(status.get("creator", {})):
        raise GateError("CodeRabbit has not published a trusted status for this commit")
    if status["state"] != "success" or status.get("description") != "Review completed":
        raise GateError("CodeRabbit review is not complete")

    # The bot edits this summary in place. Select it before checking coverage so
    # a newer paused or incomplete summary cannot fall back to an older success.
    # One-off replies can retain expired throttling notices after review resumes.
    summaries = [item for item in comments if is_bot(item.get("user"))
                 and isinstance(item.get("body"), str) and is_summary(item["body"])]
    if not summaries:
        # Bot comments without a recognised summary usually mean that CodeRabbit
        # changed its comment format, as in October 2026. Say so instead of
        # reporting that coverage is missing.
        if any(is_bot(item.get("user")) for item in comments):
            raise GateError("CodeRabbit has comments on this PR, but the gate found no "
                            "summary among them. Check whether its comment format changed")
        raise GateError("CodeRabbit review coverage is missing from this PR")
    summary = max(summaries, key=lambda item: (item["updated_at"], item["id"]))
    body = summary["body"]
    if re.search(r"<!--[^>]*(?:skip review|reviews? paused|pause review|review in progress|rate[ _-]limit)",
                 body, re.IGNORECASE):
        raise GateError("CodeRabbit skipped, paused, or has not finished this review")
    markers = re.findall(r"<!--\s*final_review_risk_coverage:(\{[^\n]+?\})\s*-->", body)
    if len(markers) != 1:
        raise GateError("CodeRabbit review coverage is missing or ambiguous")
    try:
        coverage = json.loads(markers[0])
    except json.JSONDecodeError as error:
        raise GateError("Invalid CodeRabbit review coverage") from error
    if coverage.get("kind") != "reviewed" or coverage.get("coveredCommitId") != sha:
        raise GateError("CodeRabbit has not reviewed the current PR commit")
    return {"status_id": status["id"], "summary_id": summary["id"]}


def review_policy(api, number):
    """Read all threads and GitHub's effective approval decision, including rulesets."""
    owner, name = api.repository.split("/")
    query = """
      query($owner: String!, $name: String!, $number: Int!, $cursor: String) {
        repository(owner: $owner, name: $name) {
          pullRequest(number: $number) {
            reviewDecision
            reviewThreads(first: 100, after: $cursor) {
              nodes { id isResolved isOutdated }
              pageInfo { hasNextPage endCursor }
            }
          }
        }
      }
    """
    cursor = None
    for _ in range(100):
        response = api.api("graphql", "POST", {"query": query, "variables": {
            "owner": owner, "name": name, "number": int(number), "cursor": cursor,
        }})
        if response.get("errors"):
            raise GateError("Could not read the effective review policy")
        pr = response["data"]["repository"]["pullRequest"]
        if pr["reviewDecision"] not in (None, "APPROVED"):
            raise GateError("Repository policy requires further formal review approval")
        if any(not thread["isResolved"] for thread in pr["reviewThreads"]["nodes"]):
            raise GateError("Unresolved review threads remain, including outdated threads")
        page = pr["reviewThreads"]["pageInfo"]
        if not page["hasNextPage"]:
            return pr["reviewDecision"]
        cursor = page["endCursor"]
    raise GateError("Review thread pagination limit reached")


def inspect(api, number, sha):
    validate_inputs(number, sha)
    pr = api.api(f"{api.root}/pulls/{number}")
    if pr["state"] != "open" or pr["draft"]:
        raise GateError("CI requires an open, regular PR")
    if pr["head"]["sha"] != sha:
        raise GateError("Expected SHA no longer matches the PR head")
    if pr["base"]["ref"] != "main":
        raise GateError("This CI gate supports PRs targeting main")
    base = api.api(f"{api.root}/git/ref/heads/main")["object"]["sha"]
    comparison = api.api(f"{api.root}/compare/{base}...{sha}")
    if comparison["status"] not in ("ahead", "identical"):
        raise GateError("Update the PR with main, then obtain a fresh review")
    evidence = validate_coderabbit(
        api.pages(f"{api.root}/commits/{sha}/statuses"),
        api.pages(f"{api.root}/issues/{number}/comments"), sha,
    )
    decision = review_policy(api, number)
    reviews = api.pages(f"{api.root}/pulls/{number}/reviews")
    formal = [review for review in reviews if is_bot(review.get("user", {}))
              and review["state"] in ("APPROVED", "CHANGES_REQUESTED", "DISMISSED")]
    latest = max(formal, key=lambda item: item["id"], default=None)
    if latest and latest["state"] == "CHANGES_REQUESTED":
        raise GateError("CodeRabbit still requests changes")
    if decision == "APPROVED" and (not latest or latest["state"] != "APPROVED"
                                    or latest["commit_id"] != sha):
        raise GateError("Policy requires CodeRabbit's formal approval on this commit")
    # Check once more after pagination so a push during validation is rejected.
    current = api.api(f"{api.root}/pulls/{number}")
    if current["head"]["sha"] != sha or current["state"] != "open" or current["draft"]:
        raise GateError("PR changed during review validation")
    return {"pr_number": str(number), "sha": sha, "base_sha": base, **evidence}


def run_url(api, run_id):
    return f"https://github.com/{api.repository}/actions/runs/{run_id}"


def trusted_dispatch():
    if os.environ.get("GITHUB_REF") != "refs/heads/main":
        raise GateError("Start CI must use the workflow on main")
    if os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch":
        raise GateError("CI authorization requires an explicit workflow dispatch")
    if os.environ.get("GITHUB_RUN_ATTEMPT") != "1":
        raise GateError("Use a new Start CI dispatch with retry=true instead of rerunning jobs")


def start(api, number, sha, retry=False):
    trusted_dispatch()
    actor = os.environ["GITHUB_ACTOR"]
    permission = api.api(f"{api.root}/collaborators/{actor}/permission")["permission"]
    if permission not in ("admin", "write", "maintain"):
        raise GateError("CI authorization requires repository write permission")
    evidence = inspect(api, number, sha)
    previous = latest_status(api, sha, CONTEXT)
    if previous and not retry:
        print("This revision already has an attempt. Use retry=true for an explicit retry.")
        return None
    if previous:
        request = api.api(f"{api.root}/actions/runs/{os.environ['GITHUB_RUN_ID']}")
        if previous["state"] == "pending" or request["created_at"] <= previous["created_at"]:
            print("An overlapping request already handled this revision. Skipping duplicate.")
            return None
    evidence = inspect(api, number, sha)
    api.status(sha, "pending", "Authorized revision is running full CI",
               run_url(api, os.environ["GITHUB_RUN_ID"]))
    return evidence


def finish(api, number, sha, build_result):
    trusted_dispatch()
    url = run_url(api, os.environ["GITHUB_RUN_ID"])
    previous = latest_status(api, sha, CONTEXT)
    if not previous or previous.get("target_url") != url or previous["state"] != "pending":
        raise GateError("This run no longer owns the pending CI status")
    try:
        if build_result != "success":
            raise GateError(f"Full CI did not succeed ({build_result})")
        jobs = api.pages(f"{api.root}/actions/runs/{os.environ['GITHUB_RUN_ID']}/jobs?filter=latest", "jobs")
        successful = {job["name"].rsplit(" / ", 1)[-1] for job in jobs
                      if job["status"] == "completed" and job["conclusion"] == "success"}
        if not REQUIRED_JOBS.issubset(successful):
            raise GateError("Required full CI jobs are missing, skipped, or unsuccessful")
        inspect(api, number, sha)
    except GateError as error:
        api.status(sha, "failure", str(error), url)
        raise
    api.status(sha, "success", "Reviewed commit passed all required full CI jobs", url)


def run_revision(run):
    if run.get("event") != "workflow_dispatch":
        return None
    match = RUN_TITLE.fullmatch(run.get("display_title", ""))
    return match.groups() if match else None


def invalidate(api, number):
    """Cancel only obsolete PR runs. Fetch current state, not a stale event head."""
    pr = api.api(f"{api.root}/pulls/{number}")
    current = pr["head"]["sha"] if pr["state"] == "open" and not pr["draft"] else None
    runs = {}
    for state in sorted(ACTIVE_STATES):
        for run in api.pages(f"{api.root}/actions/workflows/{WORKFLOW}/runs"
                             f"?event=workflow_dispatch&status={state}", "workflow_runs"):
            runs[run["id"]] = run
    for run in runs.values():
        revision = run_revision(run)
        if not revision or revision[0] != str(number) or revision[1] == current:
            continue
        if run["status"] not in ACTIVE_STATES:
            continue
        previous = latest_status(api, revision[1], CONTEXT)
        url = run_url(api, run["id"])
        if previous and previous.get("target_url") == url and previous["state"] == "pending":
            api.status(revision[1], "failure", "PR revision changed or PR is no longer ready", url)
        try:
            api.api(f"{api.root}/actions/runs/{run['id']}/cancel", "POST")
        except GateError:
            if api.api(f"{api.root}/actions/runs/{run['id']}")["status"] != "completed":
                raise


def reconcile(api, run_id):
    """Close an abandoned pending status after cancellation or controller failure.

    Only finish can publish success. Overall workflow success can also mean that
    all expensive jobs were skipped, so this fallback can only publish failure.
    """
    run = api.api(f"{api.root}/actions/runs/{run_id}")
    if run["path"] != f".github/workflows/{WORKFLOW}" or run["head_branch"] != "main":
        return
    revision = run_revision(run)
    if not revision or run["status"] != "completed":
        return
    previous = latest_status(api, revision[1], CONTEXT)
    url = run_url(api, run_id)
    if previous and previous.get("target_url") == url and previous["state"] == "pending":
        api.status(revision[1], "failure", "CI ended without a verified successful result", url)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("inspect", "start", "finish", "invalidate", "reconcile"))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", "lincarl/mervin-pdf"))
    parser.add_argument("--pr", default=os.environ.get("PR_NUMBER"))
    parser.add_argument("--sha", default=os.environ.get("EXPECTED_SHA"))
    parser.add_argument("--run-id", default=os.environ.get("RECONCILE_RUN_ID"))
    args = parser.parse_args()
    api = GitHub(args.repository)
    if args.command == "inspect":
        print(json.dumps(inspect(api, args.pr, args.sha), indent=2))
    elif args.command == "start":
        evidence = start(api, args.pr, args.sha, os.environ.get("RETRY") == "true")
        if evidence:
            with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
                output.write(f"authorized=true\nsha={evidence['sha']}\n")
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
                summary.write("Authorized review evidence\n\n```json\n"
                              + json.dumps(evidence, indent=2) + "\n```\n")
    elif args.command == "finish":
        finish(api, args.pr, args.sha, os.environ["BUILD_RESULT"])
    elif args.command == "invalidate":
        if not args.pr or not re.fullmatch(r"[1-9][0-9]*", args.pr):
            raise GateError("Invalid PR number")
        invalidate(api, args.pr)
    else:
        if not args.run_id or not re.fullmatch(r"[1-9][0-9]*", args.run_id):
            raise GateError("Invalid run ID")
        reconcile(api, args.run_id)


if __name__ == "__main__":
    try:
        main()
    except (GateError, KeyError, TypeError, ValueError) as error:
        print(f"CI gate rejected the request. {error}", file=sys.stderr)
        sys.exit(1)
