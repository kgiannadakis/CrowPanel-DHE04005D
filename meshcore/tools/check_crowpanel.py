#!/usr/bin/env python3
"""Run CrowPanel source checks and compile-time policy assertions, without flashing."""

import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
POLICY_TESTS = (
    "test_awake_idle_policy/test_awake_idle_policy.cpp",
    "test_battery_policy/test_battery_policy.cpp",
    "test_brightness_policy/test_brightness_policy.cpp",
    "test_display_backlight/test_display_backlight.cpp",
    "test_neighbour_map/test_neighbour_contacts.cpp",
    "test_neighbour_map/test_neighbour_map.cpp",
    "test_repeater_password/test_repeater_password.cpp",
    "test_screen_power_policy/test_screen_power_policy.cpp",
    "test_screen_wake_recovery/test_screen_wake_recovery.cpp",
    "test_stable_power_defaults/test_stable_power_defaults.cpp",
    "test_stc8_backlight_start/test_stc8_backlight_start.cpp",
    "test_stc8_battery_state/test_stc8_battery_state.cpp",
    "test_touch_wake_policy/test_touch_wake_policy.cpp",
)


def find_compiler(explicit=None):
    for candidate in (explicit, os.environ.get("CXX"), "g++", "clang++"):
        if candidate:
            executable = shutil.which(candidate)
            if executable:
                return executable
            if Path(candidate).is_file():
                return str(Path(candidate).resolve())
            if explicit:
                return None
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    name = "riscv32-esp-elf-g++.exe" if os.name == "nt" else "riscv32-esp-elf-g++"
    for parent in (core / "packages", core / "tools"):
        for candidate in sorted(parent.glob(f"toolchain-riscv*/bin/{name}")):
            return str(candidate)
    return None


def python_checks():
    suite = unittest.TestSuite()
    for path in sorted((ROOT / "test").glob("test_*/test_*.py")):
        spec = importlib.util.spec_from_file_location(f"crowpanel_{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
    if not suite.countTestCases():
        raise RuntimeError("No Python checks were discovered")
    return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()


def policy_checks(compiler):
    for relative in POLICY_TESTS:
        print(f"Checking {relative}", flush=True)
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
             str(ROOT / "test" / relative)], cwd=ROOT, check=False,
        )
        if result.returncode:
            return False
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++17 compiler executable (host or ESP32 RISC-V)")
    parser.add_argument("--python-only", action="store_true", help="Skip C++ compile-time checks explicitly")
    args = parser.parse_args(argv)
    if not python_checks():
        return 1
    if args.python_only:
        print("Python checks passed. C++ policy checks were explicitly skipped.")
        return 0
    compiler = find_compiler(args.compiler)
    if not compiler:
        print("C++ compiler not found. Supply --compiler PATH or use --python-only.", file=sys.stderr)
        return 2
    print(f"Compiler: {compiler}", flush=True)
    if not policy_checks(compiler):
        return 1
    print(f"Python checks and {len(POLICY_TESTS)} C++ policy checks passed. No device was flashed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
