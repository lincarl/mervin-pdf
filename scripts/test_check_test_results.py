"""Exercise the JUnit gate with complete results and independently malformed reports."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class TestRequiredResults(unittest.TestCase):
    """Run the real command against a fixed two-test inventory."""

    def setUp(self):
        """Create reports outside the user's configuration directories."""
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.expected = self.root / "expected.json"
        self.expected.write_text(json.dumps({"tests": [{"name": "alpha"}, {"name": "beta"}]}))

    def run_report(self, body):
        """Return the gate's status and diagnostics for one supplied report."""
        report = self.root / "results.xml"
        report.write_text(body)
        return subprocess.run(
            [sys.executable, str(Path(__file__).with_name("check-test-results.py")),
             str(report), "--expected", str(self.expected)],
            capture_output=True, text=True, timeout=10)

    def test_accepts_complete_results_in_any_order(self):
        """Completion order does not affect required membership."""
        result = self.run_report(
            '<testsuites><testsuite failures="0"><testcase name="beta" status="run"/>'
            '<testcase name="alpha"/></testsuite></testsuites>')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_missing_duplicate_and_unexpected_tests(self):
        """A successful subset or duplicate cannot stand in for a missing test."""
        for names in ([], ["alpha"], ["alpha", "alpha"], ["alpha", "beta", "extra"]):
            with self.subTest(names=names):
                cases = "".join(f'<testcase name="{name}"/>' for name in names)
                self.assertNotEqual(self.run_report(f"<testsuite>{cases}</testsuite>").returncode, 0)

    def test_rejects_every_unsuccessful_outcome(self):
        """Errors and not-run cases fail alongside ordinary failures and skips."""
        for child in ("failure", "error", "skipped"):
            with self.subTest(child=child):
                result = self.run_report(
                    f'<testsuite><testcase name="alpha"><{child}/></testcase>'
                    '<testcase name="beta"/></testsuite>')
                self.assertNotEqual(result.returncode, 0)
        for attributes in ('status="notrun"', 'status="disabled"', 'name=""'):
            with self.subTest(attributes=attributes):
                result = self.run_report(
                    f'<testsuite><testcase {attributes}/><testcase name="beta"/></testsuite>')
                self.assertNotEqual(result.returncode, 0)

    def test_rejects_invalid_documents_and_summary_errors(self):
        """Malformed or contradictory reports do not pass the gate."""
        for report in ("not XML", "<other/>",
                       '<testsuite errors="1"><testcase name="alpha"/>'
                       '<testcase name="beta"/></testsuite>'):
            with self.subTest(report=report):
                self.assertNotEqual(self.run_report(report).returncode, 0)
        self.expected.write_text('{"tests": []}')
        result = self.run_report('<testsuite><testcase name="alpha"/></testsuite>')
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
