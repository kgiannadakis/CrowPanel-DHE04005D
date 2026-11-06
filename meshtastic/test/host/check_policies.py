#!/usr/bin/env python3
"""Compile and run SDK-independent CrowPanel policies without accessing a device."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = (
    "crowpanel_storage_test.cpp",
    "map_tile_layout_test.cpp",
    "node_retention_test.cpp",
    "node_search_test.cpp",
    "position_coordinates_test.cpp",
)


def find_compiler(explicit=None):
    candidates = [explicit] if explicit else [os.environ.get("CXX"), "g++", "clang++", "zig"]
    if not explicit:
        candidates.append(str(ROOT / ".pio/host-tools/ziglang/zig.exe"))
        core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
        name = "riscv32-esp-elf-g++.exe" if os.name == "nt" else "riscv32-esp-elf-g++"
        candidates.extend(str(path) for path in sorted(core.glob(f"packages/toolchain-riscv*/bin/{name}")))
    for candidate in candidates:
        if candidate:
            executable = shutil.which(candidate)
            if executable:
                return executable
            if Path(candidate).is_file():
                return str(Path(candidate).resolve())
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++17 compiler executable, or Zig executable")
    parser.add_argument("--compile-only", action="store_true", help="Compile without running native tests")
    args = parser.parse_args(argv)
    compiler = find_compiler(args.compiler)
    if not compiler:
        print("C++ compiler not found. Install g++, clang++ or Zig, or supply --compiler PATH.", file=sys.stderr)
        return 2
    name = Path(compiler).stem.lower()
    command = [compiler, "c++"] if name == "zig" else [compiler]
    compile_only = args.compile_only or "-elf-" in name or "-none-" in name
    print(f"Meshtastic policy compiler: {compiler}", flush=True)
    with tempfile.TemporaryDirectory(prefix="crowpanel-policies-") as directory:
        for filename in TESTS:
            output = Path(directory) / (Path(filename).stem + (".exe" if os.name == "nt" else ""))
            flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Isrc",
                     "-Ivariants/esp32p4/crowpanel_dhe04005d"]
            flags += ["-fsyntax-only"] if compile_only else ["-o", str(output)]
            print(f"Checking {filename}", flush=True)
            result = subprocess.run(command + flags + [str(Path(__file__).parent / filename)],
                                    cwd=ROOT, check=False)
            if result.returncode:
                return result.returncode
            if not compile_only:
                result = subprocess.run([str(output)], cwd=ROOT, check=False)
                if result.returncode:
                    return result.returncode
    mode = "compiled only (not executed)" if compile_only else "compiled and executed"
    print(f"All {len(TESTS)} Meshtastic policy tests {mode}. No device was accessed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
