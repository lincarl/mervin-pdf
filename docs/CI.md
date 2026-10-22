# Pull request CI

Complete local code reviews and valid fixes before pushing the branch and opening
a regular pull request. Automatic GitHub CodeRabbit reviews are disabled. PR creation,
new commits, reopening, and marking a draft ready run full CI automatically.
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
- Linux release and sanitizer tests on Ubuntu 24.04 with aqt's Qt and qpdf 11.
- Linux tests on Ubuntu 26.04 with the distribution's Qt and qpdf, matching the
  AppImage and DEB toolchain.
- Windows tests, MSI installation, upgrade, repair, uninstall, and wizard checks.
- Windows MSIX installation, activation, upgrade, and uninstall checks.

A final job explicitly checks every expected result. The outer `reviewed-ci`
check requires that job and the rest of full CI to succeed. The optional MSIX
artifact diagnostic remains a separate manual `main` run. Its reduced set of
checks cannot satisfy the PR gate or provide reusable full CI evidence. Diagnostic
runs have read-only caches and accept only retained Windows verification artifacts
from a completed main-push CI run with successful Windows build, test, MSI
verification, and artifact-upload steps. MSIX packaging or testing may have
failed, since that can be the reason to diagnose the package.

The Fedora RPM retains its exported-symbol check in the release workflow.
Verification artifacts remain on the run that produced them, with their existing
retention periods.

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
