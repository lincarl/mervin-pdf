# Release policy

Every successful push to `main` publishes a release candidate with the Windows
installers and Linux packages. Use `vMAJOR.MINOR.PATCH-rcN` tags for prereleases,
starting at `rc1`. Keep the same target version for `rc2`, `rc3`, and subsequent
candidates until the stable version is released. For example, `v1.64.10-rc1` and
`v1.64.10-rc2` both lead to stable `v1.64.10`.

The CI workflow runs all required checks, then calls the packaging workflow.
Failed checks and pushes to other branches do not publish prereleases. The checks
include the `i18n_catalogs` test, so changed UI text must be translated in every
catalog before the push can publish a candidate (see [TRANSLATING.md](TRANSLATING.md)). Each main
push keeps its own run. Retrying a run reuses its annotated tag, provided the tag
still points to that run's commit. Concurrent runs allocate distinct RC numbers.

The allocator starts with the patch after the highest existing numeric tag and
continues any higher RC target already in use. All numeric tags count as occupied,
including historical prereleases. Preserve the old `v1.64.8` and `v1.64.9` tags.
The first candidate under this policy is therefore `v1.64.10-rc1`.

## Stable releases

Publish a stable release only when the user explicitly asks. Wait for CI to pass
on the intended `main` commit, then create and push the annotated numeric tag for
the RC target, such as `v1.64.10`, with the message `Release 1.64.10`.
The tag must be unused. Do not rename or move an existing tag.

A numeric tag publishes a stable release. An RC tag publishes a prerelease.
Both use the same packaging workflow, which accepts only commits on `main`.
After `v1.64.10` exists, the next automatic candidate becomes `v1.64.11-rc1`.
Packaging runs no tests, so the successful CI run must precede a stable tag push.

Installed copies offer only stable releases as updates. A copy running
`1.64.10-rc2` can update to `1.64.10` when that stable release becomes available.

## Package versions

The application, GitHub release, and artifact filenames retain the public
version, including the RC suffix. Package metadata uses each platform's ordering
rules so a stable release upgrades every candidate for its version.

| Package | `1.64.10-rc1` | `1.64.10-rc2` | `1.64.10` |
|---|---|---|---|
| Application and NSIS display version | `1.64.10-rc1` | `1.64.10-rc2` | `1.64.10` |
| Debian and RPM package version | `1.64.10~rc1` | `1.64.10~rc2` | `1.64.10` |
| MSI product version | `1.64.1001` | `1.64.1002` | `1.64.1099` |

MSI compares only three numeric fields and cannot store an RC suffix. Its build
field is `patch * 100 + rc` for candidates and `patch * 100 + 99` for stable
releases. The MSI product name includes the public version so Windows also shows
which candidate was installed. The MSI version column can show the encoded
numeric version. The fixed upgrade code remains unchanged.

This mapping supports major and minor values through 255, patch values through
654, and RC numbers from 1 through 98 for each target. Validation rejects values
outside these limits before packaging. If a limit is reached, choose a new
minor or major target explicitly rather than reusing an installer version.

Local builds accept `-DMERVIN_VERSION=1.64.10-rc1`. Without an override they use
`0.0.0`. Build metadata in `generated/package-version.json` supplies the Windows
packager with the corresponding installer version.
