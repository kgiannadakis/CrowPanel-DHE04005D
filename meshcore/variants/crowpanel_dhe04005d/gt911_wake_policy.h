#pragma once
#include <stdint.h>

namespace TouchWake {
// Module_Switch1 (0x804D), bits 1:0: rising/falling/low/high.
constexpr int activeLevel(uint8_t config) {
  return (config & 3) == 0 || (config & 3) == 3 ? 1 : 0;
}

class Verification {
 public:
  constexpr bool observe(bool pressed, int level, int active, bool buffer_idle = false) {
    if (active < 0 || active > 1 || level < 0 || level > 1) return false;
    if (buffer_idle && level == active) {
      if (++bad_idle_samples_ >= 3) { reset(); return false; }
    } else {
      bad_idle_samples_ = 0;
    }
    if (!pressed && level != active) saw_idle_ = true;
    if (pressed && level == active) saw_press_ = true;
    return saw_idle_ && saw_press_;
  }
  constexpr void reset() {
    saw_idle_ = saw_press_ = false;
    bad_idle_samples_ = 0;
  }

 private:
  bool saw_idle_ = false;
  bool saw_press_ = false;
  unsigned bad_idle_samples_ = 0;
};
}  // namespace TouchWake
