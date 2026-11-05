# Pull request CI

Complete local code reviews and valid fixes before pushing the branch and opening
a regular pull request. PR creation, new commits, reopening, and marking a draft
ready run full CI automatically.
No GitHub reviewer result or manual CI authorization is required. Draft PRs do
not start full CI, and converting a regular PR to draft cancels its builds.

The required `reviewed-ci` Actions check passes only after every required full CI
job succeeds, or after CI verifies successful evidence for the same source and
workflow revisions. Failed, canceled, unexpectedly skipped, and unfinished jobs
cannot pass the gate. Merge only after the required check passes on the current
revision, the branch meets freshness requirements, and review conversations are
resolved. Do not bypass branch protection or publish a synthetic passing status.

## Source revisions and permissions

PR CI loads its controls from the workflow's immutable revision and builds the
exact `pull_request.head.sha` from the source repository. An older PR branch does
not need to contain the latest CI helper. Fork contributions use the same full
test suite.

PR build and test jobs have read-only repository permissions. Checkouts do not retain
credentials, and build jobs receive no repository secrets. The reusable workflow
uses `cache-mode: read` for PRs. GitHub enforces read-only dependency cache access
through scoped runtime tokens, so PR code cannot save caches that later main or
release builds might consume. Main and release jobs keep their existing cache
behavior.

PR code still runs on GitHub-hosted runners. Maintainers finish local reviews
before publishing their branches. GitHub's normal approval policy for workflows
from external contributors still applies.

## Required verification

The reusable full CI workflow runs the following jobs.

- Repository metadata, script, workflow, and license checks.
- Linux release and sanitizer tests on Ubuntu 24.04 with patched Qt and qpdf 11.
- Linux tests on Ubuntu 26.04 with patched Qt and the pinned upstream vcpkg
  libraries, matching the AppImage and DEB toolchain, plus a staged runtime check.
- Windows tests, MSI installation, upgrade, repair, uninstall, and wizard checks.
- Windows MSIX installation, activation, upgrade, and uninstall checks.

The Ubuntu 24.04 test jobs remove the Qt and qpdf source/build scratch trees
after their installed SDKs are ready. They preserve SDKs, provenance, logs,
validation results, the official Qt support tree and MuPDF's cached tree.
Both variants compress executable debug sections with zlib while retaining
all DWARF information and sanitizer instrumentation. Disk usage is reported
before and after cleanup and after the complete build.

Windows CI and release jobs bootstrap PowerShell 7.6.8 from its official archive
with a pinned SHA-256. WiX installation, MSI packaging, and MSI lifecycle checks
use an isolated .NET SDK 10.0.402 with runtime 10.0.13, verified against the
official archive's SHA-512. Their runtime selection excludes the runner's other
.NET versions and previews. The bootstrap still uses runner PowerShell, and the
MSIX lifecycle test retains Windows PowerShell 5.1 for native Appx support.

A final job explicitly checks every expected result. The outer `reviewed-ci`
check requires that job and the rest of full CI to succeed. The optional MSIX
artifact diagnostic remains a separate manual `main` run. Its reduced set of
checks cannot satisfy the PR gate or provide reusable full CI evidence. Diagnostic
runs have read-only caches and accept only retained Windows verification artifacts
from a completed main-push CI run with successful Windows build, test, MSI
verification, and artifact-upload steps. MSIX packaging or testing may have
failed, since that can be the reason to diagnose the package.

The Fedora RPM builds the same upstream libraries and checks exported symbols
and its staged private runtime in the release workflow.
Verification artifacts remain on the run that produced them, with their existing
retention periods.

## Patched Qt SDK caches

Every Linux and Windows build lane uses `.github/actions/prepare-qt`, including
both Ubuntu 24.04 test variants, Ubuntu 26.04, Windows MSI and MSIX packaging,
and Fedora RPM packaging. The MSIX lifecycle job consumes the Windows test job's
verified payload. Existing job IDs and required gates remain unchanged.

