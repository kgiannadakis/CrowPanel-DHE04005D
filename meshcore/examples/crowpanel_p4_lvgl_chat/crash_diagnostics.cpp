#include "crash_diagnostics.h"
#include "mesh_api.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <esp_private/panic_internal.h>
#include <riscv/rvruntime-frames.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr uint32_t kMagic = 0x43524433;
struct TaskRecord {
  uint32_t stage, tick, stack_free, context;
};
struct PanicRecord {
  uint32_t valid, pc, ra, sp, cause, address;
};
struct Record {
  uint32_t magic;
  char build[40];
  TaskRecord tasks[2];
  PanicRecord panic[2];
  uint32_t internal_free, internal_largest;
  struct { uint32_t phase, opcode, bytes, tick; } spi[2];
};
RTC_NOINIT_ATTR volatile Record s_record;
uint32_t s_last_sample[2] = {};
String s_history; // Initialized at boot, immutable once the web server starts.

const char* stage_name(uint32_t stage) {
  static const char* names[] = {"boot", "idle", "input", "ui-work", "translation",
    "radio", "network", "storage", "render", "flush", "ppa-wait", "ppa-reset",
    "chat-open", "history-read", "history-bubbles", "history-layout", "sleep"};
  return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}
}

const char* crash_diagnostics_history() { return s_history.c_str(); }

void crash_diagnostics_poll_serial() {
  static char command[16];
  static unsigned used = 0;
  static bool overflow = false;
  for (unsigned count = 0; count < 16 && Serial.available(); ++count) {
    const int ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      command[used] = 0;
      if (!overflow && !strcmp(command, "crash")) Serial.println(s_history);
      if (!overflow && !strcmp(command, "rx")) Serial.println(mesh_rx_report());
      used = 0;
      overflow = false;
    } else if (used + 1 < sizeof(command)) command[used++] = (char)ch;
    else overflow = true;
  }
}

static void persist_boot_evidence(esp_reset_reason_t reason, bool retained) {
  String report;
  report.reserve(1400);
  char line[256];
  snprintf(line, sizeof(line), "Current build: %s %s\nApp reset reason: %d\n", __DATE__, __TIME__, (int)reason);
  report += line;
  if (reason == ESP_RST_BROWNOUT)
    report += "Reset classification: BROWNOUT (supply voltage detector).\n";
  else if (reason == ESP_RST_WDT)
    report += "Reset classification: hardware/other WATCHDOG; no captured backtrace implied.\n";
  report += "Current ELF SHA256: ";
  const esp_app_desc_t* app = esp_app_get_description();
  for (unsigned i = 0; i < sizeof(app->app_elf_sha256); ++i) {
    snprintf(line, sizeof(line), "%02x", app->app_elf_sha256[i]);
    report += line;
  }
  report += '\n';
  report += "Selector may mask the original reset. Software reset alone is not proof of a crash.\n";
  if (retained) {
    char build[sizeof(s_record.build)];
    for (size_t i = 0; i < sizeof(build); ++i) build[i] = s_record.build[i];
    build[sizeof(build) - 1] = 0;
    report += "Previous build: "; report += build; report += '\n';
    for (unsigned i = 0; i < 2; ++i) {
      const volatile TaskRecord& t = s_record.tasks[i];
      snprintf(line, sizeof(line), "%s last=%s context=%lu at=%lums stack-free=%lu bytes\n",
               i ? "lvgl" : "main", stage_name(t.stage), (unsigned long)t.context,
               (unsigned long)(t.tick * portTICK_PERIOD_MS), (unsigned long)t.stack_free);
      report += line;
      snprintf(line, sizeof(line), "core%u last-SPI phase=%lu opcode=0x%02lX bytes=%lu at=%lums\n",
               i, (unsigned long)s_record.spi[i].phase, (unsigned long)s_record.spi[i].opcode,
               (unsigned long)s_record.spi[i].bytes,
               (unsigned long)(s_record.spi[i].tick * portTICK_PERIOD_MS));
      report += line;
      const volatile PanicRecord& p = s_record.panic[i];
      if (p.valid == kMagic) {
        snprintf(line, sizeof(line), "core%u PC=%08lX RA=%08lX SP=%08lX cause=%08lX address=%08lX\n",
                 i, (unsigned long)p.pc, (unsigned long)p.ra, (unsigned long)p.sp,
                 (unsigned long)p.cause, (unsigned long)p.address);
        report += line;
      }
    }
    snprintf(line, sizeof(line), "Internal heap free=%lu largest=%lu\n",
             (unsigned long)s_record.internal_free, (unsigned long)s_record.internal_largest);
    report += line;
    report += "Stages are breadcrumbs, not proof of cause; absent panic registers mean no captured exception.\n";
    report += "Zero stack/heap samples during early boot can mean not sampled, not exhausted.\n";
  } else report += "No trustworthy retained context; power loss/selector/firmware changes can erase it.\n";

  Preferences prefs;
  if (!prefs.begin("crashlog", false)) {
    s_history = "Persistent history unavailable (NVS open failed).\n" + report;
    return;
  }
  const unsigned next = prefs.getUInt("next", 0) % 4;
  char key[8];
  snprintf(key, sizeof(key), "boot%u", next);
  const bool saved = prefs.putString(key, report) == report.length();
  const bool advanced = saved && prefs.putUInt("next", (next + 1) % 4) == sizeof(uint32_t);
  s_history = "Reboot history: four slots, newest first. No wall-clock timestamp available this early in boot.\n";
  if (!advanced) { s_history += "WARNING: history write failed; current RAM report:\n"; s_history += report; }
  const unsigned newest = saved ? next : (next + 3) % 4;
  for (unsigned i = 0; i < 4; ++i) {
    snprintf(key, sizeof(key), "boot%u", (newest + 4 - i) % 4);
    const String entry = prefs.getString(key, "");
    if (entry.length()) { s_history += "\n--- reboot ---\n"; s_history += entry; }
  }
  prefs.end();
  Serial.printf("[crash] reboot history %s; retrieve /api/crash or type crash + Enter\n", advanced ? "saved" : "WRITE FAILED");
}

