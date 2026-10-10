# Release policy

Every successful push to `main` publishes a release candidate with the Windows
MSI installer and Linux packages. Use `vMAJOR.MINOR.PATCH-rcN` tags for prereleases,
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

## Microsoft Store packages

The Windows release job also creates an unsigned x64 MSIX with the reserved
`Lincarl.MervinPDF` identity. Download the `windows-msix-store-upload` workflow
artifact for submission to Partner Center. It is separate from the public
GitHub release downloads because it cannot be installed until it is signed.
Microsoft signs the package after Store certification. This does not sign the
standalone MSI or EXE distributed on GitHub.

To build a stable Store upload without publishing a GitHub release, run the
`release` workflow manually on `main` and supply `store_version`, for example
`1.65.3`. The workflow requires a successful CI run for that main commit and
builds only the Windows packages. It creates no tag or GitHub release and does
not upload anything to Partner Center. Use a version newer than the last Store
submission. Keep the source commit and version with the submission record.

MSIX uses the same numeric ordering as MSI, with a fourth field of zero.
For example, `1.65.3-rc1` becomes `1.65.301.0` and `1.65.3` becomes
`1.65.399.0`. The Store reserves the fourth field, so never increment it for
a resubmission. Choose a newer public version instead. The major version must
be at least one. Submit a stable build to the public Store listing.

CI builds a verification-only `1.0.0` package and signs a copy with a temporary
certificate on a disposable Windows runner. The original upload package remains
unsigned. The lifecycle checks install, launch, upgrade, and uninstall that
copy. They do not establish Microsoft certification or Windows 11 compatibility
by themselves. Test the Store candidate on Windows 11 before submission,
including PDF activation, saving, printing, OCR, settings, and MSI coexistence.

For packaging diagnostics, the CI workflow's optional `msix_artifact_run` input
reuses a previous run's Windows payload and tests the current manifest and
MSIX test script. It skips compilation and application tests, so it cannot
qualify a Store release. The Store build requires successful full push CI on
its main commit.

Store copies delegate updates to Microsoft Store. Their Check for Updates
action opens the Store's Downloads and updates page. Their PDF registration
comes from the package manifest, and Windows Settings controls the default.
The existing MSI and Linux update paths remain available.

Store and MSI copies use separate single-instance groups. Windows AppData
compatibility can let the Store copy read and update preexisting MSI settings
and OCR models, so those files are not guaranteed to be isolated between
installations. Use separate `--profile` directories for verification.

See [STORE-LISTING.md](STORE-LISTING.md) for the listing, required images,
account setup, and certification notes.

Installed copies offer only stable releases as updates. A copy running
`1.64.10-rc2` can update to `1.64.10` when that stable release becomes available.

## Release retention

After publishing a stable release and all its assets successfully, the packaging
workflow keeps the three newest stable releases by version and deletes older
release entries and their downloads. It also deletes superseded prereleases whose
target version is at or below the published stable version. Candidates for higher
versions, drafts, and releases with unrecognized tags remain untouched.

Cleanup never deletes Git tags. The version allocator depends on that history,
including the legacy numeric prerelease tags. Failed builds and prerelease
publication do not trigger cleanup. Rerunning an older stable release skips
cleanup if that release is no longer the newest stable version. Candidates
published after cleanup are considered during the next stable release cleanup.

Preview the cleanup with the default dry run:

```bash
python3 scripts/prune-releases.py --released-tag v1.64.15
```

Add `--apply` to delete the selected GitHub release entries and their downloads.

## Package versions

The application, GitHub release, and artifact filenames retain the public
version, including the RC suffix. Package metadata uses each platform's ordering
rules so a stable release upgrades every candidate for its version.

| Package | `1.64.10-rc1` | `1.64.10-rc2` | `1.64.10` |
|---|---|---|---|
| Application display version | `1.64.10-rc1` | `1.64.10-rc2` | `1.64.10` |
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