The action installs source-build headers and the pinned official support SDK
without exporting the latter's environment. `scripts/prepare-qt-sdk.py` hashes
the Qt source recipe, patches, builder, API checks, selection helper, action,
dependency installation script and support binaries. It also includes the native
OS, compiler, CMake, Ninja and development-package context. Windows identities
include the MSVC compiler hash, Windows SDK and runner image versions. SDK caches
have exact keys with no prefix fallback and contain only the installed SDK.
Matching main and release lanes can share them. Cold jobs can each build an SDK;
PR jobs cannot populate a cache for another job because their tokens are read-only.

A restore must match its recorded build inputs, support hashes, complete file
inventory and toolchain identity. Extra, missing or changed files and links outside
the SDK fail validation. Each use compiles and runs the current PNG, PCRE2 JIT
and CBOR API checks against the selected SDK, including cache hits. An invalid
cache or failed source build stops the job. Delete an invalid cache or change the
inputs after fixing the cause; there is no official-Qt fallback. A failed local
source build can be resumed with `build-qt-patched.py`, but an incomplete cache
cannot be treated as a verified SDK.

Only successful validation exports the SDK paths to subsequent build and package
steps. Windows deployment checks the configured Qt path and rejects DLLs from a
different SDK. The existing PE import, plugin, staged Linux runtime and native
installer checks remain mandatory. Cache saves happen after SDK validation and
only outside PRs. GitHub's `cache-mode: read` additionally prevents PR code from
writing caches regardless of its checked-out action code.

The rebuilt Windows Qt DLLs and plugins lose Qt's original Authenticode
signatures. The copied support DLLs retain theirs. Full native Windows CI must
qualify the resulting application and installers before this selection ships.

## Retries and repeated commits

A trusted default-branch controller cancels superseded revisions and builds for
closed or draft PRs. The controller uses Actions write permission only to request
cancellation. It reads current PR state, executes no PR code or artifacts, and
publishes no statuses. It reconciles PR lifecycle changes and attempts entering
`in_progress`, including partial reruns that retain a successful preparation job.
Revision-specific concurrency groups prevent an old
rerun from canceling a newer head and replace duplicate queued runs. CI rejects known
obsolete heads before building and verifies the current head before passing the
aggregate. Cancellation never makes a required check pass. Inspect a failure and
use GitHub Actions to rerun failed jobs or the complete run. A rerun keeps the
immutable source revision. A code fix needs a new commit, local reviews, and CI on
that revision.

For a same-repository PR, a repeated run can reuse successful test evidence for
its exact source commit and workflow revision. CI checks the prior run's source,
repository, event, completed result, reusable workflow revision, and every
required job. Missing or unreadable evidence causes a fresh build. Fork PRs build
fresh. The run summary links the original evidence when CI reuses it.

Reuse skips duplicate tests, without downloading or executing prior build
outputs. Main builds and releases do not consume PR evidence or PR artifacts.
Every push to `main` retains its own complete build.

## Main builds and releases

The `ci` workflow continues to run on every `main` push. Successful runs call the
existing release workflow to publish a release candidate. Stable releases and
Store packaging keep their separate workflow and authorization rules. PR CI
cannot publish a release.

If GitHub drops a push event and no main run starts, do not claim that a main
build or release candidate was produced. An up-to-date PR may still have tested
the exact merged tree. With the merge and reviewed head SHAs, compare them using
`git rev-parse "$merge_sha^{tree}" "$task_reviewed_sha^{tree}"`. Report the missing
run and release candidate. The next successful main build includes the change.

## Repository settings and migration

Protect `main`, including administrators, with a pull request requirement,
strict up-to-date checks, resolved review conversations, and required
`reviewed-ci` from GitHub Actions. Preserve any stricter approval requirements.
Require the aggregate check rather than individual platform or reviewer checks.

The previous manual `Start CI` workflow and status publishers are removed. The
PR lifecycle workflow retains only cancellation. This migration keeps the
required check name and its GitHub Actions source. The first
migration PR must pass its actual automatic full CI before merging. Do not change
branch protection to make that PR merge or replace old results with synthetic
successes.

Rebase existing PR branches onto the migrated `main`, complete the local
reviews, and push the new head before merging. Legacy statuses stay on their
older commits. If a legacy status still blocks the current revision, report the
exact status and head rather than invoking the obsolete bot gate or lifting
protection. A future merge queue needs its own `merge_group` testing policy
before enabling it.
