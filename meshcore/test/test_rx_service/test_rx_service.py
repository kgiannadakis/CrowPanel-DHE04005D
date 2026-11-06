"""Source invariants for receive scheduling; not a substitute for device tests."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "examples/crowpanel_p4_lvgl_chat"


class ReceiveServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = (APP / "main.cpp").read_text(encoding="utf-8")
        cls.translate = (APP / "translate.cpp").read_text(encoding="utf-8")
        cls.target = (ROOT / "variants/crowpanel_dhe04005d/target.cpp").read_text(encoding="utf-8")
        cls.hal = (ROOT / "variants/crowpanel_dhe04005d/EspHal.h").read_text(encoding="utf-8")
        cls.settings = (APP / "settings_cb.cpp").read_text(encoding="utf-8")

    def test_spi_lock_spans_cs_and_every_fifo_chunk(self):
        begin = self.hal.split("void spiBeginTransaction() override", 1)[1].split(
            "void spiTransfer(", 1)[0]
        transfer = self.hal.split("void spiTransfer(", 1)[1].split(
            "void spiEndTransaction() override", 1)[0]
        end = self.hal.split("void spiEndTransaction() override", 1)[1].split(
            "void spiEnd() override", 1)[0]
        self.assertIn("xSemaphoreTake(_spiMutex, portMAX_DELAY)", begin)
        self.assertIn("xSemaphoreGive(_spiMutex)", end)
        self.assertIn("spi_device_transmit(_spiHandle, &t)", transfer)
        self.assertIn("off += CHUNK", transfer)
        for forbidden in ("xSemaphoreGive", "portENTER_CRITICAL", "vTaskSuspendAll"):
            self.assertNotIn(forbidden, transfer)
        init = self.hal.split("void spiBegin() override", 1)[1].split(
            "void spiBeginTransaction() override", 1)[0]
        self.assertLess(init.index("xSemaphoreCreateMutexStatic"), init.index("spi_bus_add_device"))

    def test_floor_noise_yields_to_rx_and_cleans_up_after_successful_samples(self):
        read = self.main.split("bool mesh_try_get_current_rssi(float& rssi)", 1)[1].split(
            "void mesh_set_tx_power_pref", 1)[0]
        self.assertLess(read.index("mesh_try_lock(0)"), read.index("radio_driver.getCurrentRSSI()"))
        self.assertIn("mesh_unlock();", read)
        tick = self.settings.split("static void floor_noise_tick(", 1)[1].split(
            "// ---- Orientation toggle", 1)[0]
        self.assertLess(tick.index("if (!mesh_try_get_current_rssi(r)) return;"), tick.index("j->idx++"))
        self.assertIn("delete j;", tick)
        self.assertIn("lv_timer_del(t);", tick)
        self.assertNotIn("lv_timer_set_repeat_count", tick)
        self.assertNotIn("radio_driver.", tick)

    def test_ui_power_change_serializes_the_complete_radio_operation(self):
        callback = self.settings.split("void cb_txpower_ready(", 1)[1].split(
            "// Persist to NVS", 1)[0]
        self.assertLess(callback.index("xSemaphoreTake(g_mesh_mutex"), callback.index("radio_set_tx_power(dbm)"))
        self.assertGreater(callback.index("xSemaphoreGive(g_mesh_mutex)"), callback.index("mesh_set_tx_power_pref(dbm)"))

    def test_radio_runs_before_ui_and_before_sleep(self):
        loop = self.main.split("void loop() {", 1)[1].split("// mesh_api wrappers", 1)[0]
        self.assertLess(loop.index("service_radio();"), loop.index("battery_poll();"))
        self.assertIn("translate_loop(service_radio)", loop)
        self.assertIn("service_radio();\n  crash_diagnostics_mark(false, CrashStage::Sleep);\n  cpu_light_sleep_poll();", loop)
        self.assertIn("if (g_deferred_msg_count == 0 &&", loop)

    def test_radio_service_is_guarded_and_no_lvgl(self):
        service = self.main.split("static void service_radio() {", 1)[1].split("// setup()", 1)[0]
        self.assertIn("!mesh_try_lock(0)", service)
        self.assertLess(service.index("mesh_try_lock(0)"), service.index("g_mesh->loop()"))
        self.assertIn("mesh_unlock();", service)
        self.assertNotIn("lvgl_lock(", service)
        self.assertNotIn("lv_label_", service)
        self.assertIn("now - last_warning_ms >= 10000UL", service)

    def test_http_worker_has_no_ui_radio_or_files(self):
        worker = self.translate.split("static void translation_network_task(void*) {", 1)[1].split(
            "static bool ensure_translation_network_task()", 1)[0]
        self.assertIn("xSemaphoreTake(s_network_start, portMAX_DELAY)", worker)
        self.assertIn("xSemaphoreGive(s_network_done)", worker)
        for forbidden in ("lvgl_", "lv_obj_", "LittleFS", "g_mesh", "radio_driver", "esp_task_wdt_reset"):
            self.assertNotIn(forbidden, worker)

    def test_wait_services_radio_and_keeps_single_request_lifetime(self):
        wait = self.translate.split("static bool translate_with_radio_service(", 1)[1].split(
            "#endif // ESP32", 1)[0]
        self.assertIn("xSemaphoreTake(s_network_done, pdMS_TO_TICKS(5))", wait)
        self.assertIn("if (service_radio) service_radio();", wait)
        self.assertIn("esp_task_wdt_reset();", wait)
        self.assertNotIn("break;", wait)
        self.assertNotIn("vTaskDelete", wait)
        self.assertNotIn("tgbridge_loop", wait)

    def test_lazy_stack_allocation_precedes_heap_guard_and_dequeue(self):
        loop = self.translate.split("void translate_loop(void (*service_radio)()) {", 1)[1]
        self.assertLess(loop.index("if (queue_empty) return;"), loop.index("ensure_translation_network_task()"))
        self.assertLess(loop.index("ensure_translation_network_task()"), loop.index("heap_caps_get_free_size(MALLOC_CAP_DMA)"))
        self.assertLess(loop.index("heap_caps_get_free_size(MALLOC_CAP_DMA)"), loop.index("TranslateRequest req;"))
        self.assertIn("network_retry_ms = now_ms + 15000UL", loop)

    def test_gain_readback_is_safe_and_repair_is_verified(self):
        check = self.target.split("RxGainCheck radio_verify_rx_gain(int16_t& error) {", 1)[1].split(
            "uint32_t radio_get_rng_seed()", 1)[0]
        self.assertLess(check.index("isReceiveIdleForCpuSleep()"), check.index("readRxGain(gain)"))
        self.assertIn("loraRadio.isReceiving()", check)
        self.assertIn("if (error != RADIOLIB_ERR_NONE) return RxGainCheck::Failed", check)
        self.assertIn("gain != RADIOLIB_SX126X_RX_GAIN_POWER_SAVING", check)
        write = check.index("setRxBoostedGainMode(SX126X_RX_BOOSTED_GAIN, true)")
        self.assertGreater(check.rindex("readRxGain(gain)"), write)
        self.assertNotIn("standby(", check)
        self.assertNotIn("reset(", check)

    def test_sleep_irq_recovery_and_error_initialization_remain(self):
        self.assertIn("radio_driver.latchReceiveInterruptAfterCpuSleep()", self.main)
        wrapper = (ROOT / "src/helpers/radiolib/RadioLibWrappers.h").read_text(encoding="utf-8")
        self.assertIn("n_recv = n_sent = n_recv_errors = 0", wrapper)

    def test_both_receive_start_paths_record_failures(self):
        wrapper = (ROOT / "src/helpers/radiolib/RadioLibWrappers.cpp").read_text(encoding="utf-8")
        self.assertEqual(wrapper.count("n_rx_start_errors++;"), 2)
        self.assertEqual(wrapper.count("last_rx_start_error = err;"), 2)
        self.assertIn("radio_driver.getRxStartErrors()", self.main)


if __name__ == "__main__":
    unittest.main()
