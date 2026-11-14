// Native host unit tests (PlatformIO `pio test -e native`).
// Acceptance cases TC-01..TC-07 from docs/main_requirement_spec.md §8 are covered
// at the logic level; TC-01 (commissioning) and TC-02 (network time) also need the
// hardware/system test, see docs in the test names.
#include <math.h>
#include <string.h>
#include <unity.h>

#include "ag_client.h"
#include "api_auth.h"
#include "clock_sync.h"
#include "iaq_algorithm.h"
#include "ir_nec_purifier.h"
#include "matter_ha_bridge.h"
#include "sim_inject.h"
#include "status_led.h"
#include "sys_config.h"

using namespace iaq;

void setUp() {}
void tearDown() {}

// ----------------------------------------------------------------- helpers
static Reading reading(float pm, float tvoc = 100.0f, float nox = 1.0f) {
  Reading r;
  r.pm25_valid = true;
  r.pm25 = pm;
  r.tvoc_valid = true;
  r.tvoc_index = tvoc;
  r.nox_valid = true;
  r.nox_index = nox;
  return r;
}

static AppConfig defaults() {
  AppConfig c;
  config_set_defaults(&c);
  return c;
}

struct Rig {
  AppConfig cfg = defaults();
  ControlCore core;
  uint32_t now = 0;
  Rig() { core.begin(cfg, 0); }
  TickResult ok(float pm, float tvoc = 100.0f, float nox = 1.0f, bool night = false) {
    now += 60000;
    return core.on_poll(true, reading(pm, tvoc, nox), night, now);
  }
  TickResult fail(bool night = false) {
    now += 60000;
    return core.on_poll(false, Reading{}, night, now);
  }
};

static led::Inputs led_inputs(const Rig& rig, bool night) {
  led::Inputs in;
  in.sensor_alarm = rig.core.fault().alarm();
  in.ag_fail_count = rig.core.fault().fail_count();
  in.manual_mode = rig.core.modes().mode() == OperatingMode::MANUAL;
  in.current_speed = rig.core.commanded_speed();
  in.is_night = night;
  return in;
}

// =================================================================== TC-01
// Matter slider / mode -> operating mode (logic part of TC-01).
void test_TC01_matter_slider_sets_manual_and_auto_resumes() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);  // AUTO -> speed 3
  TEST_ASSERT_EQUAL(3, rig.core.commanded_speed());
  // Apple/Google Home slider at 67 % -> speed 4, MANUAL
  const int s = percent_to_speed(67);
  TEST_ASSERT_EQUAL(4, s);
  rig.core.modes().set_manual(static_cast<uint8_t>(s), rig.now);
  TickResult r = rig.core.on_mode_change(rig.now, false);
  TEST_ASSERT_EQUAL(OperatingMode::MANUAL, rig.core.modes().mode());
  TEST_ASSERT_EQUAL(4, r.commanded_speed);
  TEST_ASSERT_TRUE(r.speed_changed);
  // Tile back to Auto -> immediately resumes the sensor-calculated Auto speed (3)
  rig.core.modes().set_auto(rig.now);
  r = rig.core.on_mode_change(rig.now, false);
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, rig.core.modes().mode());
  TEST_ASSERT_EQUAL(3, r.commanded_speed);
  TEST_ASSERT_TRUE(r.speed_changed);

  // Clean air (Auto speed 0), then Manual top speed (6), then back to Auto -> immediately 0
  Rig clean_rig;
  for (int i = 0; i < 3; ++i) clean_rig.ok(0.0f, 35.0f, 1.0f);
  TEST_ASSERT_EQUAL(0, clean_rig.core.commanded_speed());
  clean_rig.core.modes().set_manual(6, clean_rig.now);
  r = clean_rig.core.on_mode_change(clean_rig.now, false);
  TEST_ASSERT_EQUAL(6, r.commanded_speed);
  TEST_ASSERT_TRUE(r.speed_changed);
  clean_rig.core.modes().set_auto(clean_rig.now);
  r = clean_rig.core.on_mode_change(clean_rig.now, false);
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, clean_rig.core.modes().mode());
  TEST_ASSERT_EQUAL(0, r.commanded_speed);
  TEST_ASSERT_TRUE(r.speed_changed);
}

void test_TC01_percent_speed_mapping_roundtrip() {
  for (int s = 0; s <= 6; ++s) TEST_ASSERT_EQUAL(s, percent_to_speed(speed_to_percent(s)));
  TEST_ASSERT_EQUAL(1, percent_to_speed(1));    // any non-zero percent is at least speed 1
  TEST_ASSERT_EQUAL(6, percent_to_speed(100));
  TEST_ASSERT_EQUAL_UINT8(17, speed_to_percent(1));
  TEST_ASSERT_EQUAL_UINT8(100, speed_to_percent(6));
}

// =================================================================== TC-02
// Clock logic part: HTTP Date parsing and unsynced Day fallback.
void test_TC02_http_date_parse() {
  time_t t;
  TEST_ASSERT_TRUE(clk::parse_http_date("Mon, 09 Nov 2026 06:30:00 GMT", &t));
  TEST_ASSERT_EQUAL_INT64(1794205800LL, static_cast<long long>(t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 06:30:00 PST", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("garbage", &t));
  TEST_ASSERT_EQUAL_INT64(946684800LL, static_cast<long long>(clk::matter_utc_us_to_unix(0)));
}

void test_TC02_unsynced_clock_falls_back_to_day_unless_night_force() {
  TEST_ASSERT_FALSE(resolve_is_night(false, 2, false, 8, 23));  // 02:00 but no clock -> Day
  TEST_ASSERT_TRUE(resolve_is_night(false, 12, true, 8, 23));   // Night Quiet Lock
  TEST_ASSERT_TRUE(resolve_is_night(true, 23, false, 8, 23));
  TEST_ASSERT_TRUE(resolve_is_night(true, 7, false, 8, 23));
  TEST_ASSERT_FALSE(resolve_is_night(true, 8, false, 8, 23));
  TEST_ASSERT_FALSE(resolve_is_night(true, 22, false, 8, 23));
  TEST_ASSERT_EQUAL_STRING("unsynced_fallback_day", clk::status_to_str(clk::Status::UNSYNCED_FALLBACK_DAY));
}

// =================================================================== TC-03
void test_TC03_wake_from_off_to_speed4_sequence() {
  const AppConfig c = defaults();
  ir::Step steps[ir::kMaxSteps];
  const int n = ir::plan_transition(0, 4, false, c.wake_require_speed1_prefix, c.ir_codes, steps);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_HEX16(0xE817, steps[0].command_word);
  TEST_ASSERT_EQUAL(0, steps[0].delay_before_ms);
  TEST_ASSERT_EQUAL_HEX16(0xFA05, steps[1].command_word);
  TEST_ASSERT_EQUAL(600, steps[1].delay_before_ms);
  TEST_ASSERT_EQUAL(2, ir::kFramesPerCommand);
  TEST_ASSERT_EQUAL(100, ir::kFrameGapMs);
  // On-air frames match the verified captures (IRremote decodedRawData form)
  TEST_ASSERT_EQUAL_HEX32(0xE817FF00u, ir::nec_raw32(c.ir_nec_address, 0xE817));
  TEST_ASSERT_EQUAL_HEX32(0xFA05FF00u, ir::nec_raw32(c.ir_nec_address, 0xFA05));
}

void test_TC03_nec_symbols_and_other_transitions() {
  ir::NecSymbol sym[ir::kNecSymbols];
  ir::nec_encode(0xE817FF00u, sym);
  TEST_ASSERT_EQUAL(9000, sym[0].mark_us);
  TEST_ASSERT_EQUAL(4500, sym[0].space_us);
  for (int i = 0; i < 8; ++i) TEST_ASSERT_EQUAL(560, sym[1 + i].space_us);    // address 0x00
  for (int i = 8; i < 16; ++i) TEST_ASSERT_EQUAL(1690, sym[1 + i].space_us);  // ~address 0xFF
  TEST_ASSERT_EQUAL(1690, sym[17].space_us);  // cmd 0x17 bit0 = 1
  TEST_ASSERT_EQUAL(0, sym[33].space_us);
  const AppConfig c = defaults();
  ir::Step st[ir::kMaxSteps];
  TEST_ASSERT_EQUAL(1, ir::plan_transition(0, 1, false, true, c.ir_codes, st));  // 0 -> 1: no prefix
  TEST_ASSERT_EQUAL_HEX16(0xE817, st[0].command_word);
  TEST_ASSERT_EQUAL(1, ir::plan_transition(3, 0, false, true, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xFF00, st[0].command_word);
  TEST_ASSERT_EQUAL(0, ir::plan_transition(3, 3, false, true, c.ir_codes, st));  // no-op
  TEST_ASSERT_EQUAL(1, ir::plan_transition(3, 3, true, true, c.ir_codes, st));   // forced refresh
  TEST_ASSERT_EQUAL(1, ir::plan_transition(0, 5, false, false, c.ir_codes, st)); // prefix disabled
}

// =================================================================== TC-04
void test_TC04_room_volume_patch_rescales() {
  AppConfig c = defaults();
  uint32_t mask = 0;
  char err[96];
  const char* body = "{\"room_volume_m3\": 250.0}";
  TEST_ASSERT_TRUE(config_apply_json_patch(&c, body, strlen(body), &mask, err, sizeof(err)));
  TEST_ASSERT_TRUE(mask & CFG_CHANGED_VOLUME);
  DualPathController algo;
  algo.set_volume(c.room_volume_m3);
  const VolumeScaling& s = algo.scaling();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7746f, s.rho);
  TEST_ASSERT_EQUAL(7, s.min_stepdown_hold);
  const float expect_up[4] = {15.5f, 27.1f, 42.6f, 65.9f};
  for (int i = 0; i < 4; ++i) TEST_ASSERT_FLOAT_WITHIN(0.1f, expect_up[i], s.day_up[2 + i]);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, s.day_up[0]);   // fixed WHO/EPA floor thresholds
  TEST_ASSERT_EQUAL_FLOAT(12.0f, s.day_up[1]);
  // 150 m3 reproduces the v3.0 table exactly
  const VolumeScaling b = compute_volume_scaling(150.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, b.rho);
  TEST_ASSERT_EQUAL(4, b.min_stepdown_hold);
  TEST_ASSERT_EQUAL_FLOAT(85.0f, b.day_up[5]);
  TEST_ASSERT_EQUAL_FLOAT(65.0f, b.day_down[5]);
  // clamps
  TEST_ASSERT_EQUAL_FLOAT(1.35f, compute_volume_scaling(30.0f).rho);
  TEST_ASSERT_EQUAL(8, compute_volume_scaling(500.0f).min_stepdown_hold);
  TEST_ASSERT_EQUAL(2, compute_volume_scaling(30.0f).min_stepdown_hold);
}

// =================================================================== TC-05
// Day: failed polls 1..14 hold the speed; amber warning for 1..4.
void test_TC05_day_dropout_holds_speed() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);
  TEST_ASSERT_EQUAL(3, rig.core.commanded_speed());
  for (int i = 1; i <= 3; ++i) {
    const TickResult r = rig.fail();
    TEST_ASSERT_EQUAL(i, rig.core.fault().fail_count());
    TEST_ASSERT_FALSE(r.speed_changed);
    TEST_ASSERT_FALSE(r.force_transmit);
    TEST_ASSERT_EQUAL(3, r.commanded_speed);
  }
  TEST_ASSERT_EQUAL(AgStatus::DEGRADED, rig.core.fault().status());
  TEST_ASSERT_FALSE(rig.core.fault().alarm());
  led::Output o = led::select(led_inputs(rig, false));
  TEST_ASSERT_EQUAL(3, o.priority);
  TEST_ASSERT_EQUAL_HEX32(0xFFAA00, o.rgb);
  TEST_ASSERT_EQUAL(50, o.brightness);
  TEST_ASSERT_EQUAL(led::Pattern::PULSE_1HZ, o.pattern);
  o = led::select(led_inputs(rig, true));
  TEST_ASSERT_EQUAL(15, o.brightness);
}

