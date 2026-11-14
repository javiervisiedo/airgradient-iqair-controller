// main.cpp: atoms3-iaq-matter-controller
//
// Tasks:
//   loop()  (Arduino loop task, core 1): button, command queue, control decisions,
//           IR executor, LED renderer, status publishing. Never blocks.
//   net     (core 0): clock sync tiers + AirGradient polling every poll_interval_sec.
//           Blocking HTTP with bounded timeouts is allowed here.
//   Matter / httpd tasks: post BridgeCmd messages only.
// Both loop and net are registered with the 15 s task watchdog.
#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include <atomic>

#include "ag_client.h"
#include "clock_sync.h"
#include "iaq_algorithm.h"
#include "ir_nec_purifier.h"
#include "matter_ha_bridge.h"
#include "status_led.h"
#include "sys_config.h"
#ifdef WOKWI_SIM
#include "sim_inject.h"
#endif

#if !defined(WOKWI_SIM) && __has_include("secrets.h")
#include "secrets.h"  // optional dev Wi-Fi (WIFI_SSID / WIFI_PASSWORD); gitignored
#endif

namespace {
constexpr const char* kTag = "main";
constexpr uint32_t kWdtTimeoutMs = 15000;
constexpr uint32_t kStatusPublishMs = 5000;
constexpr uint32_t kConfigSaveDebounceMs = 5000;
constexpr uint32_t kHeapWarnBytes = 120 * 1024;   // SRS §6.2
constexpr uint32_t kDebounceMs = 50;
constexpr uint32_t kShortMaxMs = 1000;      // < 1 s: mode cycle
constexpr uint32_t kPairHoldMinMs = 3000;   // released after 3..10 s: REST API pairing mode
constexpr uint32_t kLongMs = 10000;         // >= 10 s: Matter factory reset
constexpr uint32_t kFactoryBlinkMs = 2000;

// ---- shared state --------------------------------------------------------
AppConfig g_cfg;
SemaphoreHandle_t g_cfg_mutex;
QueueHandle_t g_cmd_q;    // BridgeCmd: Matter / REST -> loop
QueueHandle_t g_poll_q;   // PollResult: net -> loop

struct PollResult {
  bool ok;
  ag::Measurement m;
  uint32_t resolved_ip;
  AgClient::Source source;
  bool stale;    // >= 3 unchanged readings: warning only
  bool frozen;   // >= 4 unchanged readings: counts as a failed poll (SRS §3.1 rule 5)
#ifdef WOKWI_SIM
  bool has_night_override;
  bool night_override;
#endif
};

iaq::ControlCore g_core;
IrNecPurifier g_ir;
StatusLed g_led;
ClockSync g_clock;
AgClient g_ag;
MatterHaBridge g_bridge;

// Wi-Fi events -> net task
enum class WifiEvent : uint8_t { NONE = 0, DISCONNECTED = 1, GOT_IP = 2 };
std::atomic<uint8_t> g_wifi_ev{static_cast<uint8_t>(WifiEvent::NONE)};
std::atomic<uint32_t> g_gateway_ip{0};
std::atomic<uint32_t> g_local_ip{0};
std::atomic<bool> g_ag_invalidate{false};  // set by loop, consumed by net task
std::atomic<bool> g_net_task_alive{false};
#ifdef WOKWI_SIM
std::atomic<bool> g_sim_injected{false};
#endif
constexpr uint32_t kTestIrReassertMs = 10000;  // after a test_ir, restore the commanded speed
constexpr uint32_t kRebootDelayMs = 500;       // let the HTTP response flush
constexpr uint32_t kNoNetworkFailAfterMs = 120000;
constexpr uint32_t kOtaMinSoakMs = 5000;       // verify OTA after 5 s when net+http are up
constexpr uint32_t kOtaVerifyFallbackMs = 30000;  // verify offline/uncommissioned after 30 s healthy loop+net

// ---- loop-task state -----------------------------------------------------
AppConfig g_loop_cfg;            // loop-local copy, refreshed when the config changes
int g_ir_speed = 0;             // speed last commanded over IR (assumed purifier state)
bool g_ir_speed_known = false;
uint32_t g_last_ir_refresh_ms = 0;
bool g_ir_flash_active = false;
uint32_t g_ir_flash_start_ms = 0;
uint32_t g_ir_flash_seq = 0;
bool g_test_reassert_pending = false;
uint32_t g_test_reassert_at_ms = 0;
const esp_partition_t* g_ota_pending_part = nullptr;
bool g_loop_ran_clean = false;
uint32_t g_boot_ms = 0;
bool g_cfg_dirty = false;
uint32_t g_cfg_dirty_since_ms = 0;
int g_last_yday = -1;
iaq::Reading g_last_raw;        // last valid raw telemetry
ag::Measurement g_last_meas;
bool g_have_meas = false;
uint32_t g_ag_ip = 0;
AgClient::Source g_ag_source = AgClient::Source::NONE;
bool g_ag_stale = false;
uint32_t g_last_status_ms = 0;
bool g_status_dirty = true;

// button
bool g_btn_raw = false, g_btn_stable = false;
uint32_t g_btn_change_ms = 0, g_btn_down_ms = 0;
bool g_factory_pending = false;
uint32_t g_factory_since_ms = 0;
bool g_btn_armed = false;     // set in setup() if released at boot, else on the first release
bool g_factory_done = false;  // factory reset ran; ignore the button until released
bool g_reboot_pending = false;
uint32_t g_reboot_at_ms = 0;

void ip_str(uint32_t ip, char* out, size_t len) {
  if (ip == 0) {
    out[0] = '\0';
    return;
  }
  snprintf(out, len, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

bool current_is_night() {
  int hour = 0;
  const bool valid = g_clock.local_hour(&hour);
  return iaq::resolve_is_night(valid, hour, g_core.modes().night_force(), g_loop_cfg.day_start_hour,
                               g_loop_cfg.night_start_hour);
}

void mark_config_dirty(uint32_t now) {
  g_cfg_dirty = true;
  g_cfg_dirty_since_ms = now;
}

// Persist mode changes made via Matter / button (debounced against slider spam).
void sync_mode_to_config(uint32_t now) {
  xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
  const OperatingMode m = g_core.modes().mode();
  const uint8_t ms = g_core.modes().manual_speed();
  if (g_cfg.operating_mode != m || g_cfg.manual_speed != ms) {
    g_cfg.operating_mode = m;
    g_cfg.manual_speed = ms;
    mark_config_dirty(now);
  }
  xSemaphoreGive(g_cfg_mutex);
}

void command_purifier(const iaq::TickResult& r) {
  if (!r.speed_changed && !r.force_transmit) return;  // no decision (e.g. boot without data)
  const bool wake_after_test = g_test_reassert_pending;
  g_test_reassert_pending = false;
  const int from = g_ir_speed_known ? g_ir_speed : -1;
  g_ir.request(r.commanded_speed, r.force_transmit || wake_after_test, wake_after_test);
  g_ir_speed = r.commanded_speed;
  g_ir_speed_known = true;
  g_last_ir_refresh_ms = millis();
  g_status_dirty = true;
  ESP_LOGI(kTag, "purifier %d -> %d%s", from, r.commanded_speed, r.force_transmit ? " (forced)" : "");
}

void apply_mode_change(uint32_t now) {
  command_purifier(g_core.on_mode_change(now, current_is_night()));
  sync_mode_to_config(now);
  g_status_dirty = true;
}

// ---- command handling ----------------------------------------------------
void handle_config_patched(uint32_t mask, uint8_t mode, uint8_t manual_speed, uint32_t now) {
  AppConfig c;
  xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
  c = g_cfg;
  xSemaphoreGive(g_cfg_mutex);
  if (mask & (CFG_CHANGED_VOLUME | CFG_CHANGED_ALGO | CFG_CHANGED_MODE | CFG_CHANGED_MANUAL_TIMEOUT)) {
    g_core.apply_config(c);  // thresholds & manual timeout
  }
  if (mask & CFG_CHANGED_IR) g_ir.apply_config(c);
  if (mask & CFG_CHANGED_TZ) g_clock.set_timezone(c.tz_posix);
  if (mask & CFG_CHANGED_AG) g_ag_invalidate = true;  // AgClient is owned by the net task
  g_loop_cfg = c;
  if (mask & CFG_CHANGED_IR && ir_address_looks_swapped(c.ir_nec_address)) {
    ESP_LOGW(kTag, "ir_nec_address 0x00FF transmits NEC address 0xFF; the verified IQAir address word is 0xFF00");
  }
  if (mask & CFG_CHANGED_MODE) {
    switch (static_cast<OperatingMode>(mode)) {
      case OperatingMode::AUTO:
        g_core.modes().set_auto(now);
        if (mask & CFG_CHANGED_MANUAL_SPEED) g_core.modes().set_manual_speed(manual_speed);
        break;
      case OperatingMode::MANUAL:
        g_core.modes().set_manual(manual_speed, now);
        break;
      case OperatingMode::NIGHT_FORCE:
        g_core.modes().set_night_force(true, now);
        if (mask & CFG_CHANGED_MANUAL_SPEED) g_core.modes().set_manual_speed(manual_speed);
        break;
    }
    command_purifier(g_core.on_mode_change(now, current_is_night()));
    sync_mode_to_config(now);
  } else if (mask & CFG_CHANGED_MANUAL_SPEED) {
    if (g_core.modes().mode() == OperatingMode::MANUAL) {
      g_core.modes().set_manual(manual_speed, now);
      command_purifier(g_core.on_mode_change(now, current_is_night()));
    } else {
      g_core.modes().set_manual_speed(manual_speed);  // preserve preset without entering MANUAL
    }
    sync_mode_to_config(now);
  } else if (mask & CFG_CHANGED_MANUAL_TIMEOUT) {
    if (g_core.modes().poll_timeout(now)) apply_mode_change(now);
  }
  mark_config_dirty(now);  // the loop task is the only writer of NVS / LittleFS
  g_status_dirty = true;
}

void drain_commands(uint32_t now) {
  BridgeCmd cmd;
  while (xQueueReceive(g_cmd_q, &cmd, 0) == pdTRUE) {
    switch (cmd.type) {
      case BridgeCmdType::SET_AUTO:
        g_core.modes().set_auto(now);
        apply_mode_change(now);
        break;
      case BridgeCmdType::SET_MANUAL:
        g_core.modes().set_manual(cmd.speed, now);
        apply_mode_change(now);
        break;
      case BridgeCmdType::SET_NIGHT_FORCE:
        g_core.modes().set_night_force(cmd.on, now);
        apply_mode_change(now);
        break;
      case BridgeCmdType::TEST_IR:
        // Diagnostic only: physically sends the code without mutating sent_speed_
        // or changing the controller's commanded speed (restored after kTestIrReassertMs).
        g_ir.test_send(cmd.speed);
        g_test_reassert_pending = g_ir_speed_known;
        g_test_reassert_at_ms = now + kTestIrReassertMs;
        break;
      case BridgeCmdType::REBOOT:
        g_reboot_pending = true;
        g_reboot_at_ms = now + kRebootDelayMs;
        break;
      case BridgeCmdType::CONFIG_PATCHED:
        handle_config_patched(cmd.cfg_mask, cmd.mode, cmd.speed, now);
        break;
    }
  }
}

void drain_polls(uint32_t now) {
  PollResult pr;
  while (xQueueReceive(g_poll_q, &pr, 0) == pdTRUE) {
    // SRS §3.1 rule 5: a frozen reading (monitor counter stuck beyond the stale
    // warning) is treated as a failed poll so the watchdog caps can engage.
    const bool usable = pr.ok && !pr.frozen;
    if (pr.ok && pr.frozen) ESP_LOGW(kTag, "AirGradient reading frozen: counted as a failed poll");
    iaq::Reading r;
    if (usable) {  // frozen values are not published as live telemetry
      r.pm25_valid = pr.m.pm25_valid;
      r.pm25 = pr.m.pm25;
      r.tvoc_valid = pr.m.tvoc_valid;
      r.tvoc_index = pr.m.tvoc_index;
      r.nox_valid = pr.m.nox_valid;
      r.nox_index = pr.m.nox_index;
      g_last_raw = r;
      g_last_meas = pr.m;
      g_have_meas = true;
    }
    if (pr.ok) g_ag_ip = pr.resolved_ip;
    g_ag_source = pr.source;
    g_ag_stale = pr.stale;
#ifdef WOKWI_SIM
    const bool night = pr.has_night_override ? pr.night_override : current_is_night();
#else
    const bool night = current_is_night();
#endif
    const iaq::TickResult res = g_core.on_poll(usable, r, night, now);
    if (res.fault_event == iaq::SensorFaultMonitor::Event::ENTERED_ALARM) {
      ESP_LOGE(kTag, "AirGradient unreachable for %d polls: ALARM", g_core.fault().fail_count());
    } else if (res.fault_event == iaq::SensorFaultMonitor::Event::RECOVERED) {
      ESP_LOGW(kTag, "AirGradient recovered");
    }
    if (!usable && res.speed_changed) {
      ESP_LOGW(kTag, "watchdog cap (%s, %d failed polls): speed -> %d", night ? "night" : "day",
               g_core.fault().fail_count(), res.commanded_speed);
    }
    sync_mode_to_config(now);  // a manual timeout may have reverted to AUTO
    command_purifier(res);
    float pm_avg = NAN;
    g_core.algo().pm_avg(&pm_avg);
    ESP_LOGI(kTag, "[ctl] ok=%d frozen=%d pm=%.1f sma=%.1f tvoc=%.0f nox=%.1f night=%d pm_spd=%d gas_spd=%d algo=%d cmd=%d mode=%s ag=%s fails=%d heap=%u min_heap=%u",
             pr.ok, pr.frozen, r.pm25, pm_avg, r.tvoc_index, r.nox_index, night, g_core.algo().pm_speed(),
             g_core.algo().gas_speed(), g_core.algo().current_speed(), res.commanded_speed,
             operating_mode_to_str(g_core.modes().mode()), iaq::ag_status_to_str(g_core.fault().status()),
             g_core.fault().fail_count(), static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    g_status_dirty = true;
  }
}

// ---- button --------------------------------------------------------------
void handle_button(uint32_t now) {
  const bool raw = digitalRead(pins::kButton) == LOW;  // active-low
  if (raw != g_btn_raw) {
    g_btn_raw = raw;
    g_btn_change_ms = now;
  }
  if (now - g_btn_change_ms >= kDebounceMs && g_btn_stable != g_btn_raw) {
    g_btn_stable = g_btn_raw;
    if (g_btn_stable) {
      g_btn_down_ms = now;
    } else if (!g_btn_armed) {
      g_btn_armed = true;          // first release since boot: gestures enabled from now on
    } else if (g_factory_done) {
      g_factory_done = false;      // release after a (non-rebooting) factory reset
    } else if (!g_factory_pending) {
      const uint32_t held = now - g_btn_down_ms;
      if (held < kShortMaxMs) {
        g_core.modes().button_cycle(now);
        ESP_LOGI(kTag, "button short: mode=%s speed=%u", operating_mode_to_str(g_core.modes().mode()),
                 g_core.modes().manual_speed());
        apply_mode_change(now);
      } else if (held >= kPairHoldMinMs && held < kLongMs) {
        g_bridge.auth_open_pairing(now);  // cyan heartbeat LED while the window is open
        g_status_dirty = true;
      }
      // 1..3 s: ignored
    }
  }
  // A button held (or stuck) since boot never counts: it must be released once first.
  if (g_btn_armed && g_btn_stable && !g_factory_pending && !g_factory_done && now - g_btn_down_ms >= kLongMs) {
    g_factory_pending = true;
    g_factory_since_ms = now;
    ESP_LOGW(kTag, "button long press: Matter decommission in %u ms", static_cast<unsigned>(kFactoryBlinkMs));
  }
  if (g_factory_pending && now - g_factory_since_ms >= kFactoryBlinkMs) {
    g_bridge.auth_clear();     // factory reset also forgets the REST API token
    state_erase_api_token();
    g_factory_pending = false;
    g_factory_done = true;     // latched until the button is released
    g_bridge.decommission();   // erases fabrics and restarts into BLE commissioning
  }
}

// ---- status --------------------------------------------------------------
void publish_status(uint32_t now) {
  StatusSnapshot s;
  const iaq::DualPathController& a = g_core.algo();
  const iaq::VolumeScaling& vs = a.scaling();
  s.current_speed = g_core.commanded_speed();
  s.operating_mode = g_core.modes().mode();
  s.pm_target_speed = a.pm_speed();
  s.gas_target_speed = a.gas_speed();
  s.dominant_trigger = iaq::trigger_to_str(a.dominant_trigger());
  s.current_airflow_m3h = iaq::kAirflowM3h[s.current_speed];
  s.effective_ach = iaq::effective_ach(s.current_speed, vs.volume_m3);
  s.room_volume_m3 = vs.volume_m3;
  s.rho_volume_factor = vs.rho;
  s.dwell_stepdown_mins = a.dwell_minutes();
  s.min_stepdown_hold_mins = vs.min_stepdown_hold;
  s.is_night_mode = current_is_night();
  s.clock_sync_status = clk::status_to_str(g_clock.status());
  g_clock.local_time_iso(s.local_time_iso, sizeof(s.local_time_iso));
  s.ag_status = iaq::ag_status_to_str(g_core.fault().status());
  s.alarm_sensor_unreachable = g_core.fault().alarm();
  ip_str(g_ag_ip, s.ag_resolved_ip, sizeof(s.ag_resolved_ip));
  s.ag_source = AgClient::source_to_str(g_ag_source);
  s.ag_fail_count = g_core.fault().fail_count();
  s.ag_data_stale = g_ag_stale;
  s.api_paired = g_bridge.auth_has_token();
  s.api_pairing_active = g_bridge.auth_pairing_open(now);
  s.have_reading = g_have_meas;
  if (g_have_meas) {
    float v;
    s.pm25_raw = g_last_meas.pm25_valid ? g_last_meas.pm25 : NAN;
    s.pm25_sma5 = a.pm_avg(&v) ? v : NAN;
    s.tvoc_raw = g_last_meas.tvoc_valid ? g_last_meas.tvoc_index : NAN;
    s.tvoc_sma5 = a.tvoc_avg(&v) ? v : NAN;
    s.nox_raw = g_last_meas.nox_valid ? g_last_meas.nox_index : NAN;
    s.nox_sma5 = a.nox_avg(&v) ? v : NAN;
    s.co2_ppm = g_last_meas.co2_valid ? g_last_meas.co2_ppm : NAN;
    s.temperature_c = g_last_meas.temp_valid ? g_last_meas.temp_c : NAN;
    s.humidity_pct = g_last_meas.rh_valid ? g_last_meas.rh_pct : NAN;
  }
  s.matter_commissioned = g_bridge.commissioned();
  s.matter_fabrics_count = s.matter_commissioned ? g_bridge.fabric_count() : 0;
  s.speed_changes_today = g_core.speed_changes_today();
  s.wifi_rssi_dbm = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  s.free_heap_bytes = esp_get_free_heap_size();
  s.min_free_heap_bytes = esp_get_minimum_free_heap_size();
  s.uptime_s = now / 1000;
  s.ir_frames_sent = g_ir.tx_frames();
  g_bridge.publish(s);
  if (s.free_heap_bytes < kHeapWarnBytes) {
    ESP_LOGW(kTag, "low heap: %u bytes (min %u)", static_cast<unsigned>(s.free_heap_bytes),
             static_cast<unsigned>(s.min_free_heap_bytes));
  }
}

// Persists the latest paired token immediately from loop(); on NVS failure,
// retries once per second until the write succeeds (SRS §5.3).
void persist_api_token(uint32_t now, bool force = false) {
  if (!g_bridge.auth_has_unsaved()) return;
  static bool retry_backoff = false;
  static uint32_t last_fail_ms = 0;
  if (!force && retry_backoff && (now - last_fail_ms) < 1000u) return;
  char token[auth::kTokenChars + 1];
  uint32_t gen = 0;
  if (!g_bridge.auth_unsaved_token(token, &gen)) return;
  if (state_save_api_token(token)) {
    g_bridge.auth_mark_saved(gen);
    retry_backoff = false;
    g_status_dirty = true;
  } else {
    retry_backoff = true;
    last_fail_ms = now;
    ESP_LOGE(kTag, "API token NVS write failed; retrying");
  }
  auth::secure_zero(token, sizeof(token));
}

void daily_rollover() {
  if (!g_clock.valid()) return;
  const time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  if (g_last_yday >= 0 && lt.tm_yday != g_last_yday) g_core.reset_daily_counters();
  g_last_yday = lt.tm_yday;
}

// ---- network task --------------------------------------------------------
void net_task(void*) {
  esp_task_wdt_add(nullptr);
  const uint32_t boot_ms = millis();
  uint32_t last_poll = 0;
  bool first_poll = true;
  ag::StaleDetector stale;
  for (;;) {
    const uint32_t now = millis();
    const auto ev = static_cast<WifiEvent>(g_wifi_ev.exchange(static_cast<uint8_t>(WifiEvent::NONE)));
    if (ev == WifiEvent::GOT_IP) {
      g_clock.on_got_ip(g_gateway_ip.load(), now);
      g_bridge.update_mdns(g_local_ip.load());  // set device_ip_ before httpd opens
      g_bridge.start_http();
    } else if (ev == WifiEvent::DISCONNECTED) {
      g_clock.on_disconnected();
    }
    g_clock.loop(now);
    esp_task_wdt_reset();

    AppConfig cfg;
    xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    cfg = g_cfg;
    xSemaphoreGive(g_cfg_mutex);
    if (g_ag_invalidate) {
      g_ag_invalidate = false;
      g_ag.invalidate_cache();
    }
    const uint32_t interval = static_cast<uint32_t>(cfg.poll_interval_sec) * 1000u;
#ifdef WOKWI_SIM
    // In simulation without a static mock server (or once serial inject is active),
    // suppress background HTTP/mDNS polling so it doesn't corrupt scenario fail_count_.
    const bool allow_http_poll = (cfg.ag_target_host[0] != '\0') && !g_sim_injected.load();
#else
    const bool allow_http_poll = true;
#endif
    if (allow_http_poll && (first_poll ? (WiFi.status() == WL_CONNECTED) : (now - last_poll >= interval))) {
      first_poll = false;
      last_poll = now;
      PollResult pr = {};
      pr.ok = g_ag.poll(cfg, &pr.m);
      pr.resolved_ip = g_ag.resolved_ip();
      pr.source = g_ag.last_source();
      pr.stale = pr.ok ? stale.update(pr.m) : stale.stale();
      pr.frozen = pr.ok && stale.frozen();
      static bool warned_no_boot = false;
      if (pr.ok && !pr.m.boot_valid && !warned_no_boot) {
        warned_no_boot = true;
        ESP_LOGW(kTag, "AirGradient reply has no 'boot' counter: stale/frozen detection inactive");
      }
      if (pr.stale) ESP_LOGW(kTag, "AirGradient data stale: boot counter unchanged for %d polls", stale.unchanged());
      if (xQueueSend(g_poll_q, &pr, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(kTag, "poll queue full");
    } else if (allow_http_poll && first_poll && (now - boot_ms >= kNoNetworkFailAfterMs)) {
      // No network for 2 min after boot: start counting failed polls anyway so the
      // watchdog caps and the unreachable alarm also cover "never connected".
      first_poll = false;
      last_poll = now;
      PollResult pr = {};
      pr.ok = false;
      xQueueSend(g_poll_q, &pr, pdMS_TO_TICKS(100));
    }
    g_net_task_alive.store(true);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_local_ip.store(info.got_ip.ip_info.ip.addr);
      g_gateway_ip.store(info.got_ip.ip_info.gw.addr);
      g_wifi_ev.store(static_cast<uint8_t>(WifiEvent::GOT_IP));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_wifi_ev.store(static_cast<uint8_t>(WifiEvent::DISCONNECTED));
      break;
    default:
      break;
  }
}

#ifdef WOKWI_SIM
void handle_sim_serial() {
  static char line[128];
  static size_t len = 0;
  static bool overflow = false;
  int budget = 128;
  while (Serial.available() > 0 && budget-- > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (!overflow && len > 0) {
        line[len] = '\0';
        sim::InjectCmd cmd;
        if (sim::parse_inject_line(line, &cmd)) {
          g_sim_injected.store(true);
          PollResult pr = {};
          pr.ok = !cmd.fail;
          if (pr.ok) {
            pr.m.pm25_valid = true;
            pr.m.pm25 = cmd.pm25;
            pr.m.tvoc_valid = cmd.tvoc_valid;
            pr.m.tvoc_index = cmd.tvoc;
            pr.m.nox_valid = cmd.nox_valid;
            pr.m.nox_index = cmd.nox;
            pr.m.co2_valid = cmd.co2_valid;
            pr.m.co2_ppm = cmd.co2;
            pr.source = AgClient::Source::TARGET_HOST;
          }
          pr.has_night_override = cmd.has_night;
          pr.night_override = cmd.night;
          if (xQueueSend(g_poll_q, &pr, 0) == pdTRUE) {
            ESP_LOGI(kTag, "[sim] injected %s", cmd.fail ? "fail" : "reading");
          } else {
            ESP_LOGW(kTag, "[sim] poll queue full; inject dropped");
          }
        } else {
          ESP_LOGW(kTag, "[sim] ignored line: %s", line);
        }
      } else if (overflow) {
        ESP_LOGW(kTag, "[sim] line too long; ignored");
      }
      len = 0;
      overflow = false;
    } else if (!overflow) {
      if (len + 1 < sizeof(line)) {
        line[len++] = static_cast<char>(c);
      } else {
        overflow = true;
      }
    }
  }
}
#endif
}  // namespace

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB-CDC enumerate so boot logs are visible (setup only)
  ESP_LOGI(kTag, "atoms3-iaq-matter-controller boot, heap=%u", static_cast<unsigned>(esp_get_free_heap_size()));

  // Task watchdog: 15 s, panic on trigger; loop task registered here.
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = kWdtTimeoutMs;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);

  pinMode(pins::kButton, INPUT_PULLUP);
  g_btn_armed = digitalRead(pins::kButton) == HIGH;  // held/stuck at boot -> wait for a release
  if (!g_btn_armed) ESP_LOGW(kTag, "button pressed at boot: ignored until released");

  g_cfg_mutex = xSemaphoreCreateMutex();
  g_cmd_q = xQueueCreate(16, sizeof(BridgeCmd));
  g_poll_q = xQueueCreate(4, sizeof(PollResult));

  config_storage_begin();  // may format LittleFS on first boot
  esp_task_wdt_reset();
  config_load(&g_cfg);
  char err[96];
  if (!config_validate(g_cfg, err, sizeof(err))) {
    ESP_LOGE(kTag, "stored config invalid (%s); using defaults", err);
    config_set_defaults(&g_cfg);
  }
  if (ir_address_looks_swapped(g_cfg.ir_nec_address)) {
    ESP_LOGW(kTag, "ir_nec_address 0x00FF transmits NEC address 0xFF; the verified IQAir address word is 0xFF00");
  }
  esp_task_wdt_reset();

  g_led.begin(pins::kRgbLed);
  const bool ir_ok = g_ir.begin(pins::kIrTx, g_cfg);
  if (!ir_ok) ESP_LOGE(kTag, "IR transmitter init failed");
  g_ir.on_transmit([](uint8_t, uint16_t) {
    g_ir_flash_start_ms = g_ir.last_tx_ms();  // exact loop `now` avoids 1-ms unsigned underflow
    g_ir_flash_active = true;
    ++g_ir_flash_seq;
  });

  command_purifier(g_core.begin(g_cfg, millis()));
  g_loop_cfg = g_cfg;

  // Initialize Wi-Fi in STA-only mode using Matter's compact buffer budget before
  // WiFi.mode(WIFI_STA) / Matter.begin():
  //  - First esp_wifi_init() owns the buffer counts (~30 KB less SRAM than Arduino defaults).
  //  - Always calling WiFi.mode(WIFI_STA) ensures Arduino Wi-Fi events work under CHIPoBLE
  //    provisioning and clears any stale WIFI_MODE_AP/APSTA left in NVS so Matter never
  //    triggers wifi_softap_start() / ieee80211_hostap_attach().
  {
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) == ESP_ERR_WIFI_NOT_INIT) {
      wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
      wcfg.static_rx_buf_num = 4;
      wcfg.dynamic_rx_buf_num = 8;
      wcfg.tx_buf_type = 1;
      wcfg.static_tx_buf_num = 0;
      wcfg.dynamic_tx_buf_num = 8;
      if (wcfg.cache_tx_buf_num < 4) wcfg.cache_tx_buf_num = 4;
      wcfg.rx_ba_win = 6;
      const esp_err_t werr = esp_wifi_init(&wcfg);
      if (werr != ESP_OK) ESP_LOGW(kTag, "esp_wifi_init: %s", esp_err_to_name(werr));
    }
  }
  // Network stack before SNTP setup (tier 1 must be armed before DHCP).
  Network.begin();
  WiFi.onEvent(on_wifi_event);
  WiFi.mode(WIFI_STA);
  g_clock.begin(g_cfg.tz_posix);
  g_ag.begin();

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  // Optional dev/simulation credentials; Matter can still commission on-network.
#ifdef WOKWI_SIM
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);  // Wokwi-GUEST is on channel 6
  ESP_LOGI(kTag, "using simulation Wi-Fi (%s)", WIFI_SSID);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  ESP_LOGW(kTag, "using compiled-in Wi-Fi credentials (secrets.h)");
