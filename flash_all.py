#!/usr/bin/env python3
"""
Build all three firmwares and flash the dual-boot system to the CrowPanel.

Uses the repo-root partitions.bin and the Meshtastic bootloader.

Requires: Python 3 + PlatformIO CLI (pio) + esptool
          (all included with a standard PlatformIO installation)

Usage:
    python flash_all.py <PORT>               (build + flash)
    python flash_all.py <PORT> --skip-build  (flash only, no rebuild)

Examples:
    python flash_all.py COM20               (Windows)
    python flash_all.py /dev/ttyUSB0        (Linux)
    python flash_all.py /dev/cu.usbserial   (macOS)
"""

import subprocess, sys, os, glob, shutil, csv

# Prefer the standalone PlatformIO install (Python 3.11) over a pio that may
# be installed under a newer Python. The pioarduino platform-espressif32
# bundles a `fatfs` C extension built for cp311 only, so running pio under
# Python 3.12+ fails with: ModuleNotFoundError: No module named 'fatfs.wrapper'.
def _resolve_pio():
    for cand in (r"C:\pio\penv\Scripts\pio.exe",
                 os.path.expanduser(r"~\.platformio\penv\Scripts\pio.exe")):
        if os.path.isfile(cand):
            return cand
    return shutil.which("pio") or "pio"

PIO = _resolve_pio()

if len(sys.argv) < 2 or sys.argv[1].startswith("--"):
    print("Usage: python flash_all.py <PORT> [--skip-build]")
    print("  e.g. python flash_all.py COM20")
    print("  e.g. python flash_all.py /dev/ttyUSB0")
    sys.exit(1)

PORT = sys.argv[1]
SKIP_BUILD = "--skip-build" in sys.argv
BAUD = "921600"

REPO_DIR = os.path.dirname(os.path.abspath(__file__))

BUILDS = [
    ("selector",   "dhe04005d-5inch-P4-selector"),
    ("meshcore",   "dhe04005d-5inch-P4-meshcore"),
    ("meshtastic", "dhe04005d-5inch-P4-meshtastic"),
]
ENVS = {name: env for name, env in BUILDS}


def parse_num(s):
    return int(s.strip(), 0)


def parse_partitions_csv(path):
    parts = {}
    with open(path, newline="") as fp:
        for row in csv.reader(line for line in fp if not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            name = row[0].strip()
            if not name:
                continue
            parts[name] = {
                "type": row[1].strip(),
                "subtype": row[2].strip(),
                "offset": parse_num(row[3]),
                "size": parse_num(row[4]),
            }
    return parts


def require_fits(label, filename, part):
    size = os.path.getsize(filename)
    if size > part["size"]:
        print(
            f"ERROR: {label} image is too large for partition "
            f"({size:#x} bytes > {part['size']:#x})."
        )
        print(f"       file: {filename}")
        sys.exit(1)


def find_firmware(proj, env):
    build_dir = os.path.join(REPO_DIR, proj, ".pio", "build", env)
    if not os.path.isdir(build_dir):
        return None
    for f in sorted(os.listdir(build_dir)):
        if f.startswith("firmware") and f.endswith(".bin") and "merged" not in f:
            return os.path.join(build_dir, f)
    return None


def build_all():
    for proj, env in BUILDS:
        proj_dir = os.path.join(REPO_DIR, proj)
        print(f"\n{'='*60}")
        print(f"  Building {proj} (env: {env})")
        print(f"{'='*60}")
        subprocess.run([PIO, "run", "-e", env], cwd=proj_dir, check=True)


def flash():
    partitions_csv = os.path.join(REPO_DIR, "selector", "partitions_dualboot.csv")
    parts = parse_partitions_csv(partitions_csv)
    for required in ("factory", "ota_0", "ota_1"):
        if required not in parts:
            print(f"ERROR: {required} partition missing from {partitions_csv}")
            sys.exit(1)

    # partitions.bin: source of truth is selector/partitions_dualboot.csv,
    # which PlatformIO compiles into selector/.pio/build/<env>/partitions.bin
    # on every selector build. Copy it to the repo root so this script and
    # the selector build can never disagree about slot offsets — drift here
    # silently corrupts the other firmwares (they write to data partitions
    # at addresses that overlap factory/ota slots).
    selector_partitions = os.path.join(
        REPO_DIR, "selector", ".pio", "build", ENVS["selector"], "partitions.bin")
    partitions = os.path.join(REPO_DIR, "partitions.bin")
    if not os.path.isfile(selector_partitions):
        print(f"ERROR: {selector_partitions} not found. Build selector first.")
        sys.exit(1)
    if os.path.getmtime(selector_partitions) < os.path.getmtime(partitions_csv):
        print(f"ERROR: {os.path.relpath(selector_partitions, REPO_DIR)} is older than")
        print(f"       {os.path.relpath(partitions_csv, REPO_DIR)}.")
        print("       Re-run without --skip-build so PlatformIO regenerates partitions.bin.")
        sys.exit(1)
    shutil.copyfile(selector_partitions, partitions)
    print(f"  partitions.bin <- {os.path.relpath(selector_partitions, REPO_DIR)}")

    # Bootloader from Meshtastic build
    mt_build = os.path.join(REPO_DIR, "meshtastic", ".pio", "build", ENVS["meshtastic"])
    bootloader = os.path.join(mt_build, "bootloader.bin")
    if not os.path.isfile(bootloader):
        print("ERROR: bootloader.bin not found in meshtastic build. Build meshtastic first.")
        sys.exit(1)

    # Firmware binaries
    selector_fw   = find_firmware("selector",   ENVS["selector"])
    meshcore_fw   = find_firmware("meshcore",   ENVS["meshcore"])
    meshtastic_fw = find_firmware("meshtastic", ENVS["meshtastic"])

    for name, fw in [("selector", selector_fw), ("meshcore", meshcore_fw), ("meshtastic", meshtastic_fw)]:
        if not fw:
            print(f"ERROR: No firmware found for {name}. Build first.")
            sys.exit(1)

    require_fits("selector", selector_fw, parts["factory"])
    require_fits("meshcore", meshcore_fw, parts["ota_0"])
    require_fits("meshtastic", meshtastic_fw, parts["ota_1"])

    factory_off = f"0x{parts['factory']['offset']:x}"
    ota0_off = f"0x{parts['ota_0']['offset']:x}"
    ota1_off = f"0x{parts['ota_1']['offset']:x}"

    print(f"\n{'='*60}")
    print(f"  Flashing to {PORT} at {BAUD} baud")
    print(f"{'='*60}")
    print(f"  0x2000   -> bootloader.bin (from meshtastic)")
    print(f"  0x8000   -> partitions.bin (from repo root)")
    print(f"  {factory_off:<8} -> {os.path.basename(selector_fw)}    (factory)")
    print(f"  {ota0_off:<8} -> {os.path.basename(meshcore_fw)}     (ota_0)")
    print(f"  {ota1_off:<8} -> {os.path.basename(meshtastic_fw)}   (ota_1)")

    subprocess.run([
        sys.executable, "-m", "esptool",
        "--chip", "esp32p4", "--port", PORT, "--baud", BAUD,
        "write-flash",
        "0x2000",   bootloader,
        "0x8000",   partitions,
        factory_off, selector_fw,
        ota0_off,    meshcore_fw,
        ota1_off,    meshtastic_fw,
    ], check=True)

    print("\nDone! Power-cycle to enter the boot selector.")


if __name__ == "__main__":
    if not SKIP_BUILD:
        build_all()
    flash()
