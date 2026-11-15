// matter_ha_bridge.h: Matter over Wi-Fi endpoints (Apple Home / Google Home /
// Home Assistant via Multi-Admin) + local HTTP REST API (SRS §2.1, §5).
//
// Endpoints (created in this order, IDs assigned by esp-matter):
//   EP1 Air Purifier 0x002D: FanControl (Off/Low/Med/High/Auto + 6-step Percent)
//   EP2 Air Quality Sensor 0x002C (+ Temperature 0x0302, Humidity 0x0307 device
//       types): AirQuality, PM2.5 (numeric), CO2 (numeric), TVOC (level), NO2 (level),
//       TemperatureMeasurement, RelativeHumidityMeasurement, BooleanState (fault)
//
// Threading: Matter callbacks (CHIP task) and HTTP handlers (httpd task) never
// touch hardware; they post BridgeCmd messages to the main loop's queue.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sys_config.h"

// Pure helpers (compiled on both device and host UNIT_TEST builds).
uint8_t matter_air_quality_from_pm25(float pm25);   // AirQualityEnum 1..6, 0 unknown
uint8_t matter_level_from_tvoc_index(float idx);    // LevelValueEnum 1..4, 0 unknown
uint8_t matter_level_from_nox_index(float idx);     // LevelValueEnum 1..4, 0 unknown

