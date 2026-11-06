"""Auto Contacts is a passive receive setting, not an advert command."""

from pathlib import Path
import unittest


APP = Path(__file__).resolve().parents[2] / "examples/crowpanel_p4_lvgl_chat"


class ContactDiscoveryToggleTests(unittest.TestCase):
    def test_toggle_only_changes_receive_policy_ui_and_saved_preference(self):
        source = (APP / "settings_cb.cpp").read_text(encoding="utf-8")
        callback = source.split("void cb_auto_contact_toggle(", 1)[1].split(
            "void ui_apply_auto_repeater_state(", 1)[0]
        self.assertIn("g_auto_contact_enabled = !g_auto_contact_enabled;", callback)
        self.assertIn("ui_apply_auto_contact_state();", callback)
        self.assertIn("save_ui_prefs_nvs();", callback)
        for forbidden in ("mesh_send_", "g_deferred_flood_advert", "g_deferred_zero_advert"):
            self.assertNotIn(forbidden, callback)

    def test_dedicated_advert_buttons_still_queue_their_commands(self):
        source = (APP / "settings_cb.cpp").read_text(encoding="utf-8")
        flood = source.split("void cb_fadvert(", 1)[1].split("}", 1)[0]
        zero = source.split("void cb_zeroadvert(", 1)[1].split("}", 1)[0]
        self.assertIn("g_deferred_flood_advert = true;", flood)
        self.assertIn("g_deferred_zero_advert = true;", zero)


if __name__ == "__main__":
    unittest.main()
