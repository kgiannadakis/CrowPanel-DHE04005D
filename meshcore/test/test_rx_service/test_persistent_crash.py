"""Crash-report source guards; physical reset retention needs device testing."""
from pathlib import Path
import unittest

APP = Path(__file__).resolve().parents[2] / "examples/crowpanel_p4_lvgl_chat"


class PersistentCrashTests(unittest.TestCase):
    def test_factory_selector_must_fully_validate_before_boot_selection_is_erased(self):
        source = (APP.parents[1] / "src/crowpanel_dualboot.cpp").read_text(encoding="utf-8")
        self.assertIn("ESP_PARTITION_SUBTYPE_APP_FACTORY", source)
        self.assertIn("const esp_partition_pos_t position = {factory->address, factory->size};", source)
        self.assertIn("esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &image) != ESP_OK) return;", source)
        self.assertLess(source.index("if (!factory_image_fits(factory)) return;"), source.index("esp_image_verify("))
        self.assertIn("segment.data_len > factory->size - offset", source)
        self.assertIn("header.segment_count > ESP_IMAGE_MAX_SEGMENTS", source)
        self.assertLess(source.index("esp_image_verify("), source.index("esp_partition_erase_range("))
        self.assertNotIn("LittleFS", source.split('extern "C" void initVariant()', 1)[1])

    def test_notification_snapshot_is_heap_backed_and_nvs_handle_is_private(self):
        source = (APP / "persistence.cpp").read_text(encoding="utf-8")
        save = source.split("void save_notifications_nvs() {", 1)[1].split("void load_notifications_nvs()", 1)[0]
        self.assertIn("std::unique_ptr<Snapshot, decltype(&heap_caps_free)>", save)
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", save)
        self.assertLess(save.index("lvgl_lock(5)"), save.index("g_contact_unread[i].valid"))
        self.assertLess(save.index("lvgl_unlock();"), save.index('prefs.begin("notif", false)'))
        self.assertIn("Preferences prefs;", save)
        self.assertNotIn("g_prefs.", save)
        for declaration in ("ContactUnreadRecord crecs[", "ChannelUnreadRecord hrecs[", "ReadPosRecord rprecs["):
            self.assertNotIn(declaration, save)
        for context in range(101, 106):
            self.assertIn(f"CrashStage::Storage, {context}", save)
        for key in ("contacts", "channels", "readpos"):
            self.assertRegex(save, rf'if \(prefs.putBytes\("{key}", .* != bytes\) ok = false;')
        self.assertIn("if (!ok) g_notifications_dirty = true;", save)

    def test_pending_chat_text_is_flushed_before_unread_metadata(self):
        source = (APP / "main.cpp").read_text(encoding="utf-8")
        save = source.split("if (g_notifications_dirty && lvgl_lock(5)) {", 1)[1].split("// Debounced contact save", 1)[0]
        self.assertLess(save.index("chat_append_flush_sync();"), save.index("lvgl_unlock();"))
        self.assertLess(save.index("lvgl_unlock();"), save.index("save_notifications_nvs();"))
        self.assertLess(save.index("g_notifications_dirty = false;"), save.index("save_notifications_nvs();"))
        self.assertLess(save.index("if (text_saved)"), save.index("g_notifications_dirty = false;"))

    def test_chat_queue_is_locked_and_flash_writes_are_staged(self):
        source = (APP / "persistence.cpp").read_text(encoding="utf-8")
        self.assertIn("xSemaphoreCreateRecursiveMutexStatic", source)
        self.assertIn("xSemaphoreTakeRecursive(s_chat_mutex, portMAX_DELAY)", source)
        flush = source.split("static bool chat_flush_all_now() {", 1)[1].split("static bool chat_enqueue_line", 1)[0]
        self.assertLess(flush.index("PendingChatGuard guard;"), flush.index("s_pending_count == 0"))
        self.assertLess(flush.index("lvgl_lock(5)"), flush.index("PendingChatGuard guard;"))
        self.assertIn("~RenderUnlock() { lvgl_unlock(); }", flush)
        self.assertIn("uint8_t staging[128]", flush)
        self.assertIn("f.write(staging, bytes)", flush)
        self.assertIn("pending.written += written", flush)
        self.assertIn("if (written != bytes)", flush)
        self.assertNotIn("FILE_WRITE", flush)
        self.assertNotIn("f.print(", flush)
        for phase in range(500, 505):
            self.assertIn(f"CrashStage::Storage, {phase}", flush)
        enqueue = source.split("static bool chat_enqueue_line(", 1)[1].split("// Public API:", 1)[0]
        self.assertLess(enqueue.index("PendingChatGuard guard;"), enqueue.index("s_pending[i].active"))
        self.assertIn("for (int attempt = 0; attempt < 2; ++attempt)", enqueue)
        self.assertIn("if (attempt == 0 && !chat_flush_all_now()) return false;", enqueue)
        timed_queue = source.split("void chat_append_flush_if_due() {", 1)[1].split("bool chat_append_flush_sync()", 1)[0]
        self.assertNotIn("PendingChatGuard", timed_queue)
        main = (APP / "main.cpp").read_text(encoding="utf-8")
        self.assertLess(main.index("chat_persistence_init();"), main.index("g_uimesh->begin(LittleFS)"))
        timed = main.split("// Coalesced chat-line append flush.", 1)[1].split("bool login_pending", 1)[0]
        self.assertLess(timed.index("lvgl_lock(5)"), timed.index("chat_append_flush_if_due();"))
        self.assertLess(timed.index("chat_append_flush_if_due();"), timed.index("lvgl_unlock();"))

    def test_cpu_rotation_is_default_and_ppa_setup_is_gated(self):
        display = (APP / "display.cpp").read_text(encoding="utf-8")
        self.assertIn("#define CROWPANEL_PPA_ROTATION 0", display)
        self.assertIn("g_ppa_disabled           = !kUsePpaRotation", display)
        setup = display.split("bool display_init", 1)[1]
        self.assertLess(setup.index("if (kUsePpaRotation)"), setup.index("ppa_register_client("))
        flush = display.split("static void flush_cb(", 1)[1].split("static void", 1)[0]
        self.assertLess(flush.index("if (g_ppa_disabled || !s_ppa_srm || !s_ppa_done_sem)"), flush.index("ppa_do_scale_rotate_mirror("))
        self.assertIn("rotate90_ccw_cpu", flush)
        diagnostics = (APP / "crash_diagnostics.cpp").read_text(encoding="utf-8")
        self.assertIn("reason == ESP_RST_BROWNOUT", diagnostics)
        self.assertIn("not sampled, not exhausted", diagnostics)

    def test_rgb_interrupts_never_copy_psram_during_flash_writes(self):
        display = (APP / "display.cpp").read_text(encoding="utf-8")
        config = display.split("static esp_lcd_rgb_panel_config_t rgb_panel_config() {", 1)[1].split("bool display_init", 1)[0]
        self.assertIn("cfg.bounce_buffer_size_px = 0;", config)
        self.assertEqual(config.count("cfg.bounce_buffer_size_px ="), 1)
        self.assertIn("cfg.flags.fb_in_psram     = 1;", config)
        self.assertIn("cfg.num_fbs               = 2;", config)
        self.assertIn("cfg.timings.pclk_hz           = kActivePixelClockHz;", config)
        self.assertIn("RGB direct DMA (no PSRAM bounce ISR)", display)

    def test_spi_and_render_substeps_are_recorded_without_logging_or_allocations(self):
        source = (APP / "crash_diagnostics.cpp").read_text(encoding="utf-8")
        spi = source.split("void crash_diagnostics_spi(", 1)[1].split("void crash_diagnostics_mark_current", 1)[0]
        for forbidden in ("Serial.", "Preferences", "malloc", "Semaphore"):
            self.assertNotIn(forbidden, spi)
        self.assertIn("core >= 2", spi)
        self.assertIn("spi.phase = phase", spi)
        self.assertIn("last-SPI phase=", source)
        display = (APP / "display.cpp").read_text(encoding="utf-8")
        for context in range(1, 7):
            self.assertIn(f"CrashStage::Render, {context}", display)
        for context in (1, 2, 3):
            self.assertIn(f"CrashStage::Flush, {context}", display)
        self.assertIn("CrashStage::Flush, 1000 + sy", display)
        hal = (APP.parents[1] / "variants/crowpanel_dhe04005d/EspHal.h").read_text(encoding="utf-8")
        self.assertLess(hal.index("crash_diagnostics_spi(3,"), hal.index("spi_device_transmit(_spiHandle, &t)"))
        self.assertGreater(hal.index("crash_diagnostics_spi(4,"), hal.index("spi_device_transmit(_spiHandle, &t)"))

    def test_contact_save_snapshots_and_batches_without_holding_mesh_during_io(self):
        source = (APP / "main.cpp").read_text(encoding="utf-8")
        save = source.split("void saveContacts() {", 1)[1].split("void ui_set_devname()", 1)[0]
        self.assertIn("saving.test_and_set(std::memory_order_acquire)", save)
        self.assertIn("flag.clear(std::memory_order_release)", save)
        self.assertLess(save.index("mesh_try_lock"), save.index("startContactsIterator"))
        self.assertLess(save.index("mesh_unlock();"), save.index('_fs->open(tmp_path'))
        self.assertLess(save.index("mesh_unlock();"), save.index("lvgl_lock(5)"))
        self.assertLess(save.index("lvgl_lock(5)"), save.index('_fs->open(tmp_path'))
        self.assertIn("~RenderUnlock() { lvgl_unlock(); }", save)
        self.assertIn("count < MAX_CONTACTS", save)
        self.assertIn("file.write(staging, bytes)", save)
        self.assertIn("vTaskDelay(1)", save)
        self.assertLess(save.index("file.close();"), save.index("replaceContactsFile(tmp_path)"))
        for context in (200, 201, 202, 203):
            self.assertIn(f"CrashStage::Storage, {context}", save)
        replace = source.split("bool replaceContactsFile(", 1)[1].split("void loadContacts()", 1)[0]
        for context in range(210, 218):
            self.assertIn(f"CrashStage::Storage, {context}", replace)

    def test_housekeeping_breadcrumbs_identify_actual_operations(self):
        main = (APP / "main.cpp").read_text(encoding="utf-8")
        storage = main.split("// Debounced contact save", 1)[1].split("bool login_pending", 1)[0]
        self.assertNotIn("crash_diagnostics_mark(false, CrashStage::Storage);", storage)
        for context in (2, 3, 4, 5):
            self.assertIn(f"CrashStage::Storage, {context}", storage)
        self.assertIn("cpu_light_sleep_poll();\n  crash_diagnostics_mark(false, CrashStage::Idle);", main)
        timeout = main.split("if (g_del.kind != TargetKind::NONE &&", 1)[1].split("// PM ring:", 1)[0]
        self.assertLess(timeout.index("lvgl_lock(5)"), timeout.index("lv_textarea_set_text"))
        self.assertLess(timeout.index("lv_textarea_set_text"), timeout.index("lvgl_unlock()"))

    def test_evidence_is_saved_before_retained_record_is_cleared(self):
        source = (APP / "crash_diagnostics.cpp").read_text(encoding="utf-8")
        boot = source.split("void crash_diagnostics_boot()", 1)[1].split("void crash_diagnostics_mark(", 1)[0]
        self.assertLess(boot.index("persist_boot_evidence("), boot.index("words[i] = 0"))
        self.assertIn('prefs.getUInt("next", 0) % 4', source)
        self.assertIn("history write failed", source)
        self.assertIn("Current ELF SHA256", source)
        hook = source.split('extern "C" void IRAM_ATTR __wrap_panic_arch_fill_info', 1)[1]
        for forbidden in ("Preferences", "putString", "Serial.", "persist_boot_evidence", "String "):
            self.assertNotIn(forbidden, hook)

    def test_report_can_be_read_later_without_continuous_logging(self):
        source = (APP / "crash_diagnostics.cpp").read_text(encoding="utf-8")
        poll = source.split("void crash_diagnostics_poll_serial()", 1)[1].split("static void persist_boot_evidence", 1)[0]
        self.assertIn("count < 16", poll)
        self.assertIn('strcmp(command, "crash")', poll)
        self.assertNotIn("putString", poll)
        web = (APP / "web_dashboard.cpp").read_text(encoding="utf-8")
        self.assertIn('s_server.on("/api/crash", HTTP_GET', web)
        self.assertIn('response->addHeader("Cache-Control", "no-store")', web)