// Night: hold for failed polls 1..2, cap at min(speed, 1) on the 3rd.
void test_TC05_night_dropout_caps_at_speed1_after_3_polls() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(40.0f, 100.0f, 1.0f, true);  // night: 35 < 40 -> speed 4
  TEST_ASSERT_EQUAL(4, rig.core.commanded_speed());
  for (int i = 1; i <= 2; ++i) {
    const TickResult r = rig.fail(true);
    TEST_ASSERT_FALSE(r.speed_changed);
    TEST_ASSERT_EQUAL(4, r.commanded_speed);
  }
  const TickResult cap = rig.fail(true);
  TEST_ASSERT_TRUE(cap.speed_changed);
  TEST_ASSERT_EQUAL(1, cap.commanded_speed);
  TEST_ASSERT_EQUAL(1, rig.core.algo().current_speed());  // algorithm re-anchored
  TEST_ASSERT_FALSE(rig.fail(true).speed_changed);       // no repeat
}

// =================================================================== TC-06
// 5 failed polls: alarm raised (LED, Matter fault) but no IR action of its own.
void test_TC06_five_failures_raise_alarm_without_shutoff() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);
  TickResult r{};
  for (int i = 0; i < 5; ++i) r = rig.fail();
  TEST_ASSERT_EQUAL(SensorFaultMonitor::Event::ENTERED_ALARM, r.fault_event);
  TEST_ASSERT_TRUE(rig.core.fault().alarm());
  TEST_ASSERT_EQUAL(3, r.commanded_speed);  // day: still holding
  TEST_ASSERT_FALSE(r.speed_changed);
  TEST_ASSERT_FALSE(r.force_transmit);
  TEST_ASSERT_EQUAL_STRING("unreachable_alarm", ag_status_to_str(rig.core.fault().status()));
  led::Output o = led::select(led_inputs(rig, false));
  TEST_ASSERT_EQUAL(1, o.priority);
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, o.rgb);
  TEST_ASSERT_EQUAL(80, o.brightness);
  TEST_ASSERT_EQUAL(led::Pattern::STROBE_3HZ, o.pattern);
  TEST_ASSERT_EQUAL(40, led::select(led_inputs(rig, true)).brightness);
  // further failures keep the alarm without re-triggering
  r = rig.fail();
  TEST_ASSERT_EQUAL(SensorFaultMonitor::Event::NONE, r.fault_event);
  TEST_ASSERT_FALSE(r.force_transmit);
}

// Day: hold for 14 failed polls, cap at min(speed, 3) on the 15th.
void test_TC06_day_cap_at_speed3_after_15_polls() {
  Rig rig;
  rig.ok(90.0f);  // Turbo
  TEST_ASSERT_EQUAL(6, rig.core.commanded_speed());
  for (int i = 1; i <= 14; ++i) {
    const TickResult r = rig.fail();
    TEST_ASSERT_FALSE(r.speed_changed);
    TEST_ASSERT_EQUAL(6, r.commanded_speed);
  }
  const TickResult cap = rig.fail();
  TEST_ASSERT_TRUE(cap.speed_changed);
  TEST_ASSERT_EQUAL(3, cap.commanded_speed);
  ir::Step st[ir::kMaxSteps];
  const AppConfig c = defaults();
  TEST_ASSERT_EQUAL(1, ir::plan_transition(6, 3, cap.force_transmit, true, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xFD02, st[0].command_word);
  TEST_ASSERT_FALSE(rig.fail().speed_changed);
}

// Caps never raise the speed, and an outage that runs into the night is capped then.
void test_watchdog_caps_never_raise_and_follow_day_night() {
  TEST_ASSERT_EQUAL(0, watchdog_cap(0, 20, true));
  TEST_ASSERT_EQUAL(1, watchdog_cap(1, 20, true));
  TEST_ASSERT_EQUAL(2, watchdog_cap(2, 14, false));
  TEST_ASSERT_EQUAL(2, watchdog_cap(2, 15, false));
  TEST_ASSERT_EQUAL(5, watchdog_cap(5, 2, true));
  Rig rig;
  rig.ok(60.0f);  // day speed 5
  TEST_ASSERT_EQUAL(5, rig.core.commanded_speed());
  for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL(5, rig.fail(false).commanded_speed);  // day hold
  const TickResult n = rig.fail(true);  // 23:00 arrives mid-outage
  TEST_ASSERT_EQUAL(1, n.commanded_speed);
  TEST_ASSERT_EQUAL(1, rig.fail(false).commanded_speed);  // morning: never raised again
}

// =================================================================== TC-07
void test_TC07_recovery_after_alarm() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);
  for (int i = 0; i < 6; ++i) rig.fail();  // ticks 1..6 unreachable (day: speed held at 3)
  const TickResult r = rig.ok(28.0f);      // tick 7: pm02 = 28.0
  TEST_ASSERT_EQUAL(SensorFaultMonitor::Event::RECOVERED, r.fault_event);
  TEST_ASSERT_FALSE(rig.core.fault().alarm());
  TEST_ASSERT_EQUAL(0, rig.core.fault().fail_count());
  float pm_avg = 0;
  TEST_ASSERT_TRUE(rig.core.algo().pm_avg(&pm_avg));
  TEST_ASSERT_EQUAL_FLOAT(28.0f, pm_avg);  // stale pre-outage samples aged out
  TEST_ASSERT_EQUAL(3, r.commanded_speed); // 20 < 28 <= 35 -> speed 3
  TEST_ASSERT_TRUE(r.force_transmit);      // re-assert after an outage
  ir::Step st[ir::kMaxSteps];
  const AppConfig c = defaults();
  TEST_ASSERT_EQUAL(1, ir::plan_transition(3, r.commanded_speed, true, true, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xFD02, st[0].command_word);
  led::Output day = led::select(led_inputs(rig, false));
  TEST_ASSERT_EQUAL(7, day.priority);  // normal display: cyan for speeds 2-3
  TEST_ASSERT_EQUAL(0, led::select(led_inputs(rig, true)).brightness);  // off at night
}

// Recovery after a night cap continues from the capped speed (attack or decay).
void test_TC07_recovery_after_night_cap_resumes_from_cap() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(40.0f, 100.0f, 1.0f, true);  // night speed 4
  for (int i = 0; i < 3; ++i) rig.fail(true);                     // capped to 1
  TEST_ASSERT_EQUAL(1, rig.core.commanded_speed());
  const TickResult up = rig.ok(40.0f, 100.0f, 1.0f, true);        // still smoky: attack 1 -> 4
  TEST_ASSERT_EQUAL(4, up.commanded_speed);
  Rig rig2;
  for (int i = 0; i < 3; ++i) rig2.ok(40.0f, 100.0f, 1.0f, true);
  for (int i = 0; i < 5; ++i) rig2.fail(true);                    // capped to 1; window flushed
  const TickResult clean = rig2.ok(2.0f, 100.0f, 1.0f, true);     // clean: decays from 1, not 4
  TEST_ASSERT_EQUAL(1, clean.commanded_speed);
  TEST_ASSERT_EQUAL(1, rig2.core.algo().current_speed());
}

