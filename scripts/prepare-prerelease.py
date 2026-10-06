#!/usr/bin/env python3
"""Allocate an annotated release-candidate tag for a successful main CI run."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

from release_version import ReleaseVersion, parse_tag


def command(*args: str, input_text: str | None = None) -> str:
    return subprocess.run(
        args, input=input_text, text=True, capture_output=True, check=True
    ).stdout


def next_tag(tags: dict[str, tuple[str, str]]) -> str:
    versions = [version for tag in tags if (version := parse_tag(tag))]
    # Numeric tags are occupied even when an older workflow used them for a
    # prerelease. Never rename those tags or reserve their versions again.
    latest = max((version.base for version in versions if version.rc is None), default=(0, 0, 0))
    target = latest[0], latest[1], latest[2] + 1
    target = max(target, max((version.base for version in versions if version.rc is not None), default=target))
    rc = 1 + max((version.rc for version in versions if version.base == target and version.rc is not None), default=0)
    ReleaseVersion(*target, rc).validate()
    return "v" + ".".join(str(part) for part in target) + f"-rc{rc}"


def fetch_tags() -> dict[str, tuple[str, str]]:
    command("git", "fetch", "--tags", "origin")
    output = command(
        "git", "for-each-ref",
        "--format=%(refname:strip=2)%09%(objecttype)%09%(*objectname)", "refs/tags",
    )
    tags = {}
    for line in output.splitlines():
        name, kind, target = line.split("\t")
        tags[name] = kind, target
    return tags


def existing_tag(tags: dict[str, tuple[str, str]], commit: str, message: str) -> str | None:
    for tag, (kind, target) in tags.items():
        version = parse_tag(tag)
        if version is None or version.rc is None or kind != "tag" or target != commit:
            continue
        annotation = command("git", "for-each-ref", "--format=%(contents)", f"refs/tags/{tag}")
        if annotation.rstrip("\n") == message:
            version.validate()
            return tag
    return None


def api(repository: str, endpoint: str, **fields: str) -> dict[str, object]:
    args = ["gh", "api", f"repos/{repository}/git/{endpoint}"]
    if fields:
        args += ["--method", "POST", "--input", "-"]
    result = json.loads(command(*args, input_text=json.dumps(fields) if fields else None))
    if not isinstance(result, dict):
        raise ValueError("GitHub returned an invalid tag API response")
    return result


def prepare_tag(repository: str, commit: str, run_id: str) -> str:
    message = f"Prerelease for CI run {run_id}"
    for _ in range(5):
        tags = fetch_tags()
        previous = existing_tag(tags, commit, message)
        if previous:
            return previous
        tag = next_tag(tags)
        annotation = api(repository, "tags", tag=tag, message=message, object=commit, type="commit")
        object_sha = annotation.get("sha")
        if not isinstance(object_sha, str) or not re.fullmatch(r"[0-9a-f]{40}", object_sha):
            raise ValueError("GitHub returned an invalid annotated tag SHA")
        try:
            api(repository, "refs", ref=f"refs/tags/{tag}", sha=object_sha)
            return tag
        except subprocess.CalledProcessError as error:
            # Retry only when the requested ref exists, including a lost success response.
            try:
                api(repository, f"ref/tags/{tag}")
            except subprocess.CalledProcessError:
                raise error
    raise RuntimeError("Could not allocate a prerelease tag after five concurrent collisions")


def main() -> int:
    try:
        required = ("GH_TOKEN", "GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_OUTPUT")
        for name in required:
            if not os.environ.get(name):
                raise ValueError(f"Missing required environment variable {name}")
        repository = os.environ["GITHUB_REPOSITORY"]
        commit = os.environ["GITHUB_SHA"]
        run_id = os.environ["GITHUB_RUN_ID"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("Invalid repository name")
        if not re.fullmatch(r"[0-9a-f]{40}", commit) or not re.fullmatch(r"[1-9][0-9]*", run_id):
            raise ValueError("Invalid CI commit or run ID")
        if command("git", "rev-parse", "HEAD").strip() != commit:
            raise ValueError("Checkout does not match the successful CI commit")
        tag = prepare_tag(repository, commit, run_id)
        with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
            output.write(f"tag={tag}\nversion={tag[1:]}\n")
        print(tag)
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) and error.stderr else str(error)
        print(detail, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
