"""Regression guards: neighbour inspection must not populate the contact list."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "examples/crowpanel_p4_lvgl_chat"


class NeighbourViewOnlyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = (APP / "main.cpp").read_text(encoding="utf-8")
        cls.ui = (APP / "repeater_ui.cpp").read_text(encoding="utf-8")
        cls.map = (APP / "map_view.cpp").read_text(encoding="utf-8")

    def test_import_and_dropdown_priority_are_removed(self):
        for retired in ("NeighbourImport", "receiveNeighbourImport", "beginNeighbourImport",
                        "mesh_repeater_import_neighbours", "mesh_get_neighbour_repeaters"):
            self.assertNotIn(retired, self.main)
            self.assertNotIn(retired, self.ui)
        self.assertIn("mesh_repeater_show_neighbours(s_selected_repeater_pubkey)", self.ui)

    def test_request_is_read_only_for_contacts(self):
        request = self.main.split("static int request_repeater_neighbours(", 1)[1].split(
            "int mesh_repeater_show_neighbours(", 1)[0]
        for mutation in ("addContact(", "removeContact(", "saveContacts(",
                         "g_contacts_save_dirty", "contact->flags", "contact->name ="):
            self.assertNotIn(mutation, request)
        self.assertIn("{0x06, 0, 20, 0, 0, 0, 4, 0, 0, 0, 0}", request)
        self.assertIn("if (show_map) map_show_neighbours(tag, source, name, timeout)", request)
        self.assertLess(request.rindex("mesh_unlock();"), request.index("map_show_neighbours("))

    def test_web_and_ui_paths_remain_separate(self):
        self.assertIn("return request_repeater_neighbours(pub_key, true);", self.main)
        self.assertIn("return request_repeater_neighbours(pub_key, false);", self.main)

    def test_cleanup_runs_before_ui_and_after_closing_contact_file(self):
        load = self.main.split("void loadContacts() {", 1)[1].split("void saveContacts()", 1)[0]
        self.assertIn("NeighbourContactCleanup::shouldRemove", load)
        self.assertIn("c.out_path_len != OUT_PATH_UNKNOWN", load)
        self.assertIn("hop != 0", load)
        self.assertLess(load.index("file.close();"), load.index("saveContacts();"))
        self.assertNotIn("purge_contacts_file_all", load)

    def test_links_are_black_and_use_logged_in_repeater_origin(self):
        draw = self.map.split("static void markers_draw_cb(", 1)[1].split(
            "// Draw labels before dots", 1)[0]
        self.assertIn("line.color = lv_color_black();", draw)
        self.assertIn("line.opa = LV_OPA_COVER;", draw)
        self.assertIn("s_neighbour_origin.screen_x", draw)
        self.assertIn("s_neighbour_points[i].screen_x", draw)
        self.assertIn("NeighbourMap::clip(", draw)
        self.assertNotIn("g_fixed_position", draw)

    def test_diagnostics_snapshot_does_not_log_or_allocate_with_radio_locked(self):
        snapshot = self.map.split("static void snapshot_detected_repeaters(", 1)[1].split(
            "static void neighbour_diagnostic_timer_cb(", 1)[0]
        self.assertNotIn("Serial.", snapshot)
        self.assertNotIn("heap_caps_", snapshot)
        self.assertIn("MAX_CONTACTS * 2", snapshot)
        self.assertIn("matches_neighbour", snapshot)

    def test_diagnostic_dump_is_batched_and_cannot_mutate_contacts(self):
        timer = self.map.split("static void neighbour_diagnostic_timer_cb(", 1)[1].split(
            "int s_pending_dx", 1)[0]
        self.assertIn("batch < 2", timer)
        self.assertIn("saved_matches=%u advert_matches=%u", timer)
        self.assertIn("in_neighbours=%s", timer)
        self.assertIn("marker draw callback NOT SEEN", timer)
        for forbidden in ("xSemaphoreTake", "portENTER_CRITICAL", "addContact(", "saveContacts("):
            self.assertNotIn(forbidden, timer)

    def test_wire_evidence_is_bounded_and_printed_outside_radio_lock(self):
        timer = self.map.split("static void neighbour_diagnostic_timer_cb(", 1)[1].split(
            "int s_pending_dx", 1)[0]
        snapshot = self.map.split("static void snapshot_detected_repeaters(", 1)[1].split(
            "static void neighbour_diagnostic_timer_cb(", 1)[0]
        self.assertIn("uint8_t raw[188]", self.map)
        self.assertIn("LV_MIN(reply.len, sizeof(s_diagnostics->raw))", snapshot)
        self.assertIn("memcpy(s_diagnostics->raw, reply.bytes", snapshot)
        self.assertIn("LV_MIN((size_t)16, dump.raw_len - dump.raw_cursor)", timer)
        self.assertIn("raw offset=%u bytes=%s", timer)
        self.assertIn("heard_ago=%lus SNR=%+.2f", timer)
        self.assertIn("hex[2 * count] = 0", timer)
        self.assertIn("compare keys, not names", self.main)
        self.assertIn("screen_matches=%u screen_name=", timer)
        self.assertIn("memcmp(g_repeater_list[j].pub_key, row.key, sizeof(row.key))", self.map)

    def test_origin_and_reply_failures_are_reported(self):
        for diagnostic in ("reply %s: source=", "IGNORED-tag", "origin query timed out",
                           "origin reply ignored", "origin reply %s:", "origin READY",
                           "neighbour reply TIMEOUT", "draw: origin=%s"):
            self.assertIn(diagnostic, self.map)
        draw = self.map.split("static void markers_draw_cb(", 1)[1].split(
            "// Draw labels before dots", 1)[0]
        self.assertNotIn("Serial.", draw)


if __name__ == "__main__":
    unittest.main()
