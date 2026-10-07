#!/usr/bin/env python3
"""Prune obsolete GitHub releases after stable publication, preserving all tags."""

import argparse
import json
import os
import re
import subprocess
import sys
from typing import NamedTuple

from release_version import ReleaseVersion, parse_tag


class Asset(NamedTuple):
    name: str
    state: str
    size: int


class Release(NamedTuple):
    id: int
    tag: str
    draft: bool
    prerelease: bool
    published_at: str | None
    assets: tuple[Asset, ...]

    @property
    def version(self) -> ReleaseVersion | None:
        return parse_tag(self.tag)

    @property
    def stable(self) -> bool:
        version = self.version
        return bool(
            version is not None and version.rc is None
            and not self.draft and not self.prerelease and self.published_at
        )


class Plan(NamedTuple):
    keep: list[Release]
    delete: list[Release]
    skip_reason: str | None = None


def command(*args: str) -> str:
    return subprocess.run(args, text=True, capture_output=True, check=True).stdout


def parse_release(raw: object) -> Release:
    """Reject malformed API data before it can influence a deletion plan."""
    if not isinstance(raw, dict):
        raise ValueError("Invalid release response")
    identifier = raw.get("id")
    tag = raw.get("tag_name")
    draft, prerelease = raw.get("draft"), raw.get("prerelease")
    published_at, assets = raw.get("published_at"), raw.get("assets")
    if (
        type(identifier) is not int or identifier <= 0 or not isinstance(tag, str)
        or type(draft) is not bool or type(prerelease) is not bool
        or (published_at is not None and not isinstance(published_at, str))
        or not isinstance(assets, list)
    ):
        raise ValueError("Invalid release metadata")
    parsed_assets = []
    for asset in assets:
        if (
            not isinstance(asset, dict)
            or not isinstance(asset.get("name"), str)
            or not isinstance(asset.get("state"), str)
            or type(asset.get("size")) is not int or asset["size"] < 0
        ):
            raise ValueError("Invalid release asset metadata")
        parsed_assets.append(Asset(asset["name"], asset["state"], asset["size"]))
    return Release(identifier, tag, draft, prerelease, published_at, tuple(parsed_assets))


def expected_assets(tag: str) -> set[str]:
    version = tag[1:]
    return {
        f"MervinPDF-{version}.msi",
        f"MervinPDF-{version}-x86_64.AppImage",
        f"mervin-pdf_{version}_ubuntu26.04_amd64.deb",
        f"mervin-pdf-{version}.x86_64.rpm",
        "SHA256SUMS",
    }


def build_plan(releases: list[Release], released_tag: str) -> Plan:
    version = parse_tag(released_tag)
    if version is None or version.rc is not None:
        raise ValueError("Cleanup requires a stable version tag")
    stable = sorted(
        (release for release in releases if release.stable),
        key=lambda release: release.version.base, reverse=True,
    )
    trigger = next((release for release in stable if release.tag == released_tag), None)
    if trigger is None:
        raise ValueError(f"{released_tag} is not a published stable release")
    if stable[0].version.base > version.base:
        return Plan(stable[:3], [], f"{released_tag} is no longer the newest stable release")
    uploaded = {asset.name for asset in trigger.assets if asset.state == "uploaded" and asset.size > 0}
    missing = expected_assets(released_tag) - uploaded
    if missing:
        raise ValueError(f"{released_tag} publication is incomplete; missing uploaded assets {sorted(missing)}")
    obsolete = [
        release for release in releases
        if not release.draft and release.published_at and release.prerelease
        and release.version is not None and release.version.base <= version.base
    ]
    return Plan(stable[:3], stable[3:] + sorted(obsolete, key=lambda release: release.tag))


def list_releases(repository: str) -> list[Release]:
    pages = json.loads(command(
        "gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100",
    ))
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValueError("Invalid paginated releases response")
    releases = [parse_release(raw) for page in pages for raw in page]
    if len({release.id for release in releases}) != len(releases):
        raise ValueError("Release listing changed during pagination; retry cleanup")
    return releases


def confirmed_absent(repository: str, identifier: int, error: subprocess.CalledProcessError) -> bool:
    # GitHub also uses 404 for inaccessible repositories. Only a successful
    # inventory read can confirm that another cleanup removed this release.
    return bool(
        re.search(r"\bHTTP 404\b", error.stderr or "")
        and all(release.id != identifier for release in list_releases(repository))
    )


def prune_releases(repository: str, released_tag: str, apply: bool = False) -> None:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid repository name")
    version = parse_tag(released_tag)
    if version is None or version.rc is not None:
        raise ValueError("Cleanup requires a stable version tag")
    plan = build_plan(list_releases(repository), released_tag)
    if plan.skip_reason:
        print(f"Skipping cleanup. {plan.skip_reason}")
        return
    print("Keep stable releases " + ", ".join(release.tag for release in plan.keep))
    for release in plan.delete:
        print(f"{'Delete' if apply else 'Would delete'} release {release.tag} (ID {release.id})")
    if not apply:
        print("Dry run. Pass --apply to delete release entries and assets. Git tags are preserved.")
        return
    for candidate in plan.delete:
        # Recompute the policy before every deletion so a concurrent publication
        # or promotion cannot make the initial snapshot authoritative.
        fresh = build_plan(list_releases(repository), released_tag)
        if fresh.skip_reason:
            print(f"Stopping cleanup. {fresh.skip_reason}")
            return
        if not any(release.id == candidate.id and release.tag == candidate.tag
                   and release.prerelease == candidate.prerelease for release in fresh.delete):
            print(f"Skipping changed or removed release {candidate.tag}")
            continue
        endpoint = f"repos/{repository}/releases/{candidate.id}"
        try:
            current = parse_release(json.loads(command("gh", "api", endpoint)))
            if (current.id, current.tag, current.draft, current.prerelease, bool(current.published_at)) != (
                candidate.id, candidate.tag, candidate.draft, candidate.prerelease, bool(candidate.published_at)
            ):
                print(f"Skipping changed release {candidate.tag}")
                continue
            command("gh", "api", "--method", "DELETE", endpoint)
        except subprocess.CalledProcessError as error:
            if not confirmed_absent(repository, candidate.id, error):
                raise
            print(f"Release {candidate.tag} was already removed")
            continue
        print(f"Deleted release {candidate.tag}. Git tag preserved.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--released-tag", required=True)
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", "lincarl/mervin-pdf"))
    parser.add_argument("--apply", action="store_true", help="Delete releases instead of previewing the plan")
    args = parser.parse_args()
    try:
        prune_releases(args.repository, args.released_tag, args.apply)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip() if isinstance(error, subprocess.CalledProcessError) and error.stderr else str(error)
        print(detail, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
