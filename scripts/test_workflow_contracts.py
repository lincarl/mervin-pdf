#!/usr/bin/env python3
"""Check the workflow wiring and execute the required PR gate's truth table."""

import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def workflow(name):
    """Parse YAML through the same Ruby standard library used by repository CI."""
    result = subprocess.run(
        ["ruby", "-rjson", "-ryaml", "-e",
         'doc = YAML.safe_load(File.read(ARGV[0]), aliases: false); '
         'doc["on"] = doc.delete(true) if doc.key?(true); puts JSON.generate(doc)',
         str(ROOT / ".github/workflows" / name)],
        capture_output=True, text=True, check=True,
    )
    return json.loads(result.stdout)


class WorkflowContractTests(unittest.TestCase):
    def require_effective_step(self, job, step):
        """Reject a disabled or advisory check at either execution level."""
        self.assertNotIn("continue-on-error", job)
        self.assertNotIn("continue-on-error", step)
        self.assertNotIn("if", step)

    def test_pr_builds_immutable_source_and_uses_current_workflow_controls(self):
        pr = workflow("pr-ci.yml")
        self.assertEqual(pr["on"], {"pull_request": {
            "branches": ["main"], "types": ["opened", "reopened", "synchronize", "ready_for_review"],
        }})
        self.assertEqual(pr["permissions"], {"contents": "read"})
        self.assertEqual(pr["concurrency"], {
            "group": "pr-ci-auto-${{ github.event.pull_request.number }}-${{ github.event.pull_request.head.sha }}",
            "cancel-in-progress": True,
        })
        jobs = pr["jobs"]
        for name in ("prepare", "reviewed-ci"):
            checkout = next(step for step in jobs[name]["steps"] if "uses" in step)
            self.assertTrue(checkout["uses"].startswith("actions/checkout@"))
            self.assertEqual(checkout["with"]["ref"], "${{ github.workflow_sha }}")
            self.assertIs(checkout["with"]["persist-credentials"], False)
        build = jobs["full-ci"]
        self.assertEqual(build["uses"], "./.github/workflows/full-ci.yml")
        self.assertEqual(build["with"], {
            "sha": "${{ needs.prepare.outputs.head_sha }}",
            "repository": "${{ needs.prepare.outputs.head_repository }}",
        })
        self.assertEqual(build["if"], "needs.prepare.outputs.reuse_run == ''")
        self.assertEqual(build["permissions"], {"actions": "read", "contents": "read"})
        self.assertEqual(build["cache-mode"], "read")
        gate = jobs["reviewed-ci"]
        self.assertEqual(gate["needs"], ["prepare", "full-ci"])
        self.assertEqual(gate["if"], "always()")
        for step in gate["steps"]:
            self.require_effective_step(gate, step)
        self.assertEqual(gate["steps"][-2]["run"], "python3 -B scripts/reviewed_ci.py verify-head")
        self.assertEqual(gate["steps"][-1]["env"], {
            "PREPARE_RESULT": "${{ needs.prepare.result }}",
            "BUILD_RESULT": "${{ needs.full-ci.result }}",
            "REUSED_RUN": "${{ needs.prepare.outputs.reuse_run }}",
        })

    def test_actual_required_gate_rejects_every_unverified_combination(self):
        script = workflow("pr-ci.yml")["jobs"]["reviewed-ci"]["steps"][-1]["run"]
        statuses = ("success", "failure", "cancelled", "skipped", "")
        for prepare, build, reused in itertools.product(statuses, statuses, ("", "81", "081", "bad")):
            expected = prepare == "success" and ((not reused and build == "success")
                                                 or (reused == "81" and build == "skipped"))
            with self.subTest(prepare=prepare, build=build, reused=reused):
                result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                    env={**os.environ, "PREPARE_RESULT": prepare, "BUILD_RESULT": build, "REUSED_RUN": reused})
                self.assertEqual(result.returncode == 0, expected, result.stderr)

    def test_lifecycle_uses_trusted_controls_and_observes_started_reruns(self):
        lifecycle = workflow("pr-ci-lifecycle.yml")
        self.assertEqual(lifecycle["on"]["workflow_run"], {"workflows": ["PR CI"], "types": ["in_progress"]})
        self.assertEqual(lifecycle["on"]["pull_request_target"]["types"],
                         ["synchronize", "closed", "converted_to_draft", "reopened", "ready_for_review"])
        job = lifecycle["jobs"]["cancel-obsolete"]
        self.assertEqual(job["steps"][0]["with"], {
            "ref": "refs/heads/${{ github.event.repository.default_branch }}", "persist-credentials": False,
        })
        commands = [step for step in job["steps"] if "run" in step]
        self.assertEqual([step["run"] for step in commands], ["python3 -B scripts/cancel_pr_ci.py"])
        self.require_effective_step(job, commands[0])

    def test_full_ci_gate_requires_all_test_aggregates(self):
        jobs = workflow("full-ci.yml")["jobs"]
        gate = jobs["required-jobs"]
        required = ["repository-checks", "linux-tests", "linux-system-qt-tests", "windows-tests", "windows-msix-tests"]
        self.assertCountEqual(gate["needs"], required)
        self.assertEqual(gate["if"], "always()")
        for step in gate["steps"]:
            self.require_effective_step(gate, step)
        self.assertEqual(gate["steps"][-1]["run"], "python3 -B scripts/reviewed_ci.py check-results")
        self.assertEqual(gate["steps"][-1]["env"], {
            "CI_REPOSITORY_RESULT": "${{ needs.repository-checks.result }}",
            "CI_LINUX_RESULT": "${{ needs.linux-tests.result }}",
            "CI_LINUX_SYSTEM_RESULT": "${{ needs.linux-system-qt-tests.result }}",
            "CI_WINDOWS_RESULT": "${{ needs.windows-tests.result }}",
            "CI_MSIX_RESULT": "${{ needs.windows-msix-tests.result }}",
            "CI_DIAGNOSTIC": "${{ inputs.msix_artifact_run != '' }}",
        })

    def test_reusable_workflow_pins_every_source_and_control_checkout(self):
        jobs = workflow("full-ci.yml")["jobs"]
        source = {"repository": "${{ inputs.repository || github.repository }}",
                  "ref": "${{ inputs.sha }}", "persist-credentials": False}
        controls = {"ref": "${{ github.workflow_sha }}", "persist-credentials": False}
        expected = {
            "repository-checks": [source, {**controls, "path": "dev-temp/workflow-controls"}],
            "linux-tests": [source], "linux-system-qt-tests": [source],
            "windows-tests": [source], "windows-msix-tests": [source],
            "required-jobs": [controls],
        }
        self.assertEqual(set(jobs), set(expected))
        for name, job in jobs.items():
            with self.subTest(job=name):
                checkouts = [step for step in job["steps"]
                             if step.get("uses", "").startswith("actions/checkout@")]
                self.assertEqual([step["with"] for step in checkouts], expected[name])
                for step in checkouts:
                    self.require_effective_step(job, step)
                    self.assertRegex(step["uses"], r"^actions/checkout@[0-9a-f]{40}$")

    def test_diagnostic_validation_precedes_artifact_download(self):
        caller = workflow("ci.yml")["jobs"]["msix-diagnostic"]
        self.assertEqual(caller["if"],
                         "github.event_name == 'workflow_dispatch' && inputs.msix_artifact_run != ''")
        self.assertEqual(caller["cache-mode"], "read")
        self.assertEqual(caller["permissions"], {"actions": "read", "contents": "read"})
        self.assertEqual(caller["with"], {"sha": "${{ github.sha }}",
                                        "msix_artifact_run": "${{ inputs.msix_artifact_run }}"})
        job = workflow("full-ci.yml")["jobs"]["windows-msix-tests"]
        steps = job["steps"]
        validation = next(i for i, step in enumerate(steps)
                          if step.get("run") == "python -B scripts/reviewed_ci.py diagnostic")
        download = next(i for i, step in enumerate(steps)
                        if step.get("uses", "").startswith("actions/download-artifact@"))
        self.assertLess(validation, download)
        self.assertEqual(steps[validation]["if"], "inputs.msix_artifact_run != ''")
        self.assertNotIn("continue-on-error", steps[validation])
        self.require_effective_step(job, steps[download])
        self.assertEqual(steps[download]["with"]["run-id"],
                         "${{ inputs.msix_artifact_run || github.run_id }}")

    def test_shell_syntax_gate_checks_every_script(self):
        steps = workflow("full-ci.yml")["jobs"]["repository-checks"]["steps"]
        script = next(step["run"] for step in steps if step.get("name") == "Validate metadata and scripts")
        gate = re.search(r"(?m)^for script in .*?^done$", script, re.DOTALL)
        self.assertIsNotNone(gate)
        paths = ["scripts/build-mupdf-linux.sh", "scripts/make-appimage.sh",
                 "packaging/linux/postinst", "packaging/linux/postrm",
                 "packaging/linux/rpm-post.sh", "packaging/linux/rpm-postun.sh"]
        temporary_root = ROOT / "dev-temp"
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="shell-gate-", dir=temporary_root) as temporary:
            directory = Path(temporary)
            for path in paths:
                target = directory / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("#!/bin/bash\ntrue\n")
            for invalid in [None, *paths]:
                with self.subTest(invalid=invalid):
                    if invalid:
                        (directory / invalid).write_text("#!/bin/bash\nif then\n")
                    result = subprocess.run(["bash", "-e", "-c", gate.group()], cwd=directory,
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, invalid is None, result.stderr)
                    if invalid:
                        (directory / invalid).write_text("#!/bin/bash\ntrue\n")

    def test_main_pushes_and_releases_keep_independent_runs(self):
        ci = workflow("ci.yml")
        self.assertEqual(ci["on"]["push"], {"branches": ["main"]})
        self.assertEqual(ci["concurrency"], {
            "group": "ci-${{ github.event_name == 'push' && github.run_id || github.ref }}",
            "cancel-in-progress": "${{ github.event_name != 'push' }}",
        })
        release = ci["jobs"]["prerelease"]
        self.assertEqual(release["needs"], "full-ci")
        self.assertEqual(release["uses"], "./.github/workflows/release.yml")
        self.assertEqual(release["if"], "github.event_name == 'push' && github.ref == 'refs/heads/main'")
        packaging = workflow("release.yml")
        self.assertEqual(packaging["on"]["push"], {"tags": ["v*"]})
        self.assertIs(packaging["concurrency"]["cancel-in-progress"], False)

    def test_linux_release_dependencies_are_bundled_and_legacy_lane_remains(self):
        full = workflow("full-ci.yml")["jobs"]
        legacy = "\n".join(step.get("run", "") for step in full["linux-tests"]["steps"])
        self.assertIn("qpdf-11.10.0.tar.gz", legacy)
        release = workflow("release.yml")["jobs"]
        for job in [full["linux-system-qt-tests"], *(release[name] for name in ("appimage", "deb", "rpm"))]:
            with self.subTest(runner=job["runs-on"], container=job.get("container")):
                steps = job["steps"]
                scripts = "\n".join(step.get("run", "") for step in steps)
                self.assertIn("cmake --preset linux-bundled", scripts)
                self.assertIn("scripts/bootstrap-vcpkg-linux.sh", scripts)
                self.assertIn("scripts/install-build-tools.py", scripts)
                verification = next(step for step in steps if "scripts/check-linux-runtime.py" in step.get("run", ""))
                self.require_effective_step(job, verification)
                for distro_library in ("qt6-base-dev", "qt6-qtbase-devel", "libqpdf-dev", "qpdf-devel", "libtomlplusplus-dev"):
                    self.assertNotIn(distro_library, scripts)
                qt = next(step for step in steps if step.get("uses", "").startswith("jurplel/install-qt-action@"))
                self.assertEqual(qt["with"]["version"], "6.12.0")
                self.assertEqual(qt["with"]["py7zrversion"], "==1.1.4")
                self.assertNotIn("archives", qt["with"])
                if job.get("container") == "fedora:44":
                    self.assertIs(qt["with"].get("install-deps"), False)
                    self.assertEqual(qt["env"]["CC"], "gcc")
                    self.assertEqual(qt["env"]["CXX"], "g++")
                    self.assertEqual(qt["env"]["LDSHARED"], "gcc -shared")
                self.require_effective_step(job, qt)
        presets = json.loads((ROOT / "CMakePresets.json").read_text())
        bundled = next(preset for preset in presets["configurePresets"] if preset["name"] == "linux-bundled")
        self.assertEqual(bundled["cacheVariables"]["VCPKG_TARGET_TRIPLET"], "x64-linux-dynamic")
        self.assertEqual(bundled["cacheVariables"]["MERVIN_BUNDLE_LINUX_RUNTIME"], "ON")


if __name__ == "__main__":
    unittest.main()