#endif
#else
  // Matter's ConnectivityManagerImpl owns STA association/reconnect when Wi-Fi is
  // provisioned over BLE; disable Arduino STA auto-reconnect so it cannot fight it.
  WiFi.setAutoReconnect(false);
#endif

  esp_task_wdt_reset();
  const bool bridge_ok = g_bridge.begin(&g_cfg, g_cfg_mutex, g_cmd_q);
  if (!bridge_ok) ESP_LOGE(kTag, "Matter bridge init failed");
  {
    char token[auth::kTokenChars + 1];
    state_load_api_token(token, sizeof(token));  // empty if never paired
    g_bridge.auth_set_active(token);
    auth::secure_zero(token, sizeof(token));
  }
  esp_task_wdt_reset();
  g_clock.set_matter_time_source([](time_t* t) { return g_bridge.matter_utc(t); });

  // Defer OTA rollback cancellation until net_task + clean loop() + HTTP readiness
  // (or bounded offline soak if router/commissioning is absent). Bootloader has
  // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in Arduino-ESP32S3.
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (ir_ok && bridge_ok) {
      g_ota_pending_part = running;
      ESP_LOGI(kTag, "OTA pending verify on %s; awaiting loop/net readiness", running->label);
    } else {
      ESP_LOGE(kTag, "critical init failed in pending-verify state; rolling back");
      delay(200);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  g_boot_ms = millis();
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 2, nullptr, 0);
  ESP_LOGI(kTag, "setup done, heap=%u", static_cast<unsigned>(esp_get_free_heap_size()));
}

