"""Exercise validation-runner modes without invoking a compiler or device."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
from io import StringIO
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_crowpanel_runner", ROOT / "tools/check_crowpanel.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class CheckRunnerTests(unittest.TestCase):
    def run_quietly(self, args):
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            return RUNNER.main(args)

    def test_python_only_is_explicit_and_does_not_find_a_compiler(self):
        with patch.object(RUNNER, "python_checks", return_value=True), patch.object(RUNNER, "find_compiler") as find:
            self.assertEqual(self.run_quietly(["--python-only"]), 0)
            find.assert_not_called()

    def test_failed_python_checks_prevent_policy_compile(self):
        with patch.object(RUNNER, "python_checks", return_value=False), patch.object(RUNNER, "policy_checks") as policies:
            self.assertEqual(self.run_quietly([]), 1)
            policies.assert_not_called()

    def test_missing_compiler_does_not_report_full_success(self):
        with patch.object(RUNNER, "python_checks", return_value=True), patch.object(RUNNER, "find_compiler", return_value=None):
            self.assertEqual(self.run_quietly([]), 2)

    def test_policy_failure_is_reported(self):
        with patch.object(RUNNER, "python_checks", return_value=True), patch.object(RUNNER, "find_compiler", return_value="compiler"), patch.object(RUNNER, "policy_checks", return_value=False):
            self.assertEqual(self.run_quietly([]), 1)
