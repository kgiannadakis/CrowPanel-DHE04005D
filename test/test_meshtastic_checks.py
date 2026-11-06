"""Test the policy runner without invoking a compiler or accessing a device."""

from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "meshtastic_policies", ROOT / "meshtastic/test/host/check_policies.py")
CHECKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKS)


class MeshtasticCheckTests(unittest.TestCase):
    def run_with_compiler(self, compiler, returncode=0):
        with patch.object(CHECKS, "find_compiler", return_value=compiler), \
             patch.object(CHECKS.subprocess, "run", return_value=SimpleNamespace(returncode=returncode)) as run, \
             redirect_stdout(io.StringIO()):
            result = CHECKS.main([])
        return result, run.call_args_list

    def test_native_compiler_executes_every_policy(self):
        result, calls = self.run_with_compiler("g++")
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 2 * len(CHECKS.TESTS))
        for index in range(0, len(calls), 2):
            self.assertEqual(calls[index].args[0][0], "g++")
            self.assertNotIn("-fsyntax-only", calls[index].args[0])
            self.assertEqual(len(calls[index + 1].args[0]), 1)

    def test_cross_compiler_never_executes_foreign_binaries(self):
        result, calls = self.run_with_compiler("riscv32-esp-elf-g++.exe")
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), len(CHECKS.TESTS))
        self.assertTrue(all("-fsyntax-only" in call.args[0] for call in calls))

    def test_failed_compile_stops_without_running_output(self):
        result, calls = self.run_with_compiler("g++", returncode=1)
        self.assertEqual(result, 1)
        self.assertEqual(len(calls), 1)

    def test_zig_uses_the_cpp_driver(self):
        result, calls = self.run_with_compiler("zig.exe")
        self.assertEqual(result, 0)
        self.assertEqual(calls[0].args[0][:2], ["zig.exe", "c++"])

    def test_all_listed_tests_exist(self):
        for filename in CHECKS.TESTS:
            self.assertTrue((ROOT / "meshtastic/test/host" / filename).is_file(), filename)


if __name__ == "__main__":
    unittest.main()