// A failed first poll after (re)boot must never command the purifier: nothing
// has been commanded, so there is nothing to hold or cap.
void test_boot_then_failed_polls_never_command() {
  Rig rig;
  for (int i = 1; i <= 20; ++i) {
    const TickResult r = rig.fail(i % 2 == 0);
    TEST_ASSERT_FALSE(r.speed_changed);
    TEST_ASSERT_FALSE(r.force_transmit);
  }
  TEST_ASSERT_TRUE(rig.core.fault().alarm());  // but the alarm is raised
  Rig rig2;
  const TickResult first = rig2.ok(2.0f);  // first valid reading: always transmitted
  TEST_ASSERT_TRUE(first.force_transmit);
  TEST_ASSERT_EQUAL(0, first.commanded_speed);
}

// ======================================================= v3.0 algorithm
void test_algo_pm_threshold_equality_edges() {
  DualPathController a;
  a.update(reading(5.0f), false, true);
  TEST_ASSERT_EQUAL(0, a.pm_speed());  // up requires > 5.0
  DualPathController b;
  b.update(reading(5.1f), false, true);
  TEST_ASSERT_EQUAL(1, b.pm_speed());
  for (int i = 0; i < 5; ++i) b.update(reading(3.0f), false, true);  // down requires <= 3.0
  TEST_ASSERT_EQUAL(0, b.pm_speed());
}

void test_algo_fast_attack_and_slow_decay() {
  DualPathController a;
  a.update(reading(90.0f), false, true);
  TEST_ASSERT_EQUAL(6, a.current_speed());  // multi-gear jump in one tick
  // Air clears: window must flush (5 ticks) before target drops, then 1 gear / 4 ticks
  int changes = 0, last = 6;
  for (int i = 0; i < 40; ++i) {
    const int s = a.update(reading(1.0f), false, true);
    if (s != last) {
      TEST_ASSERT_EQUAL(last - 1, s);  // never more than one gear at a time
      ++changes;
      last = s;
    }
  }
  TEST_ASSERT_EQUAL(0, a.current_speed());
  TEST_ASSERT_EQUAL(6, changes);
}

void test_algo_scenario_A_particleless_voc_day() {
  DualPathController a;
  const float tvoc[] = {88, 150, 250, 350, 451, 451, 451};
  int max_speed = 0;
  for (float t : tvoc) {
    const int s = a.update(reading(3.0f, t, 1.0f), false, true);
    if (s > max_speed) max_speed = s;
  }
  TEST_ASSERT_EQUAL(3, max_speed);      // gas floor capped at 3 in the day
  TEST_ASSERT_EQUAL(0, a.pm_speed());
  TEST_ASSERT_EQUAL(Trigger::GAS, a.dominant_trigger());
}

void test_algo_night_gas_cap_and_multigas_disable() {
  DualPathController a;
  for (int i = 0; i < 6; ++i) a.update(reading(2.0f, 480.0f, 10.0f), true, true);
  TEST_ASSERT_EQUAL(1, a.current_speed());
  DualPathController b;
  for (int i = 0; i < 6; ++i) b.update(reading(2.0f, 480.0f, 10.0f), false, false);
  TEST_ASSERT_EQUAL(0, b.current_speed());
}

void test_algo_scenario_B_gas_tail_holds_speed() {
  DualPathController a;
  for (int i = 0; i < 5; ++i) a.update(reading(40.0f, 300.0f), false, true);  // cooking
  TEST_ASSERT_EQUAL(4, a.current_speed());
  for (int i = 0; i < 40; ++i) a.update(reading(4.0f, 320.0f), false, true);  // PM clears, VOC lingers
  TEST_ASSERT_EQUAL(3, a.current_speed());  // gas floor holds Speed 3
}

// ================================================= modes / priorities
void test_manual_override_takes_precedence_over_watchdog() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);
  rig.core.modes().set_manual(5, rig.now);
  rig.core.on_mode_change(rig.now, true);
  TickResult r{};
  for (int i = 0; i < 6; ++i) r = rig.fail(true);
  TEST_ASSERT_TRUE(rig.core.fault().alarm());
  TEST_ASSERT_EQUAL(5, r.commanded_speed);  // user override wins at night; alarm still raised
  // Back to AUTO mid-outage at night: the night cap applies immediately.
  rig.core.modes().set_auto(rig.now);
  r = rig.core.on_mode_change(rig.now, true);
  TEST_ASSERT_TRUE(r.speed_changed);
  TEST_ASSERT_EQUAL(1, r.commanded_speed);
}

void test_button_cycle_and_manual_timeout() {
  ModeController m;
  m.begin(OperatingMode::AUTO, 0, 10, 0);
  const uint8_t expect[] = {1, 2, 3, 0};
  for (uint8_t e : expect) {
    m.button_cycle(0);
    TEST_ASSERT_EQUAL(OperatingMode::MANUAL, m.mode());
    TEST_ASSERT_EQUAL(e, m.manual_speed());
  }
  m.button_cycle(0);
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, m.mode());
  m.set_manual(5, 1000);
  TEST_ASSERT_FALSE(m.poll_timeout(1000 + 9 * 60000));
  TEST_ASSERT_TRUE(m.poll_timeout(1000 + 10 * 60000));
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, m.mode());
  TEST_ASSERT_TRUE(m.set_night_force(true, 0));
  TEST_ASSERT_TRUE(m.set_night_force(false, 0));
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, m.mode());
}

// ======================================================== config / parser
void test_config_patch_validation() {
  AppConfig c = defaults();
  char err[96];
  uint32_t mask;
  const char* bad_key = "{\"foo\": 1}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&c, bad_key, strlen(bad_key), &mask, err, sizeof(err)));
  const char* bad_word = "{\"ir_codes\": {\"speed_2\": \"0x1234\"}}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&c, bad_word, strlen(bad_word), &mask, err, sizeof(err)));
  const char* bad_vol = "{\"room_volume_m3\": 10}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&c, bad_vol, strlen(bad_vol), &mask, err, sizeof(err)));
  TEST_ASSERT_EQUAL_FLOAT(150.0f, c.room_volume_m3);  // untouched on failure
  const char* good = "{\"operating_mode\":\"manual\",\"manual_speed\":4,\"ir_nec_address\":\"0xFF00\"}";
  TEST_ASSERT_TRUE(config_apply_json_patch(&c, good, strlen(good), &mask, err, sizeof(err)));
  TEST_ASSERT_EQUAL(OperatingMode::MANUAL, c.operating_mode);
  TEST_ASSERT_EQUAL(4, c.manual_speed);
  char json[1024];
  TEST_ASSERT_TRUE(config_to_json(c, json, sizeof(json)) > 0);
  AppConfig d = defaults();
  TEST_ASSERT_TRUE(config_apply_json_patch(&d, json, strlen(json), &mask, err, sizeof(err)));
  TEST_ASSERT_EQUAL(0, memcmp(&c, &d, sizeof(c)));  // full round trip
}

void test_ag_parse_and_stale() {
  ag::Measurement m;
  const char* full =
      "{\"serialno\":\"9888e0eefcec\",\"pm02\":30,\"pm02Compensated\":21.5,\"tvocIndex\":215,"
      "\"noxIndex\":2,\"rco2\":640,\"atmp\":25.4,\"rhum\":63.8,\"boot\":42}";
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(full, strlen(full), "9888e0eefcec", &m));
  TEST_ASSERT_EQUAL_FLOAT(21.5f, m.pm25);
  TEST_ASSERT_EQUAL_FLOAT(215.0f, m.tvoc_index);
  TEST_ASSERT_TRUE(m.co2_valid);
  const char* raw_only = "{\"pm02\":12}";
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(raw_only, strlen(raw_only), "", &m));
  TEST_ASSERT_EQUAL_FLOAT(12.0f, m.pm25);
  const char* null_pm = "{\"pm02\":null,\"tvocIndex\":100}";
  TEST_ASSERT_EQUAL(ag::ParseResult::NO_PM25, ag::parse_measures(null_pm, strlen(null_pm), "", &m));
  const char* other = "{\"serialno\":\"aaaa\",\"pm02\":3}";
  TEST_ASSERT_EQUAL(ag::ParseResult::SERIAL_MISMATCH, ag::parse_measures(other, strlen(other), "9888e0eefcec", &m));
  TEST_ASSERT_EQUAL(ag::ParseResult::BAD_JSON, ag::parse_measures("{", 1, "", &m));
  ag::StaleDetector sd;
  ag::Measurement b;
  b.boot_valid = true;
  b.boot = 7;
  for (int i = 0; i < 3; ++i) TEST_ASSERT_FALSE(sd.update(b));
  TEST_ASSERT_TRUE(sd.update(b));  // 3 unchanged after the first -> stale
  b.boot = 8;
  TEST_ASSERT_FALSE(sd.update(b));
}

void test_led_night_silence_and_priorities() {
  led::Inputs in;
  in.current_speed = 5;
  in.is_night = true;
  TEST_ASSERT_EQUAL(0, led::select(in).brightness);
  in.night_led_enabled = true;
  TEST_ASSERT_EQUAL(10, led::select(in).brightness);
  in.factory_reset_warning = true;
  TEST_ASSERT_EQUAL(0, led::select(in).priority);
  led::Inputs uncomm;
  uncomm.matter_commissioned = false;
  TEST_ASSERT_EQUAL_HEX32(0xFF00FF, led::select(uncomm).rgb);
  TEST_ASSERT_EQUAL(255, led::pattern_level(led::Pattern::DOUBLE_FLASH, 0));
  TEST_ASSERT_EQUAL(0, led::pattern_level(led::Pattern::DOUBLE_FLASH, 200));
  TEST_ASSERT_EQUAL(255, led::pattern_level(led::Pattern::DOUBLE_FLASH, 320));
  TEST_ASSERT_EQUAL(0, led::pattern_level(led::Pattern::DOUBLE_FLASH, 500));
}

