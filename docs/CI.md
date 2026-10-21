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

Repository writers with permission to edit workflows are trusted maintainers.
The `main` check prevents accidental dispatch from another ref. It cannot stop a
writer from creating another workflow that publishes a status with its own
GitHub Actions token. Defending against a malicious workflow maintainer requires
a separate status-publishing app with credentials that branch workflows cannot
access. Changing the dispatch trigger alone would not provide that protection.

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
repository secrets. They still execute reviewed code, so authorization is a trust
decision, including for changes from forks. The caller sets `cache-mode: read`.
GitHub enforces read-only cache access through scoped runtime tokens, including
inside the reusable workflow. PR builds can restore existing dependency caches
but cannot save caches that a later main or release build might consume.

The required `reviewed-ci` commit status appears on the PR head, with a link to
the dispatched run. It succeeds only after repository checks, all three Linux test
jobs, Windows tests and MSI verification, and Windows MSIX verification all finish
successfully. Two Linux jobs build release and sanitizer configurations on Ubuntu
24.04 with aqt's Qt and qpdf 11. The third builds on Ubuntu 26.04 with the
distribution's Qt and qpdf, the toolchain of the AppImage and .deb. The Fedora .rpm
gets only the exported-symbol check in the release workflow. A skipped or deferred
build never creates a successful status. Cancellation or a failed reporting job
leaves a failure through the separate lifecycle workflow.

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

After a merge, GitHub's push event starts the `ci` workflow on `main`, which repeats
the full CI and publishes a release candidate. If GitHub drops that event, as it
did for `310f6f1` in October 2026, no run starts, and no new CI run is needed. The
PR was up to date with `main` when it merged, so the merge commit has the same tree
as the head that passed `reviewed-ci`, and this prints the same hash twice:

```sh
git rev-parse "$merge_sha^{tree}" "$task_reviewed_sha^{tree}"
```

Only the release candidate is missing. The next push to `main` whose CI passes
publishes one that includes the change.

## When the gate rejects its own fix

Start CI, dispatched on `main` as described above, uses the gate in
`scripts/reviewed_ci.py` and the full CI workflow from `main`, not the versions in
the PR. So the old gate, not the new one, judges a PR that changes the gate. If the
gate on `main` wrongly rejects every PR, the PR that fixes it cannot get
`reviewed-ci` either, and branch protection blocks the merge for administrators too.
This happened in October 2026, when CodeRabbit began starting its comments with a
`<!-- coderabbit-ai-generated:v1 -->` line and the gate no longer found the summary.

The steps below apply only to a PR that fixes a fault on `main` in the gate or its
workflows that also blocks that PR, and only with the repository owner's approval
for that PR and head. Because the PR merges without full CI, it may change only
`scripts/reviewed_ci.py`, `scripts/test_reviewed_ci.py`, files in
`.github/workflows/`, and `docs/CI.md`. When the gate rejects a PR for a reason in
that PR, such as an incomplete review, fix the PR instead.

1. Finish the fix PR's review as usual. CodeRabbit must cover its head SHA, every
   review thread must be resolved, and `repository-checks`, which runs the gate's
   tests, must pass.
2. Collect the evidence for the final head. Set `fix_pr_number` and `fix_head_sha`
   to the fix PR's number and full head SHA, dispatch Start CI for that head, and
   keep the failing run. Fetch `origin/main` and read the PR's change to
   `scripts/reviewed_ci.py`, because the next command runs it with your GitHub
   login, which can change branch protection. Then run the read-only `inspect`
   command below once from a checkout of `origin/main` and once from a checkout of
   the PR's head.

   ```sh
   python3 -I -B scripts/reviewed_ci.py inspect --pr "$fix_pr_number" --sha "$fix_head_sha"
   ```

   From the PR's head, it must print the authorization evidence, which shows that
   the review is complete and the branch is up to date. When the fault is in the
   gate's review checks, the run from `origin/main` must fail with the same error as
   the failing Start CI run. A fault elsewhere, such as in the gate's `start` or
   `finish` commands or in a workflow, does not show in `inspect`, so the failing run
   is its evidence.
