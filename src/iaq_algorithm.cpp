// iaq_algorithm.cpp: pure implementation (host + device).
#include "iaq_algorithm.h"

#include <math.h>

namespace iaq {

namespace {
constexpr float kDayUpBase[kMaxSpeed] = {5.0f, 12.0f, 20.0f, 35.0f, 55.0f, 85.0f};
constexpr float kDayDownBase[kMaxSpeed] = {3.0f, 8.5f, 14.5f, 26.0f, 42.0f, 65.0f};
constexpr int kFirstScaledIndex = 2;  // Speed 2<->3 and above are volume scaled

// Multi-gas 2-tier floor thresholds (spec v3 §4.3)
constexpr float kNightGasTvocUp = 300.0f;
constexpr float kNightGasNoxUp = 5.0f;
constexpr float kNightGasTvocDown = 230.0f;
constexpr float kNightGasNoxDown = 3.0f;
constexpr float kDayGasTier1TvocUp = 200.0f;
constexpr float kDayGasTier1NoxUp = 4.0f;
constexpr float kDayGasTier1TvocDown = 150.0f;
constexpr float kDayGasTier1NoxDown = 2.5f;
constexpr float kDayGasTier2TvocUp = 300.0f;
constexpr float kDayGasTier2NoxUp = 6.0f;
constexpr float kDayGasTier2TvocDown = 230.0f;
constexpr float kDayGasTier2NoxDown = 4.0f;

template <typename T>
T clampv(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

const float kNightUp[kMaxSpeed] = {10.0f, 15.0f, 25.0f, 35.0f, 55.0f, 100.0f};
const float kNightDown[kMaxSpeed] = {6.5f, 11.0f, 19.0f, 28.0f, 42.0f, 75.0f};

// ---------------------------------------------------------------------------
VolumeScaling compute_volume_scaling(float volume_m3) {
  VolumeScaling s{};
  if (!(volume_m3 >= kMinVolumeM3)) volume_m3 = (volume_m3 > 0.0f) ? kMinVolumeM3 : kDefaultVolumeM3;
  if (volume_m3 > kMaxVolumeM3) volume_m3 = kMaxVolumeM3;
  s.volume_m3 = volume_m3;
  s.rho = clampv(sqrtf(kDefaultVolumeM3 / volume_m3), 0.65f, 1.35f);
  s.min_stepdown_hold = clampv(static_cast<int>(lroundf(4.0f * (volume_m3 / kDefaultVolumeM3))), 2, 8);
  for (int i = 0; i < kMaxSpeed; ++i) {
    const float k = (i >= kFirstScaledIndex) ? s.rho : 1.0f;
    s.day_up[i] = kDayUpBase[i] * k;
    s.day_down[i] = kDayDownBase[i] * k;
  }
  return s;
}

float effective_ach(int speed, float volume_m3) {
  if (speed < 0 || speed > kMaxSpeed || !(volume_m3 > 0.0f)) return 0.0f;
  return kAirflowM3h[speed] / volume_m3;
}

bool is_night_hour(int h, int day_start, int night_start) {
  if (h < 0 || h > 23) return false;
  if (night_start > day_start) return h >= night_start || h < day_start;  // e.g. 23..07
  return h >= night_start && h < day_start;                               // e.g. 01..07
}

bool resolve_is_night(bool clock_valid, int local_hour, bool night_force, int day_start, int night_start) {
  if (night_force) return true;
  if (!clock_valid) return false;  // SRS §2.3 tier 4: unsynced -> Day Mode
  return is_night_hour(local_hour, day_start, night_start);
}

const char* trigger_to_str(Trigger t) {
  switch (t) {
    case Trigger::PM25: return "pm25";
    case Trigger::GAS: return "gas";
    default: return "none";
  }
}

// ---------------------------------------------------------------------------
void TickWindow::clear() {
  for (int i = 0; i < kSmaWindow; ++i) used_[i] = false;
  next_ = 0;
}

void TickWindow::push(float value, uint32_t tick) {
  values_[next_] = value;
  ticks_[next_] = tick;
  used_[next_] = true;
  next_ = (next_ + 1) % kSmaWindow;
}

bool TickWindow::mean(uint32_t now_tick, float* out) const {
  float sum = 0.0f;
  int n = 0;
  for (int i = 0; i < kSmaWindow; ++i) {
    if (used_[i] && (now_tick - ticks_[i]) < static_cast<uint32_t>(kSmaWindow)) {
      sum += values_[i];
      ++n;
    }
  }
  if (n == 0) return false;
  *out = sum / static_cast<float>(n);
  return true;
}

int TickWindow::count(uint32_t now_tick) const {
  int n = 0;
  for (int i = 0; i < kSmaWindow; ++i) {
    if (used_[i] && (now_tick - ticks_[i]) < static_cast<uint32_t>(kSmaWindow)) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
DualPathController::DualPathController() : scaling_(compute_volume_scaling(kDefaultVolumeM3)) {}

void DualPathController::set_volume(float volume_m3) { scaling_ = compute_volume_scaling(volume_m3); }

void DualPathController::missed_tick() { ++tick_; }

void DualPathController::sync_current_speed(int speed) {
  current_speed_ = clampv(speed, 0, kMaxSpeed);
  dwell_minutes_ = 0;
}

Trigger DualPathController::dominant_trigger() const {
  if (pm_speed_ == 0 && gas_speed_ == 0) return Trigger::NONE;
  return (gas_speed_ > pm_speed_) ? Trigger::GAS : Trigger::PM25;
}

int DualPathController::update(const Reading& r, bool is_night, bool enable_multigas) {
  ++tick_;
  if (r.pm25_valid && r.pm25 >= 0.0f && r.pm25 <= 1000.0f) pm_win_.push(r.pm25, tick_);
  if (r.tvoc_valid && r.tvoc_index >= 0.0f && r.tvoc_index <= 500.0f) tvoc_win_.push(r.tvoc_index, tick_);
  if (r.nox_valid && r.nox_index >= 0.0f && r.nox_index <= 500.0f) nox_win_.push(r.nox_index, tick_);

  float pm_avg_v;
  if (!pm_win_.mean(tick_, &pm_avg_v)) return current_speed_;  // no PM data yet
  float tvoc_avg_v = 0.0f, nox_avg_v = 0.0f;
  if (!tvoc_win_.mean(tick_, &tvoc_avg_v)) tvoc_avg_v = 0.0f;
  if (!nox_win_.mean(tick_, &nox_avg_v)) nox_avg_v = 0.0f;

  // Path 1: PM2.5 hysteresis (spec v3 §4.2; day arrays volume-scaled, SRS §2.4)
  const float* up_t = is_night ? kNightUp : scaling_.day_up;
  const float* down_t = is_night ? kNightDown : scaling_.day_down;
  while (pm_speed_ < kMaxSpeed && pm_avg_v > up_t[pm_speed_]) ++pm_speed_;
  while (pm_speed_ > 0 && pm_avg_v <= down_t[pm_speed_ - 1]) --pm_speed_;

  // Path 2: capped multi-gas floor (spec v3 §4.3)
  if (!enable_multigas) {
    gas_speed_ = 0;
  } else if (is_night) {
    if (gas_speed_ == 0 && (tvoc_avg_v > kNightGasTvocUp || nox_avg_v >= kNightGasNoxUp)) {
      gas_speed_ = 1;
    } else if (gas_speed_ >= 1 && (tvoc_avg_v <= kNightGasTvocDown && nox_avg_v <= kNightGasNoxDown)) {
      gas_speed_ = 0;
    }
    if (gas_speed_ > 1) gas_speed_ = 1;
  } else {
    if (gas_speed_ < 2 && (tvoc_avg_v > kDayGasTier1TvocUp || nox_avg_v >= kDayGasTier1NoxUp)) gas_speed_ = 2;
    if (gas_speed_ < 3 && (tvoc_avg_v > kDayGasTier2TvocUp || nox_avg_v >= kDayGasTier2NoxUp)) gas_speed_ = 3;
    if (gas_speed_ == 3 && (tvoc_avg_v <= kDayGasTier2TvocDown && nox_avg_v <= kDayGasTier2NoxDown)) gas_speed_ = 2;
    if ((gas_speed_ == 1 || gas_speed_ == 2) &&
        (tvoc_avg_v <= kDayGasTier1TvocDown && nox_avg_v <= kDayGasTier1NoxDown)) {
      gas_speed_ = 0;
    }
  }

  // Arbitration with asymmetric attack / decay (spec v3 §4.4, hold scaled by SRS §2.4)
  const int target = pm_speed_ > gas_speed_ ? pm_speed_ : gas_speed_;
  if (target > current_speed_) {
    current_speed_ = target;
    dwell_minutes_ = 0;
  } else if (target < current_speed_) {
    ++dwell_minutes_;
    if (dwell_minutes_ >= scaling_.min_stepdown_hold) {
      --current_speed_;
      dwell_minutes_ = 0;
    }
  } else {
    dwell_minutes_ = 0;
  }
  return current_speed_;
}

// ---------------------------------------------------------------------------
const char* ag_status_to_str(AgStatus s) {
  switch (s) {
    case AgStatus::DEGRADED: return "degraded";
    case AgStatus::UNREACHABLE_ALARM: return "unreachable_alarm";
    default: return "online";
  }
}

SensorFaultMonitor::Event SensorFaultMonitor::on_success() {
  const bool was_alarm = (status_ == AgStatus::UNREACHABLE_ALARM);
  fail_count_ = 0;
  status_ = AgStatus::ONLINE;
  return was_alarm ? Event::RECOVERED : Event::NONE;
}

SensorFaultMonitor::Event SensorFaultMonitor::on_failure() {
  if (fail_count_ < 1000) ++fail_count_;
  if (status_ == AgStatus::UNREACHABLE_ALARM) return Event::NONE;
  if (fail_count_ >= kUnreachableAlarmPolls) {
    status_ = AgStatus::UNREACHABLE_ALARM;
    return Event::ENTERED_ALARM;
  }
  status_ = AgStatus::DEGRADED;
  return Event::DEGRADED;
}

// ---------------------------------------------------------------------------
void ModeController::begin(OperatingMode mode, uint8_t manual_speed, uint16_t manual_timeout_mins,
                           uint32_t now_ms) {
  mode_ = mode;
  manual_speed_ = manual_speed > kMaxSpeed ? kMaxSpeed : manual_speed;
  manual_timeout_mins_ = manual_timeout_mins;
  manual_since_ms_ = now_ms;
}

bool ModeController::set_auto(uint32_t) {
  if (mode_ == OperatingMode::AUTO) return false;
  mode_ = OperatingMode::AUTO;
  return true;
}

bool ModeController::set_manual(uint8_t speed, uint32_t now_ms) {
  if (speed > kMaxSpeed) speed = kMaxSpeed;
  const bool changed = (mode_ != OperatingMode::MANUAL) || (manual_speed_ != speed);
  mode_ = OperatingMode::MANUAL;
  manual_speed_ = speed;
  manual_since_ms_ = now_ms;  // every manual command restarts the timeout
  return changed;
}

void ModeController::set_manual_speed(uint8_t speed) {
  manual_speed_ = speed > kMaxSpeed ? kMaxSpeed : speed;
}

bool ModeController::set_night_force(bool on, uint32_t) {
  if (on) {
    if (mode_ == OperatingMode::NIGHT_FORCE) return false;
    mode_ = OperatingMode::NIGHT_FORCE;
    return true;
  }
  if (mode_ != OperatingMode::NIGHT_FORCE) return false;
  mode_ = OperatingMode::AUTO;
  return true;
}

void ModeController::button_cycle(uint32_t now_ms) {
  if (mode_ != OperatingMode::MANUAL) {
    set_manual(1, now_ms);
    return;
  }
  switch (manual_speed_) {
    case 1: set_manual(2, now_ms); break;
    case 2: set_manual(3, now_ms); break;
    case 3: set_manual(0, now_ms); break;
    default: set_auto(now_ms); break;  // 0 -> AUTO; 4..6 (set remotely) -> AUTO
  }
}

bool ModeController::poll_timeout(uint32_t now_ms) {
  if (mode_ != OperatingMode::MANUAL || manual_timeout_mins_ == 0) return false;
  if (now_ms - manual_since_ms_ >= static_cast<uint32_t>(manual_timeout_mins_) * 60000u) {
    mode_ = OperatingMode::AUTO;
    return true;
  }
  return false;
}

int watchdog_cap(int speed, int fail_count, bool is_night) {
  if (is_night && fail_count >= kWatchdogNightCapPolls && speed > kWatchdogNightCapSpeed) {
    return kWatchdogNightCapSpeed;
  }
  if (!is_night && fail_count >= kWatchdogDayCapPolls && speed > kWatchdogDayCapSpeed) {
    return kWatchdogDayCapSpeed;
  }
  return speed;
}

uint8_t speed_to_percent(int speed) {
  if (speed <= 0) return 0;
  if (speed >= kMaxSpeed) return 100;
  return static_cast<uint8_t>(lroundf(speed * 100.0f / kMaxSpeed));
}

int percent_to_speed(uint8_t percent) {
  if (percent == 0) return 0;
  if (percent > 100) percent = 100;
  return clampv(static_cast<int>(lroundf(percent * kMaxSpeed / 100.0f)), 1, kMaxSpeed);
}

// ---------------------------------------------------------------------------
TickResult ControlCore::begin(const AppConfig& cfg, uint32_t now_ms) {
  TickResult res{0, false, false, SensorFaultMonitor::Event::NONE};
  modes_.begin(cfg.operating_mode, cfg.manual_speed, cfg.manual_timeout_mins, now_ms);
  apply_config(cfg);
  commanded_speed_ = 0;
  have_commanded_ = false;
  algo_has_data_ = false;
  last_mode_ = modes_.mode();
  // Persisted MANUAL mode represents an explicit user speed to apply immediately
  // on boot; AUTO / NIGHT_FORCE wait for a valid reading (SRS §3.1 rule 4).
  if (modes_.mode() == OperatingMode::MANUAL) {
    set_commanded(modes_.manual_speed(), &res);
  }
  return res;
}

void ControlCore::apply_config(const AppConfig& cfg) {
  algo_.set_volume(cfg.room_volume_m3);
  enable_multigas_ = cfg.enable_multigas;
  modes_.set_manual_timeout(cfg.manual_timeout_mins);
}

// Returns -1 when there is nothing to command yet (boot without any valid
// reading and nothing commanded): the purifier keeps whatever state it is in.
int ControlCore::target_for_mode() const {
  if (modes_.mode() == OperatingMode::MANUAL) return modes_.manual_speed();  // caps don't apply
  if (fault_.fail_count() > 0) {
    // Watchdog (spec v3 §3.1, SRS §3.1): hold the last commanded speed, capped.
    if (!have_commanded_) return -1;
    return watchdog_cap(commanded_speed_, fault_.fail_count(), is_night_);
  }
  if (!algo_has_data_) return have_commanded_ ? commanded_speed_ : -1;
  return algo_.current_speed();
}

void ControlCore::set_commanded(int speed, TickResult* res) {
  if (speed < 0) {  // no decision yet
    res->speed_changed = false;
    res->force_transmit = false;
    res->commanded_speed = commanded_speed_;
    return;
  }
  res->speed_changed = !have_commanded_ || speed != commanded_speed_;
  if (!have_commanded_) res->force_transmit = true;  // purifier state unknown: always send the first command
  if (res->speed_changed && have_commanded_) ++speed_changes_today_;
  commanded_speed_ = speed;
  have_commanded_ = true;
  res->commanded_speed = speed;
}

// After a watchdog cap, the algorithm continues from the capped speed when
// data returns (spec v3 §5 reference: the cap lowers current_speed itself).
void ControlCore::anchor_algo_to_cap() {
  if (modes_.mode() == OperatingMode::MANUAL || fault_.fail_count() == 0 || !have_commanded_) return;
  if (commanded_speed_ < algo_.current_speed()) algo_.sync_current_speed(commanded_speed_);
}

TickResult ControlCore::on_poll(bool ok, const Reading& r, bool is_night, uint32_t now_ms) {
  TickResult res{commanded_speed_, false, false, SensorFaultMonitor::Event::NONE};
  modes_.poll_timeout(now_ms);
  last_mode_ = modes_.mode();
  is_night_ = is_night;

  if (ok) {
    last_reading_ = r;
    const bool had_failures = fault_.fail_count() > 0;
    res.fault_event = fault_.on_success();
    algo_.update(r, is_night, enable_multigas_);
    algo_has_data_ = true;
    if (had_failures) res.force_transmit = true;  // SRS §3.1 Recovery: re-send even if unchanged
  } else {
    // The SRS alarm at kUnreachableAlarmPolls is a notification only; the
    // speed is governed by the watchdog caps in target_for_mode().
    res.fault_event = fault_.on_failure();
    algo_.missed_tick();
  }

  set_commanded(target_for_mode(), &res);
  anchor_algo_to_cap();
  return res;
}

TickResult ControlCore::on_mode_change(uint32_t now_ms, bool is_night) {
  TickResult res{commanded_speed_, false, false, SensorFaultMonitor::Event::NONE};
  modes_.poll_timeout(now_ms);
  is_night_ = is_night;
  last_mode_ = modes_.mode();
  set_commanded(target_for_mode(), &res);
  anchor_algo_to_cap();
  return res;
}

}  // namespace iaq