// =================================================================== TC-08
// REST API pairing + bearer auth (SRS §5.3).
static const char kTokA[] = "0123456789abcdef0123456789abcdef";
static const char kTokB[] = "fedcba9876543210fedcba9876543210";
static const uint32_t kPeerA = 0x0A01A8C0u;  // 192.168.1.10 (lwIP byte order)
static const uint32_t kPeerB = 0x0B01A8C0u;  // 192.168.1.11

void test_TC08_unpaired_device_rejects_everything() {
  auth::ApiAuth a;
  a.set_active("");  // never paired
  TEST_ASSERT_FALSE(a.has_token());
  TEST_ASSERT_EQUAL(auth::Check::MISSING, a.check(nullptr, kPeerA, 0));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer ", kPeerA, 0));
  char out[auth::kTokenChars + 1];
  TEST_ASSERT_EQUAL(auth::Pair::CLOSED, a.try_pair(0, out));  // no button, no pairing
}

void test_TC08_pairing_window_issues_token_once_and_revokes_old() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 1000));
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("bearer   0123456789abcdef0123456789abcdef", kPeerA, 1000));
  a.open_pairing(kTokB, 2000);
  TEST_ASSERT_TRUE(a.pairing_open(2000 + auth::kPairWindowMs - 1));
  char out[auth::kTokenChars + 1];
  TEST_ASSERT_EQUAL(auth::Pair::OK, a.try_pair(3000, out));
  TEST_ASSERT_EQUAL_STRING(kTokB, out);
  TEST_ASSERT_FALSE(a.pairing_open(3000));                         // one client per window
  TEST_ASSERT_EQUAL(auth::Pair::CLOSED, a.try_pair(3001, out));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 4000));  // revoked
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer fedcba9876543210fedcba9876543210", kPeerA, 4000));
}

void test_TC08_pairing_window_expires() {
  auth::ApiAuth a;
  a.open_pairing(kTokB, 10000);
  char out[auth::kTokenChars + 1];
  TEST_ASSERT_EQUAL(auth::Pair::CLOSED, a.try_pair(10000 + auth::kPairWindowMs, out));
  TEST_ASSERT_FALSE(a.has_token());
}

void test_TC08_bad_tokens_lock_out_then_recover() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  for (int i = 0; i < auth::kMaxFailures; ++i) {
    TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer 00000000000000000000000000000000", kPeerA, 100 + i));
  }
  // even the right token is refused while locked
  TEST_ASSERT_EQUAL(auth::Check::LOCKED, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 200));
  TEST_ASSERT_TRUE(a.lockout_remaining_ms(kPeerA, 200) > 0);
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 104 + auth::kLockoutMs));
  // malformed headers
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Basic 0123456789abcdef0123456789abcdef", kPeerA, 200000));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer0123456789abcdef0123456789abcdef", kPeerA, 200000));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer 0123456789abcdef0123456789abcdef0", kPeerA, 200000));
}

void test_TC08_token_format_and_clear() {
  const uint8_t bytes[auth::kTokenBytes] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  char t[auth::kTokenChars + 1];
  auth::token_from_bytes(bytes, t);
  TEST_ASSERT_EQUAL_STRING(kTokA, t);
  TEST_ASSERT_TRUE(auth::token_is_well_formed(t));
  TEST_ASSERT_FALSE(auth::token_is_well_formed("0123456789ABCDEF0123456789abcdef"));
  auth::ApiAuth a;
  a.set_active("short");
  TEST_ASSERT_FALSE(a.has_token());
  a.set_active(kTokA);
  a.clear();  // factory reset
  TEST_ASSERT_FALSE(a.has_token());
}

void test_TC08_pairing_led_patterns() {
  led::Inputs in;
  in.sensor_alarm = true;     // pairing feedback outranks the alarm...
  in.pair_hold_ready = true;
  led::Output o = led::select(in);
  TEST_ASSERT_EQUAL_HEX32(0x00E5FF, o.rgb);
  TEST_ASSERT_EQUAL(led::Pattern::SOLID, o.pattern);
  in.pair_hold_ready = false;
  in.pairing_active = true;
  in.is_night = true;         // ...and shows at night
  o = led::select(in);
  TEST_ASSERT_EQUAL(led::Pattern::HEARTBEAT_1HZ, o.pattern);
  TEST_ASSERT_EQUAL(40, o.brightness);
  in.factory_reset_warning = true;  // but never outranks the factory-reset warning
  TEST_ASSERT_EQUAL(led::Pattern::RAPID_10HZ, led::select(in).pattern);
  TEST_ASSERT_EQUAL(255, led::pattern_level(led::Pattern::HEARTBEAT_1HZ, 50));
  TEST_ASSERT_EQUAL(0, led::pattern_level(led::Pattern::HEARTBEAT_1HZ, 150));
  TEST_ASSERT_EQUAL(255, led::pattern_level(led::Pattern::HEARTBEAT_1HZ, 250));
  TEST_ASSERT_EQUAL(0, led::pattern_level(led::Pattern::HEARTBEAT_1HZ, 600));
  TEST_ASSERT_EQUAL(255, led::pattern_level(led::Pattern::HEARTBEAT_1HZ, 1050));
}

// =================================================================== TC-09
// Frozen readings: warning after 3 unchanged, failed polls from the 4th on.
void test_TC09_frozen_readings_escalate_to_failed_polls() {
  ag::StaleDetector sd;
  ag::Measurement m;
  m.boot_valid = true;
  m.boot = 100;
  Rig rig;
  rig.ok(40.0f, 100.0f, 1.0f, true);  // night: speed 4
  TEST_ASSERT_FALSE(sd.update(m));    // baseline
  TEST_ASSERT_EQUAL(4, rig.core.commanded_speed());
  // unchanged readings 1..3: data still used (3rd raises the stale warning)
  for (int i = 1; i <= 3; ++i) {
    sd.update(m);
    TEST_ASSERT_FALSE(sd.frozen());
    const TickResult r = rig.core.on_poll(!sd.frozen(), reading(40.0f), true, rig.now += 60000);
    TEST_ASSERT_EQUAL(4, r.commanded_speed);
  }
  TEST_ASSERT_TRUE(sd.stale());
  // unchanged readings 4..6: failed polls -> night cap on the 3rd of them
  for (int i = 1; i <= 3; ++i) {
    sd.update(m);
    TEST_ASSERT_TRUE(sd.frozen());
    const TickResult r = rig.core.on_poll(!sd.frozen(), reading(40.0f), true, rig.now += 60000);
    TEST_ASSERT_EQUAL(i, rig.core.fault().fail_count());
    TEST_ASSERT_EQUAL(i < 3 ? 4 : 1, r.commanded_speed);
  }
  // counter moves again: valid, recovery
  m.boot = 101;
  TEST_ASSERT_FALSE(sd.update(m));
  TEST_ASSERT_FALSE(sd.frozen());
  const TickResult rec = rig.core.on_poll(!sd.frozen(), reading(40.0f), true, rig.now += 60000);
  TEST_ASSERT_EQUAL(SensorFaultMonitor::Event::NONE, rec.fault_event);  // < 5 failures: no alarm to clear
  TEST_ASSERT_TRUE(rec.force_transmit);       // recovery always re-sends (SRS §3.1)
  TEST_ASSERT_EQUAL(0, rig.core.fault().fail_count());
  TEST_ASSERT_EQUAL(4, rec.commanded_speed);  // attack from the cap
}

// ===================================================== review regressions
// Blocking finding: an expired, unused pairing window must not reopen when millis() wraps.
void test_review_pairing_window_never_reopens_after_millis_wrap() {
  auth::ApiAuth a;
  const uint32_t start = 0xFFFF0000u;
  a.open_pairing(kTokB, start);
  TEST_ASSERT_TRUE(a.pairing_open(start + 1000));
  TEST_ASSERT_FALSE(a.pairing_open(start + auth::kPairWindowMs));  // expires (and is cleared)
  char out[auth::kTokenChars + 1];
  const uint32_t wrapped = start + 10;  // ~49.7 days later the counter shows this value again
  TEST_ASSERT_FALSE(a.pairing_open(wrapped));
  TEST_ASSERT_EQUAL(auth::Pair::CLOSED, a.try_pair(wrapped, out));
  TEST_ASSERT_FALSE(a.has_token());
}

void test_review_lockout_never_reappears_after_millis_wrap() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  for (int i = 0; i < auth::kMaxFailures; ++i) a.check("Bearer 00000000000000000000000000000000", kPeerA, 1000);
  TEST_ASSERT_EQUAL(auth::Check::LOCKED, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 2000));
  TEST_ASSERT_EQUAL(0u, a.lockout_remaining_ms(kPeerA, 1000 + auth::kLockoutMs));  // expired and cleared
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 1500));  // "wrapped" time
}

