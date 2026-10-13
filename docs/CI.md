# Pull request CI

Regular pull requests run inexpensive repository checks immediately. CodeRabbit
reviews each new PR and subsequent commit, including rebases. Full Linux and
Windows CI starts only after an authorized maintainer or coding agent completes
the review process and invokes **Start CI**.

Read the latest review, summary, and threads. Validate findings against the source,
fix valid issues, and explain disagreements before resolving their threads. The
summary can contain findings outside the diff that GitHub cannot attach inline.
Dispatching CI attests that this work is complete and that the reviewed code may
execute on GitHub-hosted runners. The workflow can verify bot evidence and thread
resolution, but it cannot establish that a written explanation is correct.

After setting the PR number and its full current head SHA, run:

```sh
gh workflow run reviewed-ci.yml --ref main \
  -f pr_number="$task_pr_number" -f expected_sha="$task_reviewed_sha"
```

The **Actions → Start CI → Run workflow** form accepts the same inputs. Use the
workflow on `main`. The caller needs repository write access. The gate rejects
closed or draft PRs, a changed head SHA, a branch behind `main`, unresolved review
threads, and incomplete reviews. PRs must target `main`.

CodeRabbit's `Review completed` status is insufficient on its own because a
skipped review can also publish that status. The gate verifies the bot's identity
and requires its summary's `final_review_risk_coverage` to say `reviewed` for the
exact head SHA. Missing, paused, skipped, rate-limited, or failed reviews leave
the gate closed. If an incremental review skips a rebase, request a full review.
When repository policy requires formal approval, GitHub's review decision and
CodeRabbit's formal approval must also cover the current revision.

## Results and retries

Every checkout in full CI uses the immutable authorized head SHA. Write-enabled
controller jobs use the trusted workflow revision and never run PR code or load
its artifacts. Build jobs have read-only repository permissions and receive no
repository secrets. They still execute reviewed code in a default-branch Actions
run, so authorization is a trust decision, including for changes from forks.
Dependency cache keys are separate for each PR and for `main`. This prevents
accidental cache contamination, but does not isolate deliberately malicious code
from the runner's Actions runtime credentials.

The required `reviewed-ci` commit status appears on the PR head, with a link to
the dispatched run. It succeeds only after repository checks, both Linux test
configurations, Windows tests and MSI verification, and Windows MSIX verification
all finish successfully. A skipped or deferred build never creates a successful
status. Cancellation or a failed reporting job leaves a failure through the
separate lifecycle workflow.

Requests for one PR run serially. An existing attempt is reused unless a new
dispatch explicitly sets `retry=true`. Overlapping requests are deduplicated,
including retries submitted before the preceding attempt finishes. For an
intentional retry after completion, use:

```sh
gh workflow run reviewed-ci.yml --ref main \
  -f pr_number="$task_pr_number" -f expected_sha="$task_reviewed_sha" -f retry=true
```

Do not use GitHub's ordinary rerun button. A new dispatch repeats authorization.
Every push or rebase needs a new review and CI authorization. The lifecycle
workflow cancels obsolete PR runs without canceling builds for `main`. If a GitHub
API outage prevents cleanup, the required status stays unsuccessful until the
controller can verify a result.

## Repository settings

Protect `main`, including administrators, with a pull request requirement, strict
up-to-date status checks, resolved review conversations, and required `reviewed-ci`
from GitHub Actions. Do not require individual job checks from a dispatched run,
because GitHub attaches those checks to the dispatch branch. Retain any stricter
review requirements if the policy changes later.

Full CI does not subscribe to PR events. External CI integrations must also leave
automatic expensive PR builds disabled. No external CI webhook or merge queue was
configured when this workflow was introduced. Any future merge queue needs its
own `merge_group` testing policy before enabling it.

Each `main` push retains its own complete CI run and existing prerelease pipeline.
Stable releases and Store packaging keep their separate workflow. The existing
manual MSIX diagnostic runs on `main` and cannot satisfy `reviewed-ci`.

Reviews and successful CI establish merge eligibility. Merging remains a separate
authorized action through the PR, with the expected head SHA checked at merge time.
