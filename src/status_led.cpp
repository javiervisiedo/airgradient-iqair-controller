// status_led.cpp: P1..P8 selection (host + device), WS2812 rendering (device).
#include "status_led.h"

#include <math.h>

namespace led {

namespace {
constexpr uint32_t kRed = 0xFF0000, kBlue = 0x0055FF, kMagenta = 0xFF00FF, kAmber = 0xFFAA00,
                   kWhite = 0xFFFFFF, kPurple = 0x9900FF, kOrangeRed = 0xFF3300, kCyan = 0x00E5FF,
                   kEmerald = 0x00C853;

Output make(uint8_t prio, uint32_t rgb, uint8_t day, uint8_t night, bool is_night, Pattern p) {
  return Output{prio, rgb, is_night ? night : day, p};
}

// Normal-status (P5..P8) brightness at night: 0 % unless night_led_enabled,
// in which case a quarter of the day brightness is used.
uint8_t normal_night(uint8_t day, const Inputs& in) {
  return in.night_led_enabled ? static_cast<uint8_t>(day / 4) : 0;
}

uint8_t sine_level(uint32_t t_ms, uint32_t period_ms) {
  const float ph = static_cast<float>(t_ms % period_ms) / static_cast<float>(period_ms);
  const float v = 0.5f - 0.5f * cosf(ph * 6.2831853f);
  return static_cast<uint8_t>(lroundf(v * 255.0f));
}
}  // namespace

Output select(const Inputs& in) {
  if (in.factory_reset_warning) return Output{0, kMagenta, 100, Pattern::RAPID_10HZ};
  // User-initiated pairing feedback: shown day and night (overrides night silence).
  if (in.pair_hold_ready) return make(0, kCyan, 80, 40, in.is_night, Pattern::SOLID);
  if (in.pairing_active) return make(0, kCyan, 80, 40, in.is_night, Pattern::HEARTBEAT_1HZ);
  if (in.sensor_alarm) return make(1, kRed, 80, 40, in.is_night, Pattern::STROBE_3HZ);
  if (!in.matter_commissioned) return make(2, kMagenta, 60, 20, in.is_night, Pattern::PULSE_SINE_1HZ);
  if (!in.wifi_connected) return make(2, kBlue, 60, 20, in.is_night, Pattern::BLINK_2HZ);
  if (in.ag_fail_count >= 1 && in.ag_fail_count <= 4) return make(3, kAmber, 50, 15, in.is_night, Pattern::PULSE_1HZ);
  if (in.ir_flash_active) return make(4, kWhite, 70, 10, in.is_night, Pattern::DOUBLE_FLASH);
  if (in.manual_mode) {
    return Output{5, kPurple, in.is_night ? normal_night(40, in) : static_cast<uint8_t>(40), Pattern::BREATHE_025HZ};
  }
  if (in.current_speed >= 4) {
    return Output{6, kOrangeRed, in.is_night ? normal_night(40, in) : static_cast<uint8_t>(40), Pattern::SOLID};
  }
  if (in.current_speed >= 2) {
    return Output{7, kCyan, in.is_night ? normal_night(30, in) : static_cast<uint8_t>(30), Pattern::SOLID};
  }
  return Output{8, kEmerald, in.is_night ? normal_night(20, in) : static_cast<uint8_t>(20), Pattern::SOLID};
}

uint8_t pattern_level(Pattern p, uint32_t t) {
  switch (p) {
    case Pattern::OFF: return 0;
    case Pattern::SOLID: return 255;
    case Pattern::STROBE_3HZ: return (t % 330) < 165 ? 255 : 0;
    case Pattern::BLINK_2HZ: return (t % 500) < 250 ? 255 : 0;
    case Pattern::PULSE_1HZ: return (t % 1000) < 500 ? 255 : 0;
    case Pattern::PULSE_SINE_1HZ: return sine_level(t, 1000);
    case Pattern::DOUBLE_FLASH: return (t < 150 || (t >= 300 && t < 450)) ? 255 : 0;
    case Pattern::BREATHE_025HZ: return sine_level(t, 4000);
    case Pattern::RAPID_10HZ: return (t % 100) < 50 ? 255 : 0;
    case Pattern::HEARTBEAT_1HZ: {
      const uint32_t ph = t % 1000;
      return (ph < 100 || (ph >= 200 && ph < 300)) ? 255 : 0;
    }
  }
  return 0;
}

void compose(const Output& o, uint32_t t_ms, uint8_t* r, uint8_t* g, uint8_t* b) {
  const uint32_t lvl = pattern_level(o.pattern, t_ms);
  const uint32_t scale = lvl * o.brightness;  // 0..25500
  *r = static_cast<uint8_t>((((o.rgb >> 16) & 0xFF) * scale) / 25500);
  *g = static_cast<uint8_t>((((o.rgb >> 8) & 0xFF) * scale) / 25500);
  *b = static_cast<uint8_t>(((o.rgb & 0xFF) * scale) / 25500);
}

bool update_pattern_state(PatternState* state, const Output& o, uint32_t ir_flash_seq) {
  const bool flash_retriggered =
      (o.pattern == Pattern::DOUBLE_FLASH && ir_flash_seq != state->ir_flash_seq);
  if (o.priority != state->priority || o.rgb != state->rgb || o.pattern != state->pattern || flash_retriggered) {
    state->priority = o.priority;
    state->rgb = o.rgb;
    state->pattern = o.pattern;
    if (o.pattern == Pattern::DOUBLE_FLASH) state->ir_flash_seq = ir_flash_seq;
    return true;
  }
  return false;
}

}  // namespace led

#ifndef UNIT_TEST
#include <Arduino.h>

void StatusLed::begin(int gpio) {
  gpio_ = gpio;
  rgbLedWrite(gpio_, 0, 0, 0);
  last_r_ = last_g_ = last_b_ = 0;
}

void StatusLed::loop(uint32_t now_ms, const led::Inputs& in) {
  if (gpio_ < 0 || now_ms - last_render_ms_ < 50) return;  // 20 Hz
  last_render_ms_ = now_ms;
  const led::Output o = led::select(in);
  if (led::update_pattern_state(&state_, o, in.ir_flash_seq)) {
    pattern_start_ms_ = now_ms;
  }
  uint8_t r, g, b;
  led::compose(o, now_ms - pattern_start_ms_, &r, &g, &b);
  if (r != last_r_ || g != last_g_ || b != last_b_) {  // write only on change
    rgbLedWrite(gpio_, r, g, b);
    last_r_ = r;
    last_g_ = g;
    last_b_ = b;
  }
}
#endif