void test_review_header_whitespace_and_interleaved_failures() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef \t", kPeerA, 0));
  // successes don't reset the failure count: 5 bad tokens in 60 s lock out
  for (int i = 0; i < auth::kMaxFailures - 1; ++i) {
    TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer 00000000000000000000000000000000", kPeerA, 10 + i));
    TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 10 + i));
  }
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer 00000000000000000000000000000000", kPeerA, 20));
  TEST_ASSERT_EQUAL(auth::Check::LOCKED, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 21));
}

// SRS §3.1 Recovery: after ANY failed-poll streak the code is re-sent even if unchanged.
void test_review_recovery_resends_after_short_outage() {
  Rig rig;
  for (int i = 0; i < 3; ++i) rig.ok(22.0f);
  rig.fail();
  rig.fail();
  const TickResult r = rig.ok(22.0f);
  TEST_ASSERT_EQUAL(3, r.commanded_speed);
  TEST_ASSERT_FALSE(r.speed_changed);
  TEST_ASSERT_TRUE(r.force_transmit);
  TEST_ASSERT_FALSE(rig.ok(22.0f).force_transmit);  // only once
}

// SRS §2.2: with a configured serial, a reply without serialno is not the AirGradient.
void test_review_missing_serial_is_mismatch() {
  ag::Measurement m;
  const char* anon = "{\"pm02\":80}";
  TEST_ASSERT_EQUAL(ag::ParseResult::SERIAL_MISMATCH, ag::parse_measures(anon, strlen(anon), "9888e0eefcec", &m));
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(anon, strlen(anon), "", &m));  // no serial configured
  // unrelated big fields are filtered out instead of exhausting the arena
  char big[1800];
  int n = snprintf(big, sizeof(big), "{\"serialno\":\"9888e0eefcec\",\"pm02\":5,\"boot\":3,\"junk\":\"");
  while (n < static_cast<int>(sizeof(big)) - 4) big[n++] = 'x';
  big[n++] = '"';
  big[n++] = '}';
  big[n] = '\0';
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(big, strlen(big), "9888e0eefcec", &m));
  TEST_ASSERT_TRUE(m.boot_valid);
}

// User decision 2026-11-10: lockouts are per client address.
void test_TC08_lockout_is_per_client() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  for (int i = 0; i < auth::kMaxFailures; ++i) a.check("Bearer 00000000000000000000000000000000", kPeerB, 100);
  TEST_ASSERT_EQUAL(auth::Check::LOCKED, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerB, 200));
  TEST_ASSERT_EQUAL(auth::Check::OK, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerA, 200));  // owner unaffected
  TEST_ASSERT_EQUAL(0u, a.lockout_remaining_ms(kPeerA, 200));
  TEST_ASSERT_TRUE(a.lockout_remaining_ms(kPeerB, 200) > 0);
  // many attacking addresses can't push an existing lockout out while unlocked slots exist
  for (uint32_t ip = 1; ip <= auth::kMaxTrackedPeers + 3; ++ip) a.check("Bearer bad", 0xC0000000u + ip, 300);
  TEST_ASSERT_EQUAL(auth::Check::LOCKED, a.check("Bearer 0123456789abcdef0123456789abcdef", kPeerB, 400));
  // IPv4-mapped IPv6 maps to the same key as plain IPv4
  const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 1, 11};
  TEST_ASSERT_EQUAL_HEX32(kPeerB, auth::peer_key_ipv6(mapped));
}

// User decision 2026-11-10: /pair only for requests addressed to the device itself.
void test_TC08_pairing_host_and_origin_guard() {
  const uint32_t ip = 0x2A01A8C0u;  // 192.168.1.42
  TEST_ASSERT_TRUE(auth::pairing_request_allowed("atoms3-iaq.local", nullptr, "atoms3-iaq", ip));
  TEST_ASSERT_TRUE(auth::pairing_request_allowed("ATOMS3-IAQ.local.:80", "", "atoms3-iaq", ip));
  TEST_ASSERT_TRUE(auth::pairing_request_allowed("192.168.1.42", nullptr, "atoms3-iaq", ip));
  TEST_ASSERT_TRUE(auth::pairing_request_allowed("192.168.1.42:80", "http://192.168.1.42", "atoms3-iaq", ip));
  TEST_ASSERT_FALSE(auth::pairing_request_allowed(nullptr, nullptr, "atoms3-iaq", ip));          // no Host
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("evil.example", nullptr, "atoms3-iaq", ip));   // DNS rebinding
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("192.168.1.43", nullptr, "atoms3-iaq", ip));
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("192.168.1.42:8080", nullptr, "atoms3-iaq", ip));
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("atoms3-iaq.local", "http://evil.example", "atoms3-iaq", ip));  // CSRF
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("atoms3-iaq.local", "null", "atoms3-iaq", ip));
  TEST_ASSERT_FALSE(auth::pairing_request_allowed("192.168.1.42", nullptr, "atoms3-iaq", 0));    // IP not known yet
}

// ========================================================= Stage 1B tests
void test_config_ir_nec_address_validation() {
  TEST_ASSERT_TRUE(nec_word_is_valid(0xFF00));
  TEST_ASSERT_TRUE(nec_word_is_valid(0x00FF));
  TEST_ASSERT_FALSE(nec_word_is_valid(0x1234));
  TEST_ASSERT_FALSE(ir_address_looks_swapped(0xFF00));
  TEST_ASSERT_TRUE(ir_address_looks_swapped(0x00FF));

  AppConfig c = defaults();
  char err[96] = "";
  TEST_ASSERT_TRUE(config_validate(c, err, sizeof(err)));

  c.ir_nec_address = 0x00FF;  // structurally valid NEC word (address 0xFF)
  TEST_ASSERT_TRUE(config_validate(c, err, sizeof(err)));

  c.ir_nec_address = 0x1234;
  TEST_ASSERT_FALSE(config_validate(c, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "ir_nec_address"));

  AppConfig d = defaults();
  uint32_t mask = 0;
  const char* bad_addr = "{\"ir_nec_address\":\"0x1234\"}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&d, bad_addr, strlen(bad_addr), &mask, err, sizeof(err)));
  TEST_ASSERT_EQUAL_HEX16(0xFF00, d.ir_nec_address);

  const char* swapped_addr = "{\"ir_nec_address\":\"0x00FF\"}";
  TEST_ASSERT_TRUE(config_apply_json_patch(&d, swapped_addr, strlen(swapped_addr), &mask, err, sizeof(err)));
  TEST_ASSERT_EQUAL_HEX16(0x00FF, d.ir_nec_address);
  TEST_ASSERT_TRUE(mask & CFG_CHANGED_IR);
}

