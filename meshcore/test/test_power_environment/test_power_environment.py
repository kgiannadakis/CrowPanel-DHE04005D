import configparser
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
NORMAL = "dhe04005d-5inch-P4-meshcore"


class PromotedPowerEnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.config = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
        self.config.read(ROOT / "platformio.ini", encoding="utf-8")

    def test_single_default_environment(self):
        self.assertEqual([s for s in self.config.sections() if s.startswith("env:")], ["env:" + NORMAL])
        self.assertEqual(self.config["platformio"]["default_envs"], NORMAL)

    def test_confirmed_power_flags(self):
        flags = self.config["env:" + NORMAL]["build_flags"].splitlines()
        expected = {
            "CROWPANEL_CPU_LIGHT_SLEEP": 1, "CROWPANEL_CPU_SLEEP_MS": 80,
            "CROWPANEL_DISPLAY_TASK_BACKLIGHT": 1, "CROWPANEL_TOUCH_IRQ_SLEEP": 1,
            "CROWPANEL_TOUCH_IRQ_SLEEP_MS": 250, "CROWPANEL_AWAKE_IDLE_POWER": 1,
            "CROWPANEL_DYNAMIC_SCREEN_POWER": 0, "CROWPANEL_SUSPEND_RGB_WHEN_DARK": 0,
        }
        for name, value in expected.items():
            self.assertEqual(sum(f.strip() == f"-D {name}={value}" for f in flags), 1, name)

    def test_network_libraries_remain_pinned(self):
        deps = self.config["env:" + NORMAL]["lib_deps"]
        self.assertIn("AsyncTCP.git#fd296a703104463c841d8711de3d5f63ea8ffb14", deps)
        self.assertIn("ESPAsyncWebServer.git#ded3e58b5cee1a4bff050b587d96a54261f792b1", deps)

    def test_upload_task_uses_normal_environment(self):
        task_file = ROOT / ".vscode/tasks.json"
        if not task_file.is_file():
            self.skipTest("Local VS Code tasks are optional and are not distributed")
        tasks = json.loads(task_file.read_text(encoding="utf-8"))["tasks"]
        self.assertEqual(len(tasks), 1)
        args = tasks[0]["args"]
        self.assertEqual(args[:5], ["run", "-e", NORMAL, "-t", "upload"])


if __name__ == "__main__":
    unittest.main()
