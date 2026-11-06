"""Publishing guards use fixture files, not real device data."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("repository_checks", ROOT / "tools/check_repository.py")
CHECKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKS)


class RepositoryTests(unittest.TestCase):
    def test_private_and_generated_paths_are_rejected(self):
        for path in ("meshcore/.pio/build/fw.bin", "meshcore/logs/device.txt", "logs/nvs.bin",
                     "meshcore/tmp/extract.txt", ".env", ".env.local", "meshcore/secrets.ini",
                     "meshcore/.local-tools/settings.json", "old.cpp.bak", "firmware.elf"):
            self.assertTrue(CHECKS.is_local_artifact(path), path)

    def test_editor_compiler_and_scratch_files_are_rejected(self):
        for path in ("selector/.vscode/tasks.json", "meshcore/.venv/pyvenv.cfg",
                     "meshtastic/node_modules/tool/index.js", "firmware.exe",
                     "obj/radio.o", "chat.cpp.swp", "prefs.tmp"):
            self.assertTrue(CHECKS.is_local_artifact(path), path)

    def test_meshtastic_owned_code_is_checked(self):
        for path in ("meshtastic/src/graphics/mcui/McUI.cpp",
                     "meshtastic/src/mqtt/MQTT.cpp",
                     "meshtastic/test/host/check_policies.py",
                     "meshtastic/variants/esp32p4/crowpanel_dhe04005d/CrowPanelStorage.cpp"):
            self.assertTrue(CHECKS.is_owned_text(path), path)

    def test_meshtastic_documentation_is_checked(self):
        names = {path.relative_to(ROOT).as_posix() for path in CHECKS.documentation_files()}
        self.assertTrue({"meshtastic/README.md", "meshtastic/DEVELOPMENT.md",
                         "meshtastic/test/host/README.md"}.issubset(names))

    def test_release_assets_and_example_config_remain_publishable(self):
        for path in ("partitions.bin", "docs/emoji_atlas_32.bin", "docs/images/Meshcore_maps.png",
                     "meshcore/platformio.ini", "meshtastic/.env.example"):
            self.assertFalse(CHECKS.is_local_artifact(path), path)

    def test_private_key_detection_is_not_a_raw_phrase_match(self):
        phrase = "BEGIN PRIVATE KEY"
        self.assertFalse(any(pattern.search(phrase) for pattern in CHECKS.SECRET_PATTERNS))
        self.assertTrue(any(pattern.search("-----" + phrase + "-----") for pattern in CHECKS.SECRET_PATTERNS))

    def test_document_links_are_case_sensitive_even_on_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Guide.md").write_text("# Guide\n", encoding="utf-8")
            readme = root / "README.md"
            readme.write_text('[Good](Guide.md#section) [Bad](guide.md) <img src="missing.png">', encoding="utf-8")
            self.assertEqual(len(CHECKS.local_link_errors(readme, root)), 2)

    def test_external_links_and_fenced_examples_are_not_local_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            readme = root / "README.md"
            readme.write_text('[Web](https://example.com) [Here](#section)\n```md\n[Example](absent.md)\n```', encoding="utf-8")
            self.assertEqual(CHECKS.local_link_errors(readme, root), [])

    def test_link_cannot_escape_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            readme = root / "README.md"
            readme.write_text('[Outside](../outside.md)', encoding="utf-8")
            self.assertEqual(len(CHECKS.local_link_errors(readme, root)), 1)

    def test_ci_is_read_only_and_actions_are_pinned(self):
        workflow = (ROOT / ".github/workflows/checks.yml").read_text(encoding="utf-8")
        self.assertIn("contents: read", workflow)
        self.assertIn("persist-credentials: false", workflow)
        self.assertIn("python tools/check_repository.py --compiler g++", workflow)
        for line in workflow.splitlines():
            if "uses:" in line:
                self.assertRegex(line, r"uses: actions/[a-z-]+@[0-9a-f]{40}\b")
        self.assertNotIn("pull_request_target", workflow)
        self.assertNotIn("-t upload", workflow)


if __name__ == "__main__":
    unittest.main()
