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

Installed copies offer only stable releases as updates. A copy running
`1.64.10-rc2` can update to `1.64.10` when that stable release becomes available.

## Windows MSI signing

The release workflow supports SignPath for both automatic release candidates and
tag-triggered releases. Signing stays disabled while the account is being set up.
An unset or `false` repository variable `SIGNPATH_ENABLED` keeps the existing
unsigned MSI build and records that choice in the Windows job summary. Other
values besides lowercase `true` and `false` fail configuration validation.

After creating the SignPath account:

1. Create a project for `lincarl/mervin-pdf`. Add the predefined `GitHub.com`
   trusted build system to the organization and link it to the project. Install
   the SignPath GitHub App for this repository if using audit-log evaluation.
2. Create an artifact configuration from
   [`packaging/signpath/msi.xml`](../packaging/signpath/msi.xml), for example with
   slug `windows-msi`. GitHub uploads a ZIP containing one `MervinPDF-*.msi`, so
   keep the XML's `zip-file` wrapper. This configuration signs only the MSI.
3. Configure a signing policy with a publicly trusted code-signing certificate.
   Give a dedicated CI user's API token Submitter access to that policy. A
   self-signed test certificate fails the workflow's Windows trust check.
4. Add the following repository settings under **Settings > Secrets and
   variables > Actions**. Use the slugs from SignPath, not their display names.
5. Set `SIGNPATH_ENABLED` to `true` after the remaining settings and SignPath
   policy are ready.

| Kind | Name | Value |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | CI user's SignPath API token |
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath organization ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | Project slug, for example `mervin-pdf` |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | Policy slug, for example `release-signing` |
| Variable | `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` | Artifact configuration slug, for example `windows-msi` |
| Variable | `SIGNPATH_ENABLED` | `true` to require signing, unset or `false` during setup |

The CI workflow passes only `SIGNPATH_API_TOKEN` to the reusable release
workflow. Pull-request MSI verification and local `scripts/deploy.ps1 -Installer`
builds remain unsigned and require no SignPath credentials.

When enabled, the Windows release job checks configuration before building,
uploads `windows-msi-unsigned`, and submits its artifact ID to SignPath. It waits
up to 30 minutes for signing, including any manual approval, and downloads to a
separate directory. It requires the expected MSI filename and a `Valid`
Authenticode signature before uploading the final `windows-msi` artifact.
Missing settings, rejected requests, timeouts, missing output, and invalid
signatures fail the job and prevent publication. There is no unsigned fallback
when signing is enabled. Release checksums cover the final signed MSI; the
unsigned intermediate artifact is excluded from GitHub release downloads.

Configure origin restrictions for this repository and verify that the policy
accepts both the `main` CI run and the `v*` tag release run. Do not assume that a
branch-only restriction also accepts tag-triggered builds. The existing release
check still requires the commit to be on `main`. After enabling signing, check
the first authorized candidate and stable release runs in SignPath and GitHub,
then verify a downloaded MSI on Windows:

```powershell
Get-AuthenticodeSignature -LiteralPath .\MervinPDF-<version>.msi |
    Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
```

Expect `Status` to be `Valid` and the signer to match the configured certificate.
MSI signing does not sign `MervinPDF.exe` inside the installer. The executable
remains unsigned, and bundled Qt and Microsoft signatures remain unchanged.
Policies that require signed application executables need additional signing.

See SignPath's [GitHub integration](https://docs.signpath.io/trusted-build-systems/github),
[artifact configuration syntax](https://docs.signpath.io/artifact-configuration/syntax),
and [origin verification](https://docs.signpath.io/origin-verification) documentation.

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
