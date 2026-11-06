#pragma once

#include <stdint.h>

namespace Battery {

constexpr uint32_t kPollMs = 5000;
constexpr uint32_t kStartupPollMs = 1000;
constexpr uint32_t kSettleMs = 15000;

constexpr uint32_t pollPeriodMs(bool have_reading) {
  return have_reading ? kPollMs : kStartupPollMs;
}

constexpr int percentFromMillivolts(int mv) {
  constexpr int curve[][2] = {
    {4100, 100}, {4000, 92}, {3900, 85}, {3850, 78}, {3800, 68},
    {3750, 55}, {3700, 42}, {3650, 32}, {3600, 25}, {3500, 17},
    {3400, 11}, {3300, 6}, {3200, 3}, {3100, 0},
  };
  if (mv >= curve[0][0]) return 100;
  for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (mv >= curve[i][0])
      return curve[i][1] + (mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) /
                              (curve[i - 1][0] - curve[i][0]);
  }
  return 0;
}

class Estimate {
 public:
  constexpr bool haveReading() const { return percent_ >= 0; }
  constexpr int percent() const { return percent_; }

  // Observe every loop, including between I2C polls, so rapid wake is noticed.
  constexpr void observeScreen(bool awake, uint32_t now) {
    if (awake == awake_) return;
    awake_ = awake;
    beginSettling(now);
  }

  constexpr bool sample(int mv, bool charging, uint32_t now) {
    if (mv < 2500 || mv > 5000) return false;
    if (!haveReading()) {
      percent_ = percentFromMillivolts(mv);
      charging_ = charging;
      filtered_mv_ = mv;
      return true;
    }
    if (charging != charging_) {
      charging_ = charging;
      beginSettling(now);
    }
    if (settling_) {
      if (uint32_t(now - settle_since_) < kSettleMs) return true;
      settling_ = false;
    }

    samples_[sample_index_] = mv;
    sample_index_ = (sample_index_ + 1) % 3;
    if (sample_count_ < 3) ++sample_count_;
    if (sample_count_ < 3) return true;

    int a = samples_[0], b = samples_[1], c = samples_[2];
    if (a > b) { const int t = a; a = b; b = t; }
    if (b > c) { const int t = b; b = c; c = t; }
    if (a > b) { const int t = a; a = b; b = t; }
    // Median rejects a single TX sag; the low-pass removes ADC jitter.
    filtered_mv_ = filtered_mv_ < 0 ? b : (3 * filtered_mv_ + b + 2) / 4;
    const int target = percentFromMillivolts(filtered_mv_);
    if (target < percent_) {
      const int step = percent_ - target > 2 ? 2 : percent_ - target;
      if (percent_ - target >= 2 || target == 0) percent_ -= step;
    } else if (charging_ && awake_ && target > percent_) {
      // Resting-voltage rebound is not newly stored charge. Never raise the
      // estimate while discharging or while the display load is absent.
      percent_ += target - percent_ > 2 ? 2 : target - percent_;
    }
    return true;
  }

 private:
  constexpr void beginSettling(uint32_t now) {
    settle_since_ = now;
    settling_ = true;
    sample_count_ = 0;
    sample_index_ = 0;
    filtered_mv_ = -1;
  }

  int percent_ = -1;
  int filtered_mv_ = -1;
  int samples_[3] = {};
  unsigned sample_count_ = 0;
  unsigned sample_index_ = 0;
  bool awake_ = true;
  bool charging_ = false;
  bool settling_ = false;
  uint32_t settle_since_ = 0;
};

}  // namespace Battery
