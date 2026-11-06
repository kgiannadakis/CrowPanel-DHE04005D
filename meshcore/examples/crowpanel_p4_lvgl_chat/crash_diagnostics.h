#pragma once

#include <stdint.h>

enum class CrashStage : uint32_t {
  Boot, Idle, Input, UiWork, Translation, Radio, Network, Storage,
  Render, Flush, PpaWait, PpaReset, ChatOpen, HistoryRead, HistoryBubbles, HistoryLayout,
  Sleep
};

void crash_diagnostics_boot();
const char* crash_diagnostics_history();
void crash_diagnostics_poll_serial();
void crash_diagnostics_mark(bool lvgl, CrashStage stage, uint32_t context = 0);
void crash_diagnostics_mark_current(CrashStage stage, uint32_t context = 0);
void crash_diagnostics_sample(bool lvgl);
void crash_diagnostics_spi(uint32_t phase, uint32_t opcode, uint32_t bytes);
