"""Guard against obsolete files and misleading build documentation returning."""

import csv
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "examples/crowpanel_p4_lvgl_chat"


class RepositoryHygieneTests(unittest.TestCase):
    def test_retired_files_are_not_in_the_active_app(self):
        for name in ("main_phase1.cpp.bak", "lv_font_emoji_16.c.disabled",
                     "lv_font_emoji_18.c.disabled", "web_dashboard_https.cpp",
                     "dashboard_cert.h", "_gen_cert_h.py", "wifi_features_stub.cpp"):
            self.assertFalse((APP / name).exists(), name)
        self.assertTrue((APP / "unsupported_features.cpp").is_file())

    def test_no_disabled_duplicate_implementations_or_shared_tls_key(self):
        for path in APP.iterdir():
            if path.suffix not in (".cpp", ".h", ".c"):
                continue
            source = path.read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"^\s*#if\s+0\b", source, re.MULTILINE), path.name)
            self.assertNotIn("BEGIN PRIVATE KEY", source, path.name)

    def test_documented_source_and_check_entry_points_exist(self):
        for path in ("docs/crowpanel.md", "tools/check_crowpanel.py",
                     "variants/crowpanel_dhe04005d/README.md"):
            self.assertTrue((ROOT / path).is_file(), path)

    def test_local_device_and_scratch_data_are_ignored(self):
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
        for pattern in ("/tmp/", "/sdcard/", "/logs/", "/tools/tiles/"):
            self.assertIn(pattern, ignore)

    def test_partition_layouts_and_distributed_binary_agree(self):
        repo = ROOT.parent
        tables = (
            ROOT / "partitions_p4.csv",
            repo / "selector/partitions_dualboot.csv",
            repo / "meshtastic/variants/esp32p4/crowpanel_dhe04005d/partitions.csv",
        )
        layouts = []
        for path in tables:
            with path.open(encoding="utf-8", newline="") as source:
                rows = csv.reader(line for line in source if not line.lstrip().startswith("#"))
                layouts.append({row[0].strip(): (int(row[3].strip(), 0), int(row[4].strip(), 0))
                                for row in rows if len(row) >= 5 and row[0].strip()})
        self.assertEqual(layouts[0], layouts[1])
        self.assertEqual(layouts[0], layouts[2])
        data = (repo / "partitions.bin").read_bytes()
        entries = (struct.unpack_from("<HBBII16sI", data, offset)
                   for offset in range(0, len(data) - 31, 32))
        binary = {entry[5].split(b"\0")[0].decode("ascii"): (entry[3], entry[4])
                  for entry in entries if entry[0] == 0x50AA}
        self.assertEqual(layouts[0], binary)
        guide = (repo / "docs/installation.md").read_text(encoding="utf-8")
        self.assertIn(f"`0x{layouts[0]['ota_0'][0]:x}` | `meshcore.bin`", guide)

    def test_release_atlases_have_one_canonical_location(self):
        for name in ("emoji_atlas_20.bin", "emoji_atlas_32.bin"):
            self.assertTrue((ROOT.parent / "docs" / name).is_file(), name)
            # A developer may regenerate local copies; ignore rules exclude
            # them, while docs/ remains the distributed source of truth.
            self.assertIn("/tools/emoji_atlas_*.bin", (ROOT / ".gitignore").read_text())


if __name__ == "__main__":
    unittest.main()