3. Ask the owner to approve the exception for the PR number and full head SHA. Send
   the failing run, both `inspect` results, the `repository-checks` result, the PR's
   diff, and the merge commit subject. A general request to merge is not this
   approval, and a new head SHA needs a new approval. When you start step 4, tell
   the owner and name the directory you run it in. If no report from step 5
   follows, the owner can then check the required checks.
4. Copy the script below from `main`'s version of this document, for example with
   `git show origin/main:docs/CI.md`, not from the fix PR. Save it as
   `merge-gate-fix.sh` in an empty directory outside the checkout, so that cleaning
   up the checkout cannot delete the files it saves. Set `merge_subject` to the
   merge commit subject that the project's rules require, then run
   `bash merge-gate-fix.sh "$fix_pr_number" "$fix_head_sha" "$merge_subject"` there.

   Before it changes anything, the script checks its arguments, that `reviewed-ci`
   from GitHub Actions is required, that auto-merge is off, that the PR is open,
   targets `main` and has the given head, that the head is ahead of `main`, that
   `repository-checks` passed on it, and that the PR changes only the files listed
   above, including the old names of renamed files. It then saves the protection
   settings, removes only that `reviewed-ci` check, stops if `main` has moved,
   merges the PR, and puts the saved checks back, even if the merge fails or the
   script is interrupted. If someone else changes the required checks meanwhile,
   it stops instead of overwriting them. The lift, the merge, the restore, and
   every failed check after the argument check go to `merge-gate-fix.log` with the
   time. `Protection restored` appears only when the settings match the saved copy.
   Otherwise the script exits with status 1.

   ```sh
   set -euo pipefail
   pr=${1-} sha=${2-} subject=${3-}
   if [[ ! $pr =~ ^[1-9][0-9]*$ || ! $sha =~ ^[0-9a-f]{40}$ || -z $subject ]]; then
     echo "Usage: bash merge-gate-fix.sh PR_NUMBER FULL_HEAD_SHA MERGE_SUBJECT" >&2
     exit 2
   fi
   repo=lincarl/mervin-pdf
   protection="repos/$repo/branches/main/protection"
   checks="$protection/required_status_checks"
   unset GH_FORCE_TTY CLICOLOR_FORCE
   note() {
     printf '%s %s\n' "$(date -u +%FT%TZ)" "$1" >> merge-gate-fix.log
     printf '%s %s\n' "$(date -u +%FT%TZ)" "$1" >&2 || true
   }
   stop() { note "$1"; exit 1; }
   before=$(gh api "$checks" --jq '{strict, checks}')
   required=$(gh api "$checks" --jq 'any(.checks[]; .context == "reviewed-ci" and .app_id == 15368)')
   [[ $required == true ]] ||
     stop "reviewed-ci is not required. If an earlier run lifted it, restore checks-before.json first."
   auto_merge=$(gh api "repos/$repo" --jq .allow_auto_merge)
   [[ $auto_merge == false ]] || stop "Auto-merge is on, so another PR could merge while reviewed-ci is lifted."
   pr_state=$(gh pr view "$pr" --repo "$repo" --json state,isDraft,baseRefName,headRefOid \
     --jq '"\(.state) \(.isDraft) \(.baseRefName) \(.headRefOid)"')
   [[ $pr_state == "OPEN false main $sha" ]] || stop "PR $pr is not an open PR into main at $sha: $pr_state"
   main_sha=$(gh api "repos/$repo/git/ref/heads/main" --jq .object.sha)
   status=$(gh api "repos/$repo/compare/$main_sha...$sha" --jq .status)
   [[ $status == ahead ]] || stop "The PR is $status relative to main. Update it and repeat steps 1 to 3."
   tests=$(gh api "repos/$repo/commits/$sha/check-runs?check_name=repository-checks" \
     --jq '[.check_runs[] | select(.app.id == 15368)][0].conclusion')
   [[ $tests == success ]] || stop "repository-checks has not passed on $sha."
   files=$(gh api "repos/$repo/compare/$main_sha...$sha" --jq \
     'if (.files | length) < 300 then .files[] | .filename, (.previous_filename // empty) else error("too many files") end')
   outside=$(grep -v -E '^(scripts/(test_)?reviewed_ci\.py|\.github/workflows/[^/]+|docs/CI\.md)$' <<< "$files" || true)
   if [[ -z $files || -n $outside ]]; then
     stop "The PR changes files outside the gate: ${outside:-none listed}"
   fi
   gh api "$protection" > protection-before.json
   printf '%s\n' "$before" > checks-before.json
   lifted=$(gh api "$checks" --jq \
     '{strict, checks: [.checks[] | select(.context != "reviewed-ci" or .app_id != 15368)]}')
   current=$(gh api "$checks" --jq '{strict, checks}')
   [[ $current == "$before" ]] || stop "The required checks changed while the script ran. Run it again."
   restore() {
     trap '' INT TERM HUP PIPE
     for _ in 1 2 3; do
       if current=$(gh api "$checks" --jq '{strict, checks}'); then
         [[ $current == "$before" || $current == "$lifted" ]] ||
           stop "The required checks changed while reviewed-ci was lifted. Add reviewed-ci back by hand."
         if gh api -X PATCH "$checks" --input checks-before.json > /dev/null &&
           gh api "$protection" > protection-after.json &&
           diff protection-before.json protection-after.json; then
           note "Protection restored"
           return
         fi
       fi
       sleep 5
     done
     stop "Protection does not match protection-before.json. Check that reviewed-ci is required, then compare the settings."
   }
   trap restore EXIT
   trap 'exit 130' INT TERM HUP PIPE
   note "Lifting reviewed-ci for PR $pr at $sha"
   printf '%s\n' "$lifted" | gh api -X PATCH "$checks" --input - > /dev/null
   main_now=$(gh api "repos/$repo/git/ref/heads/main" --jq .object.sha)
   [[ $main_now == "$main_sha" ]] || stop "main moved while reviewed-ci was lifted. Repeat steps 1 to 3."
   gh pr merge "$pr" --repo "$repo" --merge --subject "$subject" --match-head-commit "$sha"
   merged=$(gh pr view "$pr" --repo "$repo" --json mergeCommit --jq .mergeCommit.oid) ||
     merged="a commit the script could not read. Check the PR on GitHub"
   note "Merged PR $pr as $merged"
   ```

   If `gh pr merge` fails, the script still restores the checks and exits with a
   nonzero status. Fix the cause and run the script again for the same approved
   head, never with `--admin` or `--auto`. A new head SHA needs a new approval in
   step 3. If the log shows the merge, check the PR on GitHub instead of running
   the script again.

   If the script reports that protection does not match, or the session ends before
   it restores the checks, run the commands below in the directory where the script
   ran. They put the saved checks back and compare the settings with the saved copy.
   You can also add `reviewed-ci` back under **Settings → Branches**, with GitHub
   Actions as its source. If the script reports that the required checks changed,
   use only the Settings page, so that the newer checks stay.

   ```sh
   gh api -X PATCH repos/lincarl/mervin-pdf/branches/main/protection/required_status_checks \
     --input checks-before.json
   gh api repos/lincarl/mervin-pdf/branches/main/protection > protection-after.json
   diff protection-before.json protection-after.json
   ```

