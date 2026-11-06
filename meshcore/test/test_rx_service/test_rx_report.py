"""Quiet RX observation must not change RF state or hold mesh while formatting."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "examples/crowpanel_p4_lvgl_chat"


class RxReportTests(unittest.TestCase):
    def test_path_trace_observes_each_stage_without_changing_receive_behavior(self):
        main = (APP / "main.cpp").read_text(encoding="utf-8")
        hooks = main.split("void logRxRaw(", 1)[1].split("bool allowPacketForward", 1)[0]
        for forbidden in ("Serial.", "malloc", "new ", "LittleFS", "sendFlood", "setPathHash", "startReceive"):
            self.assertNotIn(forbidden, hooks)
        self.assertIn("len <= path_offset", hooks)
        self.assertIn("ROUTE_TYPE_TRANSPORT_FLOOD", hooks)
        self.assertIn("ROUTE_TYPE_TRANSPORT_DIRECT", hooks)
        self.assertIn("BaseChatMesh::onGroupDataRecv(packet, type, channel, data, len);", hooks)
        self.assertLess(hooks.index("++s_rx_paths.decrypted"), hooks.index("BaseChatMesh::onGroupDataRecv"))
        summary = main.split("static void report_rx_paths_if_changed() {", 1)[1].split("String mesh_rx_report()", 1)[0]
        self.assertIn("< 60000", summary)
        self.assertIn("if (total == last_raw) return;", summary)
        self.assertLess(summary.index("mesh_unlock()"), summary.index("Serial.printf"))
        self.assertIn("report_rx_paths_if_changed();", main.split("void loop() {", 1)[1])

    def test_report_is_a_locked_snapshot_without_radio_commands(self):
        main = (APP / "main.cpp").read_text(encoding="utf-8")
        report = main.split("String mesh_rx_report()", 1)[1].split("void mesh_set_tx_power_pref", 1)[0]
        self.assertLess(report.index("mesh_try_lock"), report.index("getLastRxReadError"))
        self.assertLess(report.index("mesh_unlock()"), report.index("snprintf"))
        for forbidden in ("setRx", "startReceive", "sendFlood", "LittleFS", "Serial."):
            self.assertNotIn(forbidden, report)
        channel = main.split("void onChannelMessageRecv(", 1)[1].split("uint8_t onContactRequest", 1)[0]
        self.assertEqual(main.count("++s_rx_channel_duplicates"), 1)
        self.assertIn("++s_rx_channel_duplicates", channel)
        serial = (APP / "crash_diagnostics.cpp").read_text(encoding="utf-8")
        self.assertIn('!strcmp(command, "rx")', serial)
        self.assertIn("Serial.println(mesh_rx_report())", serial)
        self.assertLess(channel.index("if (!getChannel(idx, ch)) return;"), channel.index("++s_rx_channel_accepted[idx]"))

    def test_read_error_is_retained_and_reset_with_statistics(self):
        header = (ROOT / "src/helpers/radiolib/RadioLibWrappers.h").read_text(encoding="utf-8")
        source = (ROOT / "src/helpers/radiolib/RadioLibWrappers.cpp").read_text(encoding="utf-8")
        self.assertIn("last_rx_read_error = err;", source)
        self.assertIn("last_rx_read_error = 0;", header.split("void resetStats()", 1)[1])