void crash_diagnostics_boot() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_WDT ||
                       reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT;
  // The dual-boot selector can replace the original watchdog reset with SW.
  // Report retained evidence in that case without claiming SW itself is a crash.
  const bool selector_restart = reason == ESP_RST_SW && s_record.magic == kMagic;
  persist_boot_evidence(reason, (crashed || selector_restart) && s_record.magic == kMagic);
  if ((crashed || selector_restart) && s_record.magic == kMagic) {
    if (selector_restart)
      Serial.println("[crash] retained activity after software/selector restart; original reset cause unknown (not proof of a crash)");
    char build[sizeof(s_record.build)];
    for (size_t i = 0; i < sizeof(build); ++i) build[i] = s_record.build[i];
    build[sizeof(build) - 1] = 0;
    Serial.printf("[crash] previous build: %s\n", build);
    for (unsigned i = 0; i < 2; ++i) {
      const volatile TaskRecord& task = s_record.tasks[i];
      Serial.printf("[crash] %s last=%s context=%lu at=%lums stack-free=%lu bytes\n",
                    i ? "lvgl" : "main", stage_name(task.stage),
                    (unsigned long)task.context,
                    (unsigned long)(task.tick * portTICK_PERIOD_MS), (unsigned long)task.stack_free);
      const volatile PanicRecord& panic = s_record.panic[i];
      if (panic.valid == kMagic)
        Serial.printf("[crash] core%u PC=0x%08lX RA=0x%08lX SP=0x%08lX cause=0x%08lX address=0x%08lX\n",
                      i, (unsigned long)panic.pc, (unsigned long)panic.ra,
                      (unsigned long)panic.sp, (unsigned long)panic.cause, (unsigned long)panic.address);
    }
    Serial.printf("[crash] sampled internal-free=%lu largest=%lu\n",
                  (unsigned long)s_record.internal_free, (unsigned long)s_record.internal_largest);
    if (s_record.panic[0].valid != kMagic && s_record.panic[1].valid != kMagic)
      Serial.println("[crash] panic capture not reached; stages are the last known activity, not proof of cause");
  } else if (crashed) {
    Serial.println("[crash] no retained diagnostics from the previous firmware/reset");
  }
  volatile uint32_t* words = reinterpret_cast<volatile uint32_t*>(&s_record);
  for (size_t i = 0; i < sizeof(s_record) / sizeof(uint32_t); ++i) words[i] = 0;
  const char build[] = __DATE__ " " __TIME__;
  for (size_t i = 0; i < sizeof(build); ++i) s_record.build[i] = build[i];
  s_record.magic = kMagic;
}

void crash_diagnostics_mark(bool lvgl, CrashStage stage, uint32_t context) {
  volatile TaskRecord& task = s_record.tasks[lvgl ? 1 : 0];
  task.tick = xTaskGetTickCount();
  task.context = context;
  task.stage = static_cast<uint32_t>(stage);
}

void crash_diagnostics_spi(uint32_t phase, uint32_t opcode, uint32_t bytes) {
  const unsigned core = xPortGetCoreID();
  if (core >= 2) return;
  auto& spi = s_record.spi[core];
  spi.opcode = opcode;
  spi.bytes = bytes;
  spi.tick = xTaskGetTickCount();
  spi.phase = phase;
}

void crash_diagnostics_mark_current(CrashStage stage, uint32_t context) {
  crash_diagnostics_mark(strcmp(pcTaskGetName(nullptr), "lvgl") == 0, stage, context);
}

void crash_diagnostics_sample(bool lvgl) {
  const unsigned index = lvgl ? 1 : 0;
  const uint32_t now = xTaskGetTickCount();
  if (s_last_sample[index] && now - s_last_sample[index] < pdMS_TO_TICKS(5000)) return;
  s_last_sample[index] = now;
  s_record.tasks[index].stack_free = uxTaskGetStackHighWaterMark(nullptr);
  if (!lvgl) {
    s_record.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_record.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
}

extern "C" void __real_panic_arch_fill_info(void* frame, panic_info_t* info);

// No logging, allocation, locks, or flash strings in the panic hook.
extern "C" void IRAM_ATTR __wrap_panic_arch_fill_info(void* frame, panic_info_t* info) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(frame);
  if (s_record.magic == kMagic && start && !(start & 3) &&
      esp_ptr_in_dram(frame) && esp_ptr_in_dram(reinterpret_cast<void*>(start + sizeof(RvExcFrame) - 1))) {
    const volatile RvExcFrame* registers = static_cast<const volatile RvExcFrame*>(frame);
    const unsigned core = registers->mhartid;
    if (core < 2 && s_record.panic[core].valid != kMagic) {
      volatile PanicRecord& panic = s_record.panic[core];
      panic.pc = registers->mepc;
      panic.ra = registers->ra;
      panic.sp = registers->sp;
      panic.cause = registers->mcause;
      panic.address = registers->mtval;
      panic.valid = kMagic;
    }
  }
  __real_panic_arch_fill_info(frame, info);
}