5. Check that `reviewed-ci` from GitHub Actions, app ID 15368, is required again and
   that `main` gained only the merge commit. Report the merge commit and the result
   to the owner. Keep `merge-gate-fix.log`, both protection files, and the owner's
   approval with the task's records.

   ```sh
   gh api repos/lincarl/mervin-pdf/branches/main/protection/required_status_checks --jq .checks
   git fetch origin && git log --first-parent --oneline -2 origin/main
   ```

6. The fix PR got no full CI before it merged, and `main`'s CI builds and tests the
   merge commit without running Start CI. Rebase each PR that the old gate blocked.
   Wait until CodeRabbit covers the new head, and request a full review if the
   incremental review skips the rebase. Then dispatch Start CI for the new head.
   This dispatch is the first Start CI run with the fixed gate. If it rejects a PR
   that should pass, or a PR CI lifecycle run or `main`'s CI fails, stop and tell
   the owner. If no PR was waiting, tell the owner that Start CI with the fixed gate
   stays untested until the next PR. Do not repeat the exception without a new
   approval.

Only workflows on `main` publish `reviewed-ci`, and only Start CI publishes a
success. Do not add a branch workflow that publishes it, and do not edit
CodeRabbit's comments so that the old gate accepts them, because that changes the
evidence the gate checks.

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
