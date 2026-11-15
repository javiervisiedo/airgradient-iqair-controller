// iaq_algorithm.h: v3.0 Dual-Path (PM2.5 + TVOC/NOx) hysteresis controller,
// dynamic room-volume scaling, sensor-fault state machine with the network
// watchdog speed caps, operating modes, and the orchestrating control core.
//
// Pure C++17 (no Arduino / ESP-IDF includes) so it is unit tested on the host.
// Requirements:
//   docs/automated_iaq_multigas_fan_controller_spec_v3.md  §3.1 (watchdog), §4 (algorithm)
//   docs/main_requirement_spec.md (SRS v2.1) §2.4 (volume), §3.1 (fault), §3.3 (modes)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sys_config.h"

namespace iaq {

constexpr int kMaxSpeed = 6;
constexpr int kSmaWindow = 5;                    // SMA-5 (spec v3 §4.1)
constexpr int kUnreachableAlarmPolls = 5;        // SRS §3.1: alarm/notification only, no IR action
// Network watchdog caps (spec v3 §3.1, SRS §3.1), in consecutive failed polls.
constexpr int kWatchdogNightCapPolls = 3;        // night: cap at once after 3 failed polls
constexpr int kWatchdogNightCapSpeed = 1;
constexpr int kWatchdogDayCapPolls = 15;         // day: hold, then cap after 15 failed polls
constexpr int kWatchdogDayCapSpeed = 3;
constexpr float kDefaultVolumeM3 = 150.0f;
constexpr float kMinVolumeM3 = 30.0f;
constexpr float kMaxVolumeM3 = 500.0f;

// IQAir GC MultiGas airflow per speed (spec v3 §2.2, SRS §2.4).
constexpr float kAirflowM3h[kNumSpeeds] = {0.0f, 80.0f, 150.0f, 240.0f, 310.0f, 380.0f, 510.0f};

// ---------------------------------------------------------------------------
// Dynamic room-volume scaling (SRS §2.4)
// ---------------------------------------------------------------------------
struct VolumeScaling {
  float volume_m3;
  float rho;                 // clamp(sqrt(150/V), 0.65, 1.35)
  int min_stepdown_hold;     // clamp(round(4 * V/150), 2, 8) minutes (ticks)
  float day_up[kMaxSpeed];
  float day_down[kMaxSpeed];
};

VolumeScaling compute_volume_scaling(float volume_m3);

// Fixed night thresholds (spec v3 §4.2; not volume scaled per SRS §2.4 item 4).
extern const float kNightUp[kMaxSpeed];
extern const float kNightDown[kMaxSpeed];

float effective_ach(int speed, float volume_m3);

// Day/night resolution (SRS §2.3 tier 4, §3.3).
// night_force -> night; clock not valid -> day fallback; else schedule.
bool is_night_hour(int local_hour, int day_start_hour, int night_start_hour);
bool resolve_is_night(bool clock_valid, int local_hour, bool night_force, int day_start_hour,
                      int night_start_hour);

// ---------------------------------------------------------------------------
// Tick-aged sample window: keeps only samples from the last kSmaWindow ticks
// ("N <= 5 valid samples from the last 5 minutes", spec v3 §4.1).
// ---------------------------------------------------------------------------
class TickWindow {
 public:
  void clear();
  void push(float value, uint32_t tick);
  // Mean of samples with tick > now_tick - kSmaWindow. Returns false if empty.
  bool mean(uint32_t now_tick, float* out) const;
  int count(uint32_t now_tick) const;

 private:
  float values_[kSmaWindow] = {};
  uint32_t ticks_[kSmaWindow] = {};
  bool used_[kSmaWindow] = {};
  int next_ = 0;
};

// ---------------------------------------------------------------------------
// Dual-path controller (spec v3 §4.2–§4.4, SRS §2.4 scaling)
// ---------------------------------------------------------------------------
struct Reading {
  bool pm25_valid = false;
  float pm25 = 0.0f;
  bool tvoc_valid = false;
  float tvoc_index = 0.0f;
  bool nox_valid = false;
  float nox_index = 0.0f;
};

enum class Trigger : uint8_t { NONE, PM25, GAS };
const char* trigger_to_str(Trigger t);

class DualPathController {
 public:
  DualPathController();

  void set_volume(float volume_m3);
  const VolumeScaling& scaling() const { return scaling_; }

  // One 60 s tick with a valid poll. Returns the new algorithm speed (0..6).
  int update(const Reading& r, bool is_night, bool enable_multigas);
  // One 60 s tick without a valid poll: ages the windows, keeps all state.
  void missed_tick();

  // Re-anchors the commanded speed (e.g. after a watchdog cap or when
  // returning from MANUAL to AUTO) so attack/decay continue from reality.
  void sync_current_speed(int speed);

  int current_speed() const { return current_speed_; }
  int pm_speed() const { return pm_speed_; }
  int gas_speed() const { return gas_speed_; }
  int dwell_minutes() const { return dwell_minutes_; }
  Trigger dominant_trigger() const;

  bool pm_avg(float* out) const { return pm_win_.mean(tick_, out); }
  bool tvoc_avg(float* out) const { return tvoc_win_.mean(tick_, out); }
  bool nox_avg(float* out) const { return nox_win_.mean(tick_, out); }
  uint32_t tick() const { return tick_; }