constexpr uint8_t kOtaImageMagic = 0xE9;
constexpr uint16_t kOtaChipIdEsp32S3 = 0x0009;
constexpr size_t kOtaHeaderBytes = 24;              // sizeof(esp_image_header_t)
constexpr size_t kOtaMinImageBytes = 32;            // esp_image_header_t (24) + segment header (8)
bool ota_size_is_valid(size_t content_len, size_t partition_size);
bool ota_header_is_valid(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// On-device activity / decision event log (GET /api/v1/events)
// ---------------------------------------------------------------------------
enum class ActivityEventType : uint8_t {
  BOOT = 0,
  SPEED_CHANGE = 1,
  MODE_CHANGE = 2,
  AG_ALARM = 3,
  AG_RECOVERED = 4,
  WATCHDOG_CAP = 5,
};

enum class ActivityTrigger : uint8_t {
  NONE = 0,
  PM25 = 1,
  GAS = 2,
  MANUAL = 3,
  WATCHDOG = 4,
};

const char* activity_event_type_to_str(ActivityEventType t);
const char* activity_trigger_to_str(ActivityTrigger t);

struct ActivityEvent {
  uint32_t seq = 0;         // assigned by EventLog::push (1, 2, ...)
  uint32_t uptime_s = 0;    // seconds since boot
  ActivityEventType type = ActivityEventType::BOOT;
  int8_t from_speed = -1;   // -1 if unknown / N/A
  uint8_t to_speed = 0;     // 0..6
  uint8_t mode = 0;         // OperatingMode (0=auto, 1=manual, 2=night_force)
  ActivityTrigger trigger = ActivityTrigger::NONE;
  int16_t pm25_x10 = -1;    // round(pm25_sma5 * 10), -1 if unavailable
  int16_t tvoc_x10 = -1;    // round(tvoc_sma5 * 10), -1 if unavailable
  int16_t nox_x10 = -1;     // round(nox_sma5 * 10), -1 if unavailable
};

class EventLog {
 public:
  static constexpr size_t kCapacity = 24;

  void clear();
  uint32_t push(const ActivityEvent& ev);
  size_t count() const { return count_; }
  uint32_t total_pushed() const { return next_seq_; }
  // Index 0 = oldest retained event, count() - 1 = newest event.
  const ActivityEvent& at_oldest(size_t i) const;

 private:
  ActivityEvent buf_[kCapacity] = {};
  size_t head_ = 0;        // next write slot
  size_t count_ = 0;       // valid items (0..kCapacity)
  uint32_t next_seq_ = 0;  // total events pushed
};

// Serializes events in newest-first order into `out` (returns bytes written, or 0 on overflow).
size_t events_to_json(const EventLog& log, uint32_t current_uptime_s, char* out, size_t out_cap);

enum class BridgeCmdType : uint8_t {
  SET_AUTO,
  SET_MANUAL,        // speed
  SET_MANUAL_RESUME, // enter MANUAL using current/last non-zero speed
  SET_NIGHT_FORCE,
  TEST_IR,           // speed
  REBOOT,
  CONFIG_PATCHED,    // non-mode fields already committed to the shared config; the loop
                     // applies effects, applies the requested mode, and persists
};

struct BridgeCmd {
  BridgeCmdType type;
  uint8_t speed;      // SET_MANUAL / TEST_IR; CONFIG_PATCHED: requested manual_speed
  uint32_t cfg_mask;  // CONFIG_PATCHED
  uint8_t mode;       // CONFIG_PATCHED with CFG_CHANGED_MODE: requested OperatingMode
};

// Parses and strictly validates POST /api/v1/command JSON using caller-supplied JsonArena buffer.
bool parse_command_json(const char* json, size_t len, uint8_t* arena_buf, size_t arena_size,
                        BridgeCmd* out, const char** err);

#ifndef UNIT_TEST
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>

#include "api_auth.h"

// Live status published by the main loop (copied under a mutex for readers).
struct StatusSnapshot {
  int current_speed = 0;
  OperatingMode operating_mode = OperatingMode::AUTO;
  int pm_target_speed = 0;
  int gas_target_speed = 0;
  const char* dominant_trigger = "none";
  float current_airflow_m3h = 0;
  float effective_ach = 0;
  float room_volume_m3 = 150.0f;
  float rho_volume_factor = 1.0f;
  int dwell_stepdown_mins = 0;
  int min_stepdown_hold_mins = 4;
  bool is_night_mode = false;
  const char* clock_sync_status = "unsynced_fallback_day";
  char local_time_iso[32] = "";
  const char* ag_status = "online";
  bool alarm_sensor_unreachable = false;
  char ag_resolved_ip[16] = "";
  const char* ag_source = "none";
  int ag_fail_count = 0;
  bool ag_data_stale = false;     // monitor `boot` counter frozen for >= 3 polls
  bool have_reading = false;
  bool api_paired = false;        // a REST API token exists (never the token itself)
  bool api_pairing_active = false;
  // telemetry (raw + SMA-5); NaN = unavailable
  float pm25_raw = NAN, pm25_sma5 = NAN;
  float tvoc_raw = NAN, tvoc_sma5 = NAN;
  float nox_raw = NAN, nox_sma5 = NAN;
  float co2_ppm = NAN, temperature_c = NAN, humidity_pct = NAN;
  bool matter_commissioned = false;
  int matter_fabrics_count = 0;
  uint32_t speed_changes_today = 0;
  int wifi_rssi_dbm = 0;
  uint32_t free_heap_bytes = 0;
  uint32_t min_free_heap_bytes = 0;
  uint32_t uptime_s = 0;
  uint32_t ir_frames_sent = 0;
};

class MatterHaBridge {
 public:
  // cfg/cfg_mutex are owned by main; cmd_queue receives BridgeCmd.
  bool begin(AppConfig* cfg, SemaphoreHandle_t cfg_mutex, QueueHandle_t cmd_queue);

  // Starts the REST server (call once the network stack is up; idempotent).
  bool start_http();
  bool http_started() const { return http_started_.load(); }
  bool commissioning_active() const { return commissioning_active_.load(); }
  bool ble_released() const { return ble_released_.load(); }
  void request_reboot() { reboot_requested_.store(true); }
  bool take_reboot_request() { return reboot_requested_.exchange(false); }
  // Registers atoms3-iaq.local (+ _http._tcp) as an mDNS delegate host so the
  // Matter stack's own hostname/records stay untouched. Call after GOT_IP.
  void update_mdns(uint32_t ipv4);

  // Main loop: publish status to REST readers and Matter attributes.
  void publish(const StatusSnapshot& s);
  void schedule_publish();

  bool commissioned() const;   // cached, updated in the CHIP context
  int fabric_count() const;    // cached, updated in the CHIP context
  // Tier-3 clock source: UTCTime of the Time Synchronization cluster, if present.
  bool matter_utc(time_t* utc) const;

  // Long-press factory reset (erases fabrics, reboots into BLE commissioning).
  void decommission();

  // ---- REST authentication (SRS §5.3); thread-safe (loop + httpd) ----------
  void auth_set_active(const char* token);      // boot: token loaded from NVS
  void auth_open_pairing(uint32_t now_ms);      // button hold 3–10 s released
  bool auth_pairing_open(uint32_t now_ms);      // LED: pairing pattern
  bool auth_has_token();                        // status: api_paired
  // Loop: if a pairing hasn't been persisted yet, copies the token + its generation.
  bool auth_has_unsaved() const { return auth_unsaved_.load(); }
  bool auth_unsaved_token(char* out33, uint32_t* generation);
  void auth_mark_saved(uint32_t generation);    // after a successful NVS write
  void auth_clear();                            // factory reset
  // httpd: returns 0 if authorized, else sends the 401/429 response and returns its status.
  int auth_require(void* httpd_req);
  // httpd: POST /api/v1/pair; true if paired (token copied to out33).
  bool auth_try_pair(char* out33);
  // Device IPv4 (lwIP byte order) for the pairing Host check; 0 until GOT_IP.
  uint32_t device_ipv4() const { return device_ip_.load(); }

  // REST snapshot access (used by HTTP handlers).
  void copy_status(StatusSnapshot* out) const;
  void log_event(const ActivityEvent& ev);
  void copy_events(EventLog* out) const;
  AppConfig* config() { return cfg_; }
  SemaphoreHandle_t config_mutex() { return cfg_mutex_; }
  QueueHandle_t queue() { return cmd_q_; }

 private:
  void publish_matter(const StatusSnapshot& s);   // runs in the CHIP task only
  static void publish_work(intptr_t self);

  AppConfig* cfg_ = nullptr;
  SemaphoreHandle_t cfg_mutex_ = nullptr;
  SemaphoreHandle_t status_mutex_ = nullptr;
  SemaphoreHandle_t auth_mutex_ = nullptr;
  auth::ApiAuth auth_;
  uint32_t auth_saved_gen_ = 0;   // guarded by auth_mutex_
  QueueHandle_t cmd_q_ = nullptr;
  StatusSnapshot status_;
  StatusSnapshot matter_pending_;
  EventLog events_;  // guarded by status_mutex_
  bool started_ = false;
  std::atomic<bool> http_started_{false};
  std::atomic<bool> commissioning_active_{false};
  std::atomic<bool> ble_released_{false};
  std::atomic<bool> reboot_requested_{false};
  std::atomic<bool> auth_unsaved_{false};
  void* httpd_ = nullptr;
  uint32_t mdns_ip_ = 0;
  std::atomic<uint32_t> device_ip_{0};
  bool mdns_registered_ = false;

  // last values pushed to Matter (dedup)
  int pub_speed_ = -1;
  int pub_mode_ = -1;
  bool pub_fault_ = false;
  bool pub_fault_valid_ = false;
  float pub_pm_ = -1, pub_co2_ = -1, pub_temp_ = -1000, pub_rh_ = -1;
  int pub_aq_ = -1, pub_tvoc_lvl_ = -1, pub_no2_lvl_ = -1;
};
#endif  // UNIT_TEST