void test_matter_mappers_breakpoints_and_edges() {
  // PM2.5 -> AirQualityEnum (0=Unknown, 1=Good..6=ExtremelyPoor)
  TEST_ASSERT_EQUAL_UINT8(0, matter_air_quality_from_pm25(NAN));
  TEST_ASSERT_EQUAL_UINT8(0, matter_air_quality_from_pm25(INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_air_quality_from_pm25(-INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_air_quality_from_pm25(-0.01f));
  TEST_ASSERT_EQUAL_UINT8(0, matter_air_quality_from_pm25(-100.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_air_quality_from_pm25(0.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_air_quality_from_pm25(9.0f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_air_quality_from_pm25(9.01f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_air_quality_from_pm25(35.4f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_air_quality_from_pm25(35.41f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_air_quality_from_pm25(55.4f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_air_quality_from_pm25(55.41f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_air_quality_from_pm25(125.4f));
  TEST_ASSERT_EQUAL_UINT8(5, matter_air_quality_from_pm25(125.41f));
  TEST_ASSERT_EQUAL_UINT8(5, matter_air_quality_from_pm25(225.4f));
  TEST_ASSERT_EQUAL_UINT8(6, matter_air_quality_from_pm25(225.41f));
  TEST_ASSERT_EQUAL_UINT8(6, matter_air_quality_from_pm25(500.0f));

  // TVOC index -> LevelValueEnum (0=Unknown, 1=Low..4=Critical)
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_tvoc_index(NAN));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_tvoc_index(INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_tvoc_index(-INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_tvoc_index(-1.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_level_from_tvoc_index(0.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_level_from_tvoc_index(150.0f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_level_from_tvoc_index(150.1f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_level_from_tvoc_index(250.0f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_level_from_tvoc_index(250.1f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_level_from_tvoc_index(400.0f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_level_from_tvoc_index(400.1f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_level_from_tvoc_index(500.0f));

  // NOx index -> LevelValueEnum (0=Unknown, 1=Low..4=Critical)
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_nox_index(NAN));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_nox_index(INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_nox_index(-INFINITY));
  TEST_ASSERT_EQUAL_UINT8(0, matter_level_from_nox_index(-0.5f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_level_from_nox_index(0.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_level_from_nox_index(1.0f));
  TEST_ASSERT_EQUAL_UINT8(1, matter_level_from_nox_index(3.99f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_level_from_nox_index(4.0f));
  TEST_ASSERT_EQUAL_UINT8(2, matter_level_from_nox_index(5.99f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_level_from_nox_index(6.0f));
  TEST_ASSERT_EQUAL_UINT8(3, matter_level_from_nox_index(19.99f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_level_from_nox_index(20.0f));
  TEST_ASSERT_EQUAL_UINT8(4, matter_level_from_nox_index(500.0f));
}

void test_ota_validation_helpers() {
  constexpr size_t kSlotSize = 0x380000;  // 3.5 MB from partitions_8MB_matter.csv
  TEST_ASSERT_FALSE(ota_size_is_valid(0, kSlotSize));
  TEST_ASSERT_FALSE(ota_size_is_valid(kOtaMinImageBytes, 0));
  TEST_ASSERT_FALSE(ota_size_is_valid(kOtaMinImageBytes - 1, kSlotSize));
  TEST_ASSERT_TRUE(ota_size_is_valid(kOtaMinImageBytes, kSlotSize));
  TEST_ASSERT_TRUE(ota_size_is_valid(1950000, kSlotSize));
  TEST_ASSERT_TRUE(ota_size_is_valid(kSlotSize, kSlotSize));
  TEST_ASSERT_FALSE(ota_size_is_valid(kSlotSize + 1, kSlotSize));

  uint8_t good_hdr[kOtaHeaderBytes] = {kOtaImageMagic, 0x05, 0x02, 0x20};
  good_hdr[12] = static_cast<uint8_t>(kOtaChipIdEsp32S3 & 0xFF);
  good_hdr[13] = static_cast<uint8_t>(kOtaChipIdEsp32S3 >> 8);
  uint8_t wrong_chip[kOtaHeaderBytes] = {kOtaImageMagic, 0x05, 0x02, 0x20};  // chip_id 0x0000 (ESP32)
  const uint8_t bad_hdr[] = {0x7F, 'E', 'L', 'F'};
  TEST_ASSERT_FALSE(ota_header_is_valid(nullptr, sizeof(good_hdr)));
  TEST_ASSERT_FALSE(ota_header_is_valid(good_hdr, 0));
  TEST_ASSERT_FALSE(ota_header_is_valid(good_hdr, kOtaHeaderBytes - 1));
  TEST_ASSERT_FALSE(ota_header_is_valid(bad_hdr, sizeof(bad_hdr)));
  TEST_ASSERT_FALSE(ota_header_is_valid(wrong_chip, sizeof(wrong_chip)));
  TEST_ASSERT_TRUE(ota_header_is_valid(good_hdr, sizeof(good_hdr)));
}

void test_sim_inject_parser() {
  sim::InjectCmd cmd;
  TEST_ASSERT_TRUE(sim::parse_inject_line("inject pm25=42.0", &cmd));
  TEST_ASSERT_FALSE(cmd.fail);
  TEST_ASSERT_EQUAL_FLOAT(42.0f, cmd.pm25);
  TEST_ASSERT_FALSE(cmd.tvoc_valid);
  TEST_ASSERT_FALSE(cmd.nox_valid);
  TEST_ASSERT_FALSE(cmd.co2_valid);
  TEST_ASSERT_FALSE(cmd.has_night);

  TEST_ASSERT_TRUE(sim::parse_inject_line("  inject   pm25=0  tvoc=215.5  nox=4.2  co2=800  night=1 \r\n", &cmd));
  TEST_ASSERT_FALSE(cmd.fail);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, cmd.pm25);
  TEST_ASSERT_TRUE(cmd.tvoc_valid);
  TEST_ASSERT_EQUAL_FLOAT(215.5f, cmd.tvoc);
  TEST_ASSERT_TRUE(cmd.nox_valid);
  TEST_ASSERT_EQUAL_FLOAT(4.2f, cmd.nox);
  TEST_ASSERT_TRUE(cmd.co2_valid);
  TEST_ASSERT_EQUAL_FLOAT(800.0f, cmd.co2);
  TEST_ASSERT_TRUE(cmd.has_night);
  TEST_ASSERT_TRUE(cmd.night);

  TEST_ASSERT_TRUE(sim::parse_inject_line("inject fail", &cmd));
  TEST_ASSERT_TRUE(cmd.fail);
  TEST_ASSERT_FALSE(cmd.has_night);

  TEST_ASSERT_TRUE(sim::parse_inject_line("inject fail night=1", &cmd));
  TEST_ASSERT_TRUE(cmd.fail);
  TEST_ASSERT_TRUE(cmd.has_night);
  TEST_ASSERT_TRUE(cmd.night);

  TEST_ASSERT_TRUE(sim::parse_inject_line("inject pm25=1000 tvoc=500 nox=500 co2=40000 night=0", &cmd));
  TEST_ASSERT_FALSE(cmd.fail);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, cmd.pm25);
  TEST_ASSERT_EQUAL_FLOAT(500.0f, cmd.tvoc);
  TEST_ASSERT_EQUAL_FLOAT(500.0f, cmd.nox);
  TEST_ASSERT_EQUAL_FLOAT(40000.0f, cmd.co2);
  TEST_ASSERT_TRUE(cmd.has_night);
  TEST_ASSERT_FALSE(cmd.night);

  // Oversized token (>63 chars) must be rejected cleanly
  TEST_ASSERT_FALSE(sim::parse_inject_line(
      "inject pm25=10.00000000000000000000000000000000000000000000000000000000000000", &cmd));

  // Reject malformed / out-of-range / conflicting commands
  TEST_ASSERT_FALSE(sim::parse_inject_line(nullptr, &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("   ", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("status", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject tvoc=100", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=-1", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=1000.1", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=nan", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=inf", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=abc", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 tvoc=501", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 nox=-0.1", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 co2=40001", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 night=2", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 pm25=20", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject fail pm25=10", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject tvoc=100 fail", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=10 unknown=1", &cmd));
  TEST_ASSERT_FALSE(sim::parse_inject_line("inject pm25=", &cmd));
}

// =================================================== expanded audit tests
void test_clock_parse_edges_and_night_schedule_wrap() {
  time_t t = 0;
  // Single-digit day + UTC suffix
  TEST_ASSERT_TRUE(clk::parse_http_date("Sun, 9 Nov 2026 06:30:00 UTC", &t));
  TEST_ASSERT_EQUAL_INT64(1794205800LL, static_cast<long long>(t));
  // Negative time components must be rejected
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 -1:30:00 GMT", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 06:-1:00 GMT", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 06:30:-1 GMT", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 24:00:00 GMT", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 06:60:00 GMT", &t));
  TEST_ASSERT_TRUE(clk::parse_http_date("Mon, 09 Nov 2026 06:30:60 GMT", &t));
  TEST_ASSERT_FALSE(clk::parse_http_date("Mon, 09 Nov 2026 06:30:61 GMT", &t));

  // night_start < day_start (e.g. 01:00..07:59)
  TEST_ASSERT_FALSE(is_night_hour(0, 8, 1));
  TEST_ASSERT_TRUE(is_night_hour(1, 8, 1));
  TEST_ASSERT_TRUE(is_night_hour(7, 8, 1));
  TEST_ASSERT_FALSE(is_night_hour(8, 8, 1));
  TEST_ASSERT_FALSE(is_night_hour(23, 8, 1));
  TEST_ASSERT_FALSE(is_night_hour(-1, 8, 1));
  TEST_ASSERT_FALSE(is_night_hour(24, 8, 1));
}

void test_multigas_exact_thresholds_and_reset_blocking() {
  // Day Tier 1 & Tier 2 exact TVOC / NOx boundaries
  {
    DualPathController a;
    a.update(reading(2.0f, 200.0f, 3.9f), false, true);
    TEST_ASSERT_EQUAL(0, a.gas_speed());
    a.update(reading(2.0f, 200.2f, 1.0f), false, true);  // SMA > 200.0 -> Tier 1 (2)
    TEST_ASSERT_EQUAL(2, a.gas_speed());
  }
  {
    DualPathController a;  // NOx-only Tier 1 (>= 4.0) -> Tier 2 (>= 6.0)
    a.update(reading(2.0f, 80.0f, 4.0f), false, true);
    TEST_ASSERT_EQUAL(2, a.gas_speed());
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 80.0f, 6.0f), false, true);
    TEST_ASSERT_EQUAL(3, a.gas_speed());
    // AND-reset partial blocking at Tier 2: TVOC low, NOx = 4.1 (> 4.0) stays at 3
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 50.0f, 4.1f), false, true);
    TEST_ASSERT_EQUAL(3, a.gas_speed());
    // Both below Tier 2 reset (TVOC <= 230 && NOx <= 4.0) -> drops to Tier 1 (2)
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 230.0f, 4.0f), false, true);
    TEST_ASSERT_EQUAL(2, a.gas_speed());
    // AND-reset partial blocking at Tier 1: TVOC = 151 (> 150) & NOx = 1.0 -> stays at 2
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 151.0f, 1.0f), false, true);
    TEST_ASSERT_EQUAL(2, a.gas_speed());
    // Both <= 150 / <= 2.5 -> resets to 0
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 150.0f, 2.5f), false, true);
    TEST_ASSERT_EQUAL(0, a.gas_speed());
  }
  // Night exact thresholds, NOx-only trigger, AND-reset, and Day->Night clamp
  {
    DualPathController a;
    a.update(reading(2.0f, 300.0f, 4.9f), true, true);
    TEST_ASSERT_EQUAL(0, a.gas_speed());
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 100.0f, 5.0f), true, true);
    TEST_ASSERT_EQUAL(1, a.gas_speed());
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 100.0f, 3.1f), true, true);
    TEST_ASSERT_EQUAL(1, a.gas_speed());  // blocked by NOx > 3.0
    for (int i = 0; i < 5; ++i) a.update(reading(2.0f, 230.0f, 3.0f), true, true);
    TEST_ASSERT_EQUAL(0, a.gas_speed());
  }
  {
    DualPathController a;
    a.update(reading(2.0f, 350.0f, 1.0f), false, true);  // Day gas speed 3
    TEST_ASSERT_EQUAL(3, a.gas_speed());
    TEST_ASSERT_EQUAL(3, a.current_speed());
    a.update(reading(2.0f, 350.0f, 1.0f), true, true);   // Switch to Night -> gas clamps to 1
    TEST_ASSERT_EQUAL(1, a.gas_speed());
    TEST_ASSERT_EQUAL(3, a.current_speed());             // smooth decay 3 -> 2 -> 1
  }
}

void test_volume_250_scaling_and_manual_to_auto_dwell() {
  Rig rig;
  rig.cfg.room_volume_m3 = 250.0f;
  rig.core.begin(rig.cfg, 0);
  TEST_ASSERT_EQUAL(7, rig.core.algo().scaling().min_stepdown_hold);
  // At 250 m3, U_3 = 35 * 0.7746 = 27.11 -> pm=28.0 hits Speed 4 (vs Speed 3 at 150 m3)
  TickResult r{};
  for (int i = 0; i < 5; ++i) r = rig.ok(28.0f);
  TEST_ASSERT_EQUAL(4, r.commanded_speed);

  // Switch to MANUAL at 4, then back to AUTO with clean air:
  // 1st clean tick avg=22.6 (> L_3=20.14), then 7 ticks below L_3 -> step down to 3
  rig.core.modes().set_manual(4, rig.now);
  rig.core.on_mode_change(rig.now, false);
  rig.core.modes().set_auto(rig.now);
  rig.core.on_mode_change(rig.now, false);
  TEST_ASSERT_EQUAL(4, rig.ok(1.0f).commanded_speed);  // avg=22.6 > 20.14 -> dwell=0
  TEST_ASSERT_EQUAL(0, rig.core.algo().dwell_minutes());
  for (int i = 1; i <= 6; ++i) {
    TEST_ASSERT_EQUAL(4, rig.ok(1.0f).commanded_speed);
    TEST_ASSERT_EQUAL(i, rig.core.algo().dwell_minutes());
  }
  TEST_ASSERT_EQUAL(3, rig.ok(1.0f).commanded_speed);  // 7th tick -> 4 -> 3
  TEST_ASSERT_EQUAL(0, rig.core.algo().dwell_minutes());
}

void test_ag_parse_partial_dropouts_and_bounds() {
  ag::Measurement m;
  // pm02Compensated null -> fallback to pm02; partial env dropouts retained as valid=false
  const char* partial = "{\"serialno\":\"9888e0eefcec\",\"pm02Compensated\":null,\"pm02\":18.5,"
                        "\"tvocIndex\":null,\"noxIndex\":2.0,\"rco2\":null,\"atmp\":null,\"rhum\":null}";
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(partial, strlen(partial), "9888e0eefcec", &m));
  TEST_ASSERT_TRUE(m.pm25_valid);
  TEST_ASSERT_EQUAL_FLOAT(18.5f, m.pm25);
  TEST_ASSERT_FALSE(m.tvoc_valid);
  TEST_ASSERT_TRUE(m.nox_valid);
  TEST_ASSERT_FALSE(m.co2_valid);
  TEST_ASSERT_FALSE(m.temp_valid);
  TEST_ASSERT_FALSE(m.rh_valid);
  TEST_ASSERT_FALSE(m.boot_valid);

  // pm02 out of [0, 1000] rejected as NO_PM25
  const char* too_high = "{\"pm02\":1000.1}";
  const char* negative = "{\"pm02\":-0.1}";
  TEST_ASSERT_EQUAL(ag::ParseResult::NO_PM25, ag::parse_measures(too_high, strlen(too_high), "", &m));
  TEST_ASSERT_EQUAL(ag::ParseResult::NO_PM25, ag::parse_measures(negative, strlen(negative), "", &m));

  // Optional fields out of bounds are ignored without failing pm25
  const char* bad_opt = "{\"pm02\":10,\"tvocIndex\":501,\"noxIndex\":-1,\"rco2\":40001,\"atmp\":-41,\"rhum\":101}";
  TEST_ASSERT_EQUAL(ag::ParseResult::OK, ag::parse_measures(bad_opt, strlen(bad_opt), "", &m));
  TEST_ASSERT_TRUE(m.pm25_valid);
  TEST_ASSERT_FALSE(m.tvoc_valid);
  TEST_ASSERT_FALSE(m.nox_valid);
  TEST_ASSERT_FALSE(m.co2_valid);
  TEST_ASSERT_FALSE(m.temp_valid);
  TEST_ASSERT_FALSE(m.rh_valid);

  // StaleDetector without boot_valid stays non-stale, and frozen() triggers at 4th unchanged
  ag::StaleDetector sd;
  ag::Measurement no_boot;
  for (int i = 0; i < 6; ++i) TEST_ASSERT_FALSE(sd.update(no_boot));
  TEST_ASSERT_FALSE(sd.frozen());
  ag::Measurement with_boot;
  with_boot.boot_valid = true;
  with_boot.boot = 10;
  TEST_ASSERT_FALSE(sd.update(with_boot));  // baseline
  for (int i = 0; i < 2; ++i) TEST_ASSERT_FALSE(sd.update(with_boot));
  TEST_ASSERT_TRUE(sd.update(with_boot));   // 3rd unchanged -> stale, not yet frozen
  TEST_ASSERT_FALSE(sd.frozen());
  TEST_ASSERT_TRUE(sd.update(with_boot));   // 4th unchanged -> frozen
  TEST_ASSERT_TRUE(sd.frozen());
  with_boot.boot = 11;
  TEST_ASSERT_FALSE(sd.update(with_boot));
  TEST_ASSERT_FALSE(sd.frozen());
  TEST_ASSERT_EQUAL(0, sd.unchanged());
}

void test_api_auth_eviction_and_short_headers() {
  auth::ApiAuth a;
  a.set_active(kTokA);
  // Short (< 6 char) and malformed Authorization headers
  TEST_ASSERT_EQUAL(auth::Check::MISSING, a.check("", kPeerA, 0));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("B", kPeerA, 1));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Beare", kPeerA, 2));
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Basic", kPeerA, 3));

  // Fill all 8 slots: 7 locked peers (timestamps 100..700), 1 unlocked peer (ts 800)
  for (uint32_t i = 0; i < auth::kMaxTrackedPeers - 1; ++i) {
    const uint32_t peer = 0x0A000001u + i;
    for (int f = 0; f < auth::kMaxFailures; ++f) a.check("Bearer bad", peer, 100 + i * 100);
    TEST_ASSERT_TRUE(a.lockout_remaining_ms(peer, 1000) > 0);
  }
  const uint32_t unlocked_peer = 0x0A0000F0u;
  a.check("Bearer bad", unlocked_peer, 800);
  // 9th peer must evict the unlocked entry, preserving all 7 locked entries
  const uint32_t peer9 = 0x0A0000F1u;
  for (int f = 0; f < auth::kMaxFailures; ++f) a.check("Bearer bad", peer9, 900);
  for (uint32_t i = 0; i < auth::kMaxTrackedPeers - 1; ++i) {
    TEST_ASSERT_TRUE(a.lockout_remaining_ms(0x0A000001u + i, 1000) > 0);
  }
  // All 8 slots are now locked; 10th peer evicts the oldest lockout (100 ms -> peer 0x0A000001)
  const uint32_t peer10 = 0x0A0000F2u;
  TEST_ASSERT_EQUAL(auth::Check::INVALID, a.check("Bearer bad", peer10, 1000));
  TEST_ASSERT_EQUAL(0u, a.lockout_remaining_ms(0x0A000001u, 1000));
  TEST_ASSERT_TRUE(a.lockout_remaining_ms(0x0A000002u, 1000) > 0);
}

void test_control_core_persisted_manual_and_timeout_ordering() {
  // Booting in persisted MANUAL mode applies manual_speed immediately
  AppConfig c = defaults();
  c.operating_mode = OperatingMode::MANUAL;
  c.manual_speed = 4;
  ControlCore core;
  const TickResult boot_r = core.begin(c, 0);
  TEST_ASSERT_EQUAL(4, boot_r.commanded_speed);
  TEST_ASSERT_TRUE(boot_r.speed_changed);
  TEST_ASSERT_TRUE(boot_r.force_transmit);
  TEST_ASSERT_EQUAL(4, core.commanded_speed());
  TEST_ASSERT_EQUAL(0u, core.speed_changes_today());

  // Booting in NIGHT_FORCE without readings still commands nothing (SRS §3.1 rule 4)
  AppConfig nf = defaults();
  nf.operating_mode = OperatingMode::NIGHT_FORCE;
  ControlCore nf_core;
  const TickResult nf_boot = nf_core.begin(nf, 0);
  TEST_ASSERT_FALSE(nf_boot.speed_changed);
  TEST_ASSERT_FALSE(nf_boot.force_transmit);

  // Manual timeout expiring inside on_poll() simultaneously with a spike must NOT
  // overwrite the spike speed back down to the old manual speed.
  AppConfig tc = defaults();
  tc.operating_mode = OperatingMode::MANUAL;
  tc.manual_speed = 2;
  tc.manual_timeout_mins = 5;
  ControlCore tcore;
  tcore.begin(tc, 0);
  const TickResult spike = tcore.on_poll(true, reading(90.0f), false, 5u * 60000u);
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, tcore.modes().mode());
  TEST_ASSERT_EQUAL(6, spike.commanded_speed);
  TEST_ASSERT_EQUAL(6, tcore.algo().current_speed());
}

void test_led_compose_and_double_flash_restart() {
  led::Output o{4, 0xFFFFFF, 70, led::Pattern::DOUBLE_FLASH};
  uint8_t r = 0, g = 0, b = 0;
  led::compose(o, 0, &r, &g, &b);
  TEST_ASSERT_EQUAL_UINT8(178, r);  // (255 * 255 * 70) / 25500 = 178
  TEST_ASSERT_EQUAL_UINT8(178, g);
  TEST_ASSERT_EQUAL_UINT8(178, b);
  led::compose(o, 200, &r, &g, &b);
  TEST_ASSERT_EQUAL_UINT8(0, r);

  led::PatternState ps;
  led::Inputs in;
  in.ir_flash_active = true;
  in.ir_flash_seq = 1;
  TEST_ASSERT_TRUE(led::update_pattern_state(&ps, led::select(in), in.ir_flash_seq));
  TEST_ASSERT_FALSE(led::update_pattern_state(&ps, led::select(in), in.ir_flash_seq));
  // Back-to-back IR TX increments ir_flash_seq while DOUBLE_FLASH is still active -> restarts pattern
  in.ir_flash_seq = 2;
  TEST_ASSERT_TRUE(led::update_pattern_state(&ps, led::select(in), in.ir_flash_seq));
}

void test_config_edge_validation_and_mode_masks() {
  AppConfig c = defaults();
  char err[96] = "";
  uint32_t mask = 0;

  // ag_serial uppercase rejection
  const char* upper_sn = "{\"ag_serial\":\"9888E0EEFCEC\"}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&c, upper_sn, strlen(upper_sn), &mask, err, sizeof(err)));
  // day_start_hour == night_start_hour rejection
  const char* same_hr = "{\"day_start_hour\":12,\"night_start_hour\":12}";
  TEST_ASSERT_FALSE(config_apply_json_patch(&c, same_hr, strlen(same_hr), &mask, err, sizeof(err)));
  // parse_u16_hex rejects +, -, and internal spaces
  uint16_t u = 0;
  TEST_ASSERT_FALSE(parse_u16_hex("+12", &u));
  TEST_ASSERT_FALSE(parse_u16_hex("-1", &u));
  TEST_ASSERT_FALSE(parse_u16_hex("0x+1", &u));
  TEST_ASSERT_FALSE(parse_u16_hex("0x-0", &u));
  TEST_ASSERT_FALSE(parse_u16_hex("0x 1", &u));
  TEST_ASSERT_TRUE(parse_u16_hex(" 0xFF00", &u));
  TEST_ASSERT_EQUAL_HEX16(0xFF00, u);

  // Separate change masks for manual_speed and manual_timeout_mins
  const char* ms_patch = "{\"manual_speed\":4}";
  TEST_ASSERT_TRUE(config_apply_json_patch(&c, ms_patch, strlen(ms_patch), &mask, err, sizeof(err)));
  TEST_ASSERT_TRUE((mask & CFG_CHANGED_MANUAL_SPEED) != 0);
  TEST_ASSERT_FALSE((mask & CFG_CHANGED_MODE) != 0);
  const char* mt_patch = "{\"manual_timeout_mins\":15}";
  TEST_ASSERT_TRUE(config_apply_json_patch(&c, mt_patch, strlen(mt_patch), &mask, err, sizeof(err)));
  TEST_ASSERT_TRUE((mask & CFG_CHANGED_MANUAL_TIMEOUT) != 0);
  TEST_ASSERT_FALSE((mask & CFG_CHANGED_MODE) != 0);

  // ModeController::set_manual_speed updates preset without switching mode or timer
  ModeController mc;
  mc.begin(OperatingMode::AUTO, 0, 15, 1000);
  mc.set_manual_speed(4);
  TEST_ASSERT_EQUAL(OperatingMode::AUTO, mc.mode());
  TEST_ASSERT_EQUAL_UINT8(4, mc.manual_speed());
}

void test_ir_test_send_preserves_unknown_sent_speed() {
  // Diagnostic test_ir (track_state=false) must NOT mutate sent_speed_ when unknown (-1),
  // so the first real command >= 2 still emits the 600 ms Speed-1 wake prefix.
  int sent = -1;
  sent = ir::next_sent_speed(sent, 3, 0, false);
  sent = ir::next_sent_speed(sent, 3, 1, false);
  TEST_ASSERT_EQUAL(-1, sent);

  const AppConfig c = defaults();
  ir::Step st[ir::kMaxSteps];
  const int from = sent < 0 ? 0 : sent;
  TEST_ASSERT_EQUAL(2, ir::plan_transition(from, 4, true, c.wake_require_speed1_prefix, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xE817, st[0].command_word);
  TEST_ASSERT_EQUAL_HEX16(0xFA05, st[1].command_word);

  // Real tracked command updates sent_speed_ only after the final frame completes
  sent = ir::next_sent_speed(sent, 4, 0, true);
  TEST_ASSERT_EQUAL(-1, sent);
  sent = ir::next_sent_speed(sent, 4, 1, true);
  TEST_ASSERT_EQUAL(4, sent);
  // Subsequent diagnostic test_ir(0) leaves tracked sent_speed_ at 4
  sent = ir::next_sent_speed(sent, 0, 1, false);
  TEST_ASSERT_EQUAL(4, sent);

  // Transitioning to 0 after test_ir (assume_off=true, force=false) must STILL emit Off (0xFF00)
  TEST_ASSERT_EQUAL(1, ir::plan_executor_request(4, 0, false, true, c.wake_require_speed1_prefix, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xFF00, st[0].command_word);
  // Unknown sent_speed (-1) to speed 4 forces wake prefix even when force=false
  TEST_ASSERT_EQUAL(2, ir::plan_executor_request(-1, 4, false, false, c.wake_require_speed1_prefix, c.ir_codes, st));
  TEST_ASSERT_EQUAL_HEX16(0xE817, st[0].command_word);
  TEST_ASSERT_EQUAL_HEX16(0xFA05, st[1].command_word);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_TC01_matter_slider_sets_manual_and_auto_resumes);
  RUN_TEST(test_TC01_percent_speed_mapping_roundtrip);
  RUN_TEST(test_TC02_http_date_parse);
  RUN_TEST(test_TC02_unsynced_clock_falls_back_to_day_unless_night_force);
  RUN_TEST(test_TC03_wake_from_off_to_speed4_sequence);
  RUN_TEST(test_TC03_nec_symbols_and_other_transitions);
  RUN_TEST(test_TC04_room_volume_patch_rescales);
  RUN_TEST(test_TC05_day_dropout_holds_speed);
  RUN_TEST(test_TC05_night_dropout_caps_at_speed1_after_3_polls);
  RUN_TEST(test_TC06_five_failures_raise_alarm_without_shutoff);
  RUN_TEST(test_TC06_day_cap_at_speed3_after_15_polls);
  RUN_TEST(test_watchdog_caps_never_raise_and_follow_day_night);
  RUN_TEST(test_TC07_recovery_after_alarm);
  RUN_TEST(test_TC07_recovery_after_night_cap_resumes_from_cap);
  RUN_TEST(test_boot_then_failed_polls_never_command);
  RUN_TEST(test_algo_pm_threshold_equality_edges);
  RUN_TEST(test_algo_fast_attack_and_slow_decay);
  RUN_TEST(test_algo_scenario_A_particleless_voc_day);
  RUN_TEST(test_algo_night_gas_cap_and_multigas_disable);
  RUN_TEST(test_algo_scenario_B_gas_tail_holds_speed);
  RUN_TEST(test_manual_override_takes_precedence_over_watchdog);
  RUN_TEST(test_button_cycle_and_manual_timeout);
  RUN_TEST(test_config_patch_validation);
  RUN_TEST(test_ag_parse_and_stale);
  RUN_TEST(test_led_night_silence_and_priorities);
  RUN_TEST(test_TC08_unpaired_device_rejects_everything);
  RUN_TEST(test_TC08_pairing_window_issues_token_once_and_revokes_old);
  RUN_TEST(test_TC08_pairing_window_expires);
  RUN_TEST(test_TC08_bad_tokens_lock_out_then_recover);
  RUN_TEST(test_TC08_token_format_and_clear);
  RUN_TEST(test_TC08_pairing_led_patterns);
  RUN_TEST(test_TC09_frozen_readings_escalate_to_failed_polls);
  RUN_TEST(test_review_pairing_window_never_reopens_after_millis_wrap);
  RUN_TEST(test_review_lockout_never_reappears_after_millis_wrap);
  RUN_TEST(test_review_header_whitespace_and_interleaved_failures);
  RUN_TEST(test_review_recovery_resends_after_short_outage);
  RUN_TEST(test_review_missing_serial_is_mismatch);
  RUN_TEST(test_TC08_lockout_is_per_client);
  RUN_TEST(test_TC08_pairing_host_and_origin_guard);
  RUN_TEST(test_config_ir_nec_address_validation);
  RUN_TEST(test_matter_mappers_breakpoints_and_edges);
  RUN_TEST(test_ota_validation_helpers);
  RUN_TEST(test_sim_inject_parser);
  RUN_TEST(test_clock_parse_edges_and_night_schedule_wrap);
  RUN_TEST(test_multigas_exact_thresholds_and_reset_blocking);
  RUN_TEST(test_volume_250_scaling_and_manual_to_auto_dwell);
  RUN_TEST(test_ag_parse_partial_dropouts_and_bounds);
  RUN_TEST(test_api_auth_eviction_and_short_headers);
  RUN_TEST(test_control_core_persisted_manual_and_timeout_ordering);
  RUN_TEST(test_led_compose_and_double_flash_restart);
  RUN_TEST(test_config_edge_validation_and_mode_masks);
  RUN_TEST(test_ir_test_send_preserves_unknown_sent_speed);
  return UNITY_END();
}