 private:
  VolumeScaling scaling_;
  TickWindow pm_win_, tvoc_win_, nox_win_;
  uint32_t tick_ = 0;
  int current_speed_ = 0;
  int pm_speed_ = 0;
  int gas_speed_ = 0;
  int dwell_minutes_ = 0;
};

// ---------------------------------------------------------------------------
// AirGradient reachability state machine (SRS §3.1)
// ---------------------------------------------------------------------------
enum class AgStatus : uint8_t { ONLINE, DEGRADED, UNREACHABLE_ALARM };
const char* ag_status_to_str(AgStatus s);

class SensorFaultMonitor {
 public:
  enum class Event : uint8_t { NONE, DEGRADED, ENTERED_ALARM, RECOVERED };
  Event on_success();
  Event on_failure();
  AgStatus status() const { return status_; }
  int fail_count() const { return fail_count_; }
  bool alarm() const { return status_ == AgStatus::UNREACHABLE_ALARM; }

 private:
  AgStatus status_ = AgStatus::ONLINE;
  int fail_count_ = 0;
};

// ---------------------------------------------------------------------------
// Operating mode controller (SRS §3.3)
// ---------------------------------------------------------------------------
class ModeController {
 public:
  void begin(OperatingMode mode, uint8_t manual_speed, uint16_t manual_timeout_mins, uint32_t now_ms);

  // External requests (Matter, REST, button). Return true if state changed.
  bool set_auto(uint32_t now_ms);
  bool set_manual(uint8_t speed, uint32_t now_ms);
  void set_manual_speed(uint8_t speed);  // updates stored preset without changing mode or timer
  void record_nonzero_speed(int speed);
  uint8_t last_nonzero_speed() const { return last_nonzero_speed_; }
  uint8_t resume_manual_speed(int current_commanded_speed) const;
  bool set_manual_resume(int current_commanded_speed, uint32_t now_ms);
  bool set_night_force(bool on, uint32_t now_ms);
  void set_manual_timeout(uint16_t mins) { manual_timeout_mins_ = mins; }

  // Button short press: AUTO -> M1 -> M2 -> M3 -> M0 -> AUTO (NIGHT_FORCE -> M1).
  void button_cycle(uint32_t now_ms);

  // Expires a timed manual override. Returns true if it reverted to AUTO.
  bool poll_timeout(uint32_t now_ms);

  OperatingMode mode() const { return mode_; }
  uint8_t manual_speed() const { return manual_speed_; }
  bool night_force() const { return mode_ == OperatingMode::NIGHT_FORCE; }

 private:
  OperatingMode mode_ = OperatingMode::AUTO;
  uint8_t manual_speed_ = 0;
  uint8_t last_nonzero_speed_ = 1;
  uint16_t manual_timeout_mins_ = 0;
  uint32_t manual_since_ms_ = 0;
};

// Network watchdog cap (spec v3 §3.1): highest speed allowed after `fail_count`
// consecutive failed polls. Never raises the speed.
int watchdog_cap(int speed, int fail_count, bool is_night);

// Matter fan percent <-> discrete speed (6 steps of 16.67 %).
uint8_t speed_to_percent(int speed);
int percent_to_speed(uint8_t percent);

// ---------------------------------------------------------------------------
// Control core: one call per poll tick, returns the speed to command.
// ---------------------------------------------------------------------------
struct TickResult {
  int commanded_speed;        // speed the purifier should be at after this tick
  bool speed_changed;         // commanded_speed differs from previous tick
  bool force_transmit;        // send even if unchanged (first command, recovery)
  SensorFaultMonitor::Event fault_event;
};

class ControlCore {
 public:
  TickResult begin(const AppConfig& cfg, uint32_t now_ms);
  void apply_config(const AppConfig& cfg);   // volume, multigas, schedule

  ModeController& modes() { return modes_; }
  const ModeController& modes() const { return modes_; }
  DualPathController& algo() { return algo_; }
  const DualPathController& algo() const { return algo_; }
  const SensorFaultMonitor& fault() const { return fault_; }

  // Poll tick: `ok` = HTTP 200 with a valid PM2.5 reading. `is_night` is the
  // resolved diurnal mode (night force and unsynced-clock day fallback applied).
  TickResult on_poll(bool ok, const Reading& r, bool is_night, uint32_t now_ms);

  // Mode changes between polls (Matter / REST / button) take effect at once.
  // `is_night` must be resolved with the new mode. Returns the speed to command now.
  TickResult on_mode_change(uint32_t now_ms, bool is_night);

  int commanded_speed() const { return commanded_speed_; }
  uint32_t speed_changes_today() const { return speed_changes_today_; }
  void reset_daily_counters() { speed_changes_today_ = 0; }
  const Reading& last_reading() const { return last_reading_; }

 private:
  int target_for_mode() const;
  void set_commanded(int speed, TickResult* res);
  void anchor_algo_to_cap();

  ModeController modes_;
  DualPathController algo_;
  SensorFaultMonitor fault_;
  bool enable_multigas_ = true;
  int commanded_speed_ = 0;
  bool have_commanded_ = false;
  bool algo_has_data_ = false;
  bool is_night_ = false;
  OperatingMode last_mode_ = OperatingMode::AUTO;
  uint32_t speed_changes_today_ = 0;
  Reading last_reading_;
};

}  // namespace iaq
