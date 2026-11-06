#!/usr/bin/env python3
"""Build and install the complete CrowPanel dual-boot image set.

For routine firmware updates, use the affected project's normal upload target.
--dry-run checks already-built inputs without building or accessing a device.
"""

import argparse
import csv
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
BUILDS = (
    ("selector", "dhe04005d-5inch-P4-selector"),
    ("meshcore", "dhe04005d-5inch-P4-meshcore"),
    ("meshtastic", "dhe04005d-5inch-P4-meshtastic"),
)
PARTITION_TABLES = (
    "selector/partitions_dualboot.csv",
    "meshcore/partitions_p4.csv",
    "meshtastic/variants/esp32p4/crowpanel_dhe04005d/partitions.csv",
)
FLASH_SIZE = 16 * 1024 * 1024


def resolve_pio():
    """Prefer the normal user installation, then a CLI on PATH."""
    penv = Path.home() / ".platformio" / "penv"
    for relative in ("Scripts/platformio.exe", "Scripts/pio.exe", "bin/platformio", "bin/pio"):
        candidate = penv / relative
        if candidate.is_file():
            return str(candidate)
    for command in ("platformio", "pio"):
        candidate = shutil.which(command)
        if candidate:
            return candidate
    raise FileNotFoundError("PlatformIO not found. Install its CLI or VS Code extension.")


def parse_partitions_csv(path):
    parts = {}
    with Path(path).open(encoding="utf-8", newline="") as source:
        rows = csv.reader(line for line in source if not line.lstrip().startswith("#"))
        for row in rows:
            if not row or not any(field.strip() for field in row):
                continue
            if len(row) < 5 or not row[0].strip():
                raise ValueError(f"Incomplete partition row in {path}")
            name = row[0].strip()
            if name in parts:
                raise ValueError(f"Duplicate partition {name} in {path}")
            parts[name] = {
                "type": row[1].strip(),
                "subtype": row[2].strip(),
                "offset": int(row[3].strip(), 0),
                "size": int(row[4].strip(), 0),
            }
    previous_end = 0x9000
    for name, part in sorted(parts.items(), key=lambda item: item[1]["offset"]):
        if part["size"] <= 0 or part["offset"] < previous_end:
            raise ValueError(f"Invalid or overlapping partition: {name}")
        previous_end = part["offset"] + part["size"]
        if previous_end > FLASH_SIZE:
            raise ValueError(f"Partition outside 16 MB flash: {name}")
    return parts


def binary_layout(path):
    data = Path(path).read_bytes()
    if not data or len(data) % 32:
        raise ValueError(f"Invalid partition table size: {path}")
    parts = {}
    for offset in range(0, len(data), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBII16sI", data, offset)
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:  # Optional SDK partition-table MD5 record.
            break
        if magic != 0x50AA:
            raise ValueError(f"Invalid partition entry in {path}")
        name = label.split(b"\0", 1)[0].decode("ascii")
        if name in parts:
            raise ValueError(f"Duplicate binary partition {name} in {path}")
        parts[name] = (address, size)
    if not parts:
        raise ValueError(f"No partitions in {path}")
    return parts


def build_directory(repo, project, environment):
    return repo / project / ".pio" / "build" / environment


def find_firmware(directory):
    exact = directory / "firmware.bin"
    if exact.is_file():
        return exact
    candidates = sorted(path for path in directory.glob("firmware*.bin")
                        if "merged" not in path.name.lower() and "factory" not in path.name.lower())
    if len(candidates) != 1:
        raise ValueError(f"Expected one application image in {directory}; rebuild or remove stale outputs.")
    return candidates[0]


def prepare_flash_plan(repo=ROOT):
    repo = Path(repo)
    tables = [parse_partitions_csv(repo / relative) for relative in PARTITION_TABLES]
    if not all(table == tables[0] for table in tables[1:]):
        raise ValueError("The selector, MeshCore and Meshtastic partition CSVs disagree.")
    parts = tables[0]
    for name in ("factory", "ota_0", "ota_1"):
        if name not in parts:
            raise ValueError(f"Required application partition is missing: {name}")
    expected = {name: (part["offset"], part["size"]) for name, part in parts.items()}
    directories = {project: build_directory(repo, project, env) for project, env in BUILDS}
    generated_table = directories["selector"] / "partitions.bin"
    for path in (repo / "partitions.bin", generated_table):
        if binary_layout(path) != expected:
            raise ValueError(f"Partition table disagrees with the CSV layout: {path}")

    # Never rewrite the distributed table as a side effect of inspection/flashing.
    images = [
        (0x2000, directories["meshtastic"] / "bootloader.bin", 0x6000),
        (0x8000, generated_table, 0x1000),
    ]
    for project, partition in (("selector", "factory"), ("meshcore", "ota_0"), ("meshtastic", "ota_1")):
        images.append((parts[partition]["offset"], find_firmware(directories[project]), parts[partition]["size"]))
    previous_end = 0
    for address, path, capacity in sorted(images):
        size = path.stat().st_size
        if not size or size > capacity or address < previous_end or address + size > FLASH_SIZE:
            raise ValueError(f"Empty, oversized or overlapping image: {path}")
        if address != 0x8000:
            with path.open("rb") as source:
                if source.read(1) != b"\xe9":
                    raise ValueError(f"Not an ESP application/bootloader image: {path}")
        previous_end = address + size
    return [(address, path) for address, path, capacity in images]


def build_all(repo=ROOT):
    pio = resolve_pio()
    for project, environment in BUILDS:
        print(f"Building {project}: {environment}", flush=True)
        subprocess.run([pio, "run", "-e", environment], cwd=Path(repo) / project, check=True)


def flash_command(port, images, baud=921600):
    command = [sys.executable, "-m", "esptool", "--chip", "esp32p4",
               "--port", port, "--baud", str(baud), "write-flash"]
    for address, path in images:
        command.extend((f"0x{address:x}", str(path)))
    return command


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", help="Serial port; required unless --dry-run")
    parser.add_argument("--skip-build", action="store_true", help="Use already-built images")
    parser.add_argument("--dry-run", action="store_true", help="Inspect existing inputs only; no builds or device access")
    args = parser.parse_args(argv)
    if not args.port and not args.dry_run:
        parser.error("A serial port is required unless --dry-run is selected.")
    try:
        if not args.skip_build and not args.dry_run:
            build_all()
        images = prepare_flash_plan()
        print("Full-device installation plan (not a routine firmware update):")
        for address, path in images:
            print(f"  0x{address:06x}  {path.stat().st_size:>9} bytes  {path.relative_to(ROOT)}")
        if args.dry_run:
            print("Dry run complete. No build, device connection or file writes performed.")
            return 0
        print("Back up device data first. Continuing with the standard esptool RAM stub.", flush=True)
        subprocess.run(flash_command(args.port, images), check=True)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Installation complete. Power-cycle to enter the selector.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
