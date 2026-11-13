// status_led.h: WS2812 status LED (GPIO35), P1..P8 priority table (SRS §4).
// The selector and pattern math are pure (host-testable); output is device-only.
#pragma once

#include <stdint.h>

namespace led {

enum class Pattern : uint8_t {
  OFF,
  SOLID,
  STROBE_3HZ,      // 165 ms on / 165 ms off
  BLINK_2HZ,       // 250 ms on / 250 ms off
  PULSE_1HZ,       // 500 ms on / 500 ms off
  PULSE_SINE_1HZ,  // smooth 1 Hz pulse (commissioning)
  DOUBLE_FLASH,    // 150 on, 150 off, 150 on, then off
  BREATHE_025HZ,   // 4 s sine breathing
  RAPID_10HZ,      // factory-reset warning
  HEARTBEAT_1HZ,   // 100 on, 100 off, 100 on, 700 off (REST API pairing mode)
};

struct Inputs {
  bool factory_reset_warning = false;  // long press >= 10 s in progress
  bool pair_hold_ready = false;        // button held 3..10 s: releasing now opens pairing
  bool pairing_active = false;         // REST API pairing window open (SRS §5.3)
  bool sensor_alarm = false;           // ag_fail_count >= 5
  bool wifi_connected = true;
  bool matter_commissioned = true;
  int ag_fail_count = 0;               // 1..4 -> warning
  bool ir_flash_active = false;        // within the double-flash window after an IR transmission
  uint32_t ir_flash_seq = 0;           // increments on each IR TX so back-to-back flashes restart
  bool manual_mode = false;
  int current_speed = 0;
  bool is_night = false;
  bool night_led_enabled = false;
};

struct Output {
  uint8_t priority;      // 0 = user-gesture overrides (factory reset, pairing), 1..8 = SRS priorities
  uint32_t rgb;          // 0xRRGGBB
  uint8_t brightness;    // percent 0..100
  Pattern pattern;
};

constexpr uint32_t kDoubleFlashMs = 600;

Output select(const Inputs& in);

// Pattern intensity 0..255 at `t_ms` since the pattern became active.
uint8_t pattern_level(Pattern p, uint32_t t_ms);

// Final color after brightness and pattern.
void compose(const Output& o, uint32_t t_ms, uint8_t* r, uint8_t* g, uint8_t* b);

// Tracks active pattern state so back-to-back DOUBLE_FLASH events restart from t=0.
struct PatternState {
  uint8_t priority = 0xFF;
  uint32_t rgb = 0xFFFFFFFF;
  Pattern pattern = Pattern::OFF;
  uint32_t ir_flash_seq = 0;
};
bool update_pattern_state(PatternState* state, const Output& o, uint32_t ir_flash_seq);

}  // namespace led

#ifndef UNIT_TEST
class StatusLed {
 public:
  void begin(int gpio);
  // Call from the main loop; renders at 20 Hz.
  void loop(uint32_t now_ms, const led::Inputs& in);

 private:
  int gpio_ = -1;
  uint32_t last_render_ms_ = 0;
  led::PatternState state_;
  uint32_t pattern_start_ms_ = 0;
  uint8_t last_r_ = 1, last_g_ = 1, last_b_ = 1;  // force first write
};
#endif