void loop() {
  const uint32_t now = millis();

  handle_button(now);
  drain_commands(now);
  if (g_bridge.take_reboot_request()) {
    g_reboot_pending = true;
    g_reboot_at_ms = now + kRebootDelayMs;
  }
#ifdef WOKWI_SIM
  handle_sim_serial();
#endif
  drain_polls(now);
  persist_api_token(now);

  // Manual-timeout expiry between polls (1 s granularity is plenty).
  static uint32_t last_mode_check = 0;
  if (now - last_mode_check >= 1000) {
    last_mode_check = now;
    if (g_core.modes().poll_timeout(now)) apply_mode_change(now);
    daily_rollover();
  }

  // Periodic idempotent IR refresh while the purifier should be running.
  const AppConfig& cfg = g_loop_cfg;
  if (cfg.ir_refresh_mins > 0 && g_ir_speed_known && g_ir_speed >= 1 && !g_ir.busy() &&
      now - g_last_ir_refresh_ms >= static_cast<uint32_t>(cfg.ir_refresh_mins) * 60000u) {
    g_last_ir_refresh_ms = now;
    g_test_reassert_pending = false;
    g_ir.resend(g_ir_speed);
  }

  if (g_test_reassert_pending && static_cast<int32_t>(now - g_test_reassert_at_ms) >= 0 && !g_ir.busy()) {
    g_test_reassert_pending = false;
    g_last_ir_refresh_ms = now;
    g_ir.resend(g_ir_speed);  // assume_off=true: wakes purifier if test_ir(0) turned it off
  }

  g_ir.loop(now);

  if (g_ir_flash_active && now - g_ir_flash_start_ms >= led::kDoubleFlashMs) {
    g_ir_flash_active = false;
  }
  led::Inputs li;
  const bool gesture_hold = g_btn_armed && !g_factory_done && g_btn_stable;
  li.factory_reset_warning = g_factory_pending || (gesture_hold && now - g_btn_down_ms >= kLongMs);
  li.pair_hold_ready = gesture_hold && !li.factory_reset_warning && now - g_btn_down_ms >= kPairHoldMinMs;
  li.pairing_active = g_bridge.auth_pairing_open(now);
  li.sensor_alarm = g_core.fault().alarm();
  li.wifi_connected = WiFi.status() == WL_CONNECTED;
  li.matter_commissioned = g_bridge.commissioned();
  li.ag_fail_count = g_core.fault().fail_count();
  li.ir_flash_active = g_ir_flash_active;
  li.ir_flash_seq = g_ir_flash_seq;
  li.manual_mode = g_core.modes().mode() == OperatingMode::MANUAL;
  li.current_speed = g_core.commanded_speed();
  li.is_night = current_is_night();
  li.night_led_enabled = cfg.night_led_enabled;
  g_led.loop(now, li);

  if (g_status_dirty || now - g_last_status_ms >= kStatusPublishMs) {
    g_status_dirty = false;
    g_last_status_ms = now;
    publish_status(now);
  }

  if (g_cfg_dirty && now - g_cfg_dirty_since_ms >= kConfigSaveDebounceMs) {
    AppConfig c;
    xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    c = g_cfg;
    xSemaphoreGive(g_cfg_mutex);
    if (config_save(c)) {
      g_cfg_dirty = false;
    } else {
      g_cfg_dirty_since_ms = now;  // retry after debounce on NVS write error
    }
  }

  if (g_ota_pending_part != nullptr && g_loop_ran_clean && g_net_task_alive.load()) {
    const bool net_ready = g_bridge.http_started() && (now - g_boot_ms >= kOtaMinSoakMs);
    const bool offline_fallback = (now - g_boot_ms >= kOtaVerifyFallbackMs);
    if (net_ready || offline_fallback) {
      const esp_err_t verr = esp_ota_mark_app_valid_cancel_rollback();
      if (verr == ESP_OK) {
        ESP_LOGI(kTag, "OTA image verified on %s (%s); rollback cancelled", g_ota_pending_part->label,
                 net_ready ? "net+http ready" : "offline soak");
      } else {
        ESP_LOGE(kTag, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(verr));
      }
      g_ota_pending_part = nullptr;
    }
  }

  if (g_reboot_pending && static_cast<int32_t>(now - g_reboot_at_ms) >= 0 && !g_ir.busy()) {
    persist_api_token(now, true);
    if (g_cfg_dirty) {
      AppConfig c;
      xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
      c = g_cfg;
      xSemaphoreGive(g_cfg_mutex);
      if (config_save(c)) g_cfg_dirty = false;
    }
    ESP_LOGW(kTag, "rebooting now");
    esp_restart();
  }

  g_loop_ran_clean = true;
  esp_task_wdt_reset();
  delay(5);  // yield; keeps the loop at ~200 Hz
}
