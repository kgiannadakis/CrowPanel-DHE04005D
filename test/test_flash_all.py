"""Exercise full-install planning with disposable files and mocked subprocesses."""

from contextlib import redirect_stdout, redirect_stderr
from io import StringIO
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import flash_all as FLASH


class FlashPlanTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for relative in FLASH.PARTITION_TABLES:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, path)
        shutil.copyfile(ROOT / "partitions.bin", self.root / "partitions.bin")
        self.directories = {}
        for project, environment in FLASH.BUILDS:
            directory = FLASH.build_directory(self.root, project, environment)
            directory.mkdir(parents=True)
            (directory / "firmware.bin").write_bytes(b"\xe9" + b"x" * 64)
            self.directories[project] = directory
        shutil.copyfile(ROOT / "partitions.bin", self.directories["selector"] / "partitions.bin")
        (self.directories["meshtastic"] / "bootloader.bin").write_bytes(b"\xe9boot")

    def test_plan_matches_layout_without_mutating_inputs(self):
        before = (self.root / "partitions.bin").read_bytes()
        plan = FLASH.prepare_flash_plan(self.root)
        self.assertEqual([address for address, path in plan], [0x2000, 0x8000, 0x10000, 0x210000, 0x690000])
        self.assertEqual((self.root / "partitions.bin").read_bytes(), before)

    def test_csv_disagreement_is_rejected(self):
        path = self.root / FLASH.PARTITION_TABLES[1]
        path.write_text(path.read_text().replace("0x210000", "0x220000"), encoding="utf-8")
        with self.assertRaises(ValueError):
            FLASH.prepare_flash_plan(self.root)

    def test_wrong_binary_layout_is_rejected(self):
        table = self.directories["selector"] / "partitions.bin"
        data = bytearray(table.read_bytes())
        data[4] ^= 1
        table.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "Partition table disagrees"):
            FLASH.prepare_flash_plan(self.root)

    def test_wrong_magic_and_empty_images_are_rejected(self):
        path = self.directories["meshcore"] / "firmware.bin"
        for content in (b"not firmware", b""):
            path.write_bytes(content)
            with self.assertRaises(ValueError):
                FLASH.prepare_flash_plan(self.root)

    def test_oversized_factory_image_is_rejected(self):
        path = self.directories["selector"] / "firmware.bin"
        with path.open("r+b") as output:
            output.truncate(0x200001)
        with self.assertRaisesRegex(ValueError, "oversized"):
            FLASH.prepare_flash_plan(self.root)

    def test_stale_ambiguous_application_outputs_are_rejected(self):
        directory = self.directories["meshcore"]
        (directory / "firmware.bin").unlink()
        for name in ("firmware-old.bin", "firmware-new.bin"):
            (directory / name).write_bytes(b"\xe9app")
        with self.assertRaisesRegex(ValueError, "Expected one application"):
            FLASH.prepare_flash_plan(self.root)

    def test_command_uses_stub_and_never_erase_or_data_partitions(self):
        command = FLASH.flash_command("TEST_PORT", FLASH.prepare_flash_plan(self.root))
        self.assertIn("write-flash", command)
        self.assertNotIn("--no-stub", command)
        self.assertNotIn("erase-flash", command)
        self.assertNotIn("0xb90000", command)
        self.assertNotIn("0xdc0000", command)

    def test_dry_run_never_builds_or_connects(self):
        plan = FLASH.prepare_flash_plan(self.root)
        with patch.object(FLASH, "ROOT", self.root), patch.object(FLASH, "prepare_flash_plan", return_value=plan), \
                patch.object(FLASH, "build_all") as build, patch.object(FLASH.subprocess, "run") as run, \
                redirect_stdout(StringIO()):
            self.assertEqual(FLASH.main(["--dry-run"]), 0)
        build.assert_not_called()
        run.assert_not_called()

    def test_bad_plan_cannot_reach_device_write(self):
        with patch.object(FLASH, "prepare_flash_plan", side_effect=ValueError("invalid")), \
                patch.object(FLASH.subprocess, "run") as run, redirect_stderr(StringIO()):
            self.assertEqual(FLASH.main(["TEST_PORT", "--skip-build"]), 1)
        run.assert_not_called()

    def test_missing_port_is_rejected_before_build(self):
        with patch.object(FLASH, "build_all") as build, redirect_stderr(StringIO()), self.assertRaises(SystemExit):
            FLASH.main([])
        build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
