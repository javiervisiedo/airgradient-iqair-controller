// sys_config.cpp: defaults, validation, JSON (host + device); NVS / LittleFS
// persistence (device only).
#include "sys_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

namespace {
const char* const kCodeKeys[kNumSpeeds] = {"off", "speed_1", "speed_2", "speed_3",
                                           "speed_4", "speed_5", "speed_6"};

alignas(8) uint8_t g_cfg_json_buf[3072];  // config JSON work area (2x kMaxBody)
std::recursive_mutex g_buf_mutex;        // guards all static buffers in this file (httpd + loop tasks)

void set_err(char* err, size_t len, const char* msg) {
  if (err != nullptr && len > 0) {
    strncpy(err, msg, len - 1);
    err[len - 1] = '\0';
  }
}

bool copy_str(char* dst, size_t dst_len, const char* src) {
  if (src == nullptr) return false;
  const size_t n = strlen(src);
  if (n >= dst_len) return false;
  memcpy(dst, src, n + 1);
  return true;
}

bool is_lower_hex(const char* s) {
  if (*s == '\0') return false;
  for (; *s; ++s) {
    if (!isxdigit(static_cast<unsigned char>(*s)) || isupper(static_cast<unsigned char>(*s))) return false;
  }
  return true;
}

bool is_host_chars(const char* s) {
  for (; *s; ++s) {
    const char c = *s;
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ':')) return false;
  }
  return true;
}
}  // namespace

const char* operating_mode_to_str(OperatingMode m) {
  switch (m) {
    case OperatingMode::MANUAL: return "manual";
    case OperatingMode::NIGHT_FORCE: return "night_force";
    default: return "auto";
  }
}

bool operating_mode_from_str(const char* s, OperatingMode* out) {
  if (s == nullptr) return false;
  if (strcmp(s, "auto") == 0) { *out = OperatingMode::AUTO; return true; }
  if (strcmp(s, "manual") == 0) { *out = OperatingMode::MANUAL; return true; }
  if (strcmp(s, "night_force") == 0) { *out = OperatingMode::NIGHT_FORCE; return true; }
  return false;
}

bool ir_address_looks_swapped(uint16_t addr) { return addr == 0x00FF; }

bool nec_word_is_valid(uint16_t word) {
  return static_cast<uint8_t>(word >> 8) == static_cast<uint8_t>(~word & 0xFF);
}

bool parse_u16_hex(const char* s, uint16_t* out) {
  if (s == nullptr) return false;
  while (*s == ' ') ++s;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (*s == '\0') return false;
  for (const char* p = s; *p != '\0'; ++p) {
    if (!isxdigit(static_cast<unsigned char>(*p))) return false;
  }
  char* end = nullptr;
  const unsigned long v = strtoul(s, &end, 16);
  if (end == nullptr || *end != '\0' || v > 0xFFFFul) return false;
  *out = static_cast<uint16_t>(v);
  return true;
}

void config_set_defaults(AppConfig* c) {
  memset(c, 0, sizeof(*c));
  c->operating_mode = OperatingMode::AUTO;
  c->manual_speed = 0;
  c->manual_timeout_mins = 0;
  c->room_volume_m3 = 150.0f;
  copy_str(c->ag_serial, sizeof(c->ag_serial), "9888e0eefcec");
  c->ag_target_host[0] = '\0';
  c->poll_interval_sec = 60;
  c->enable_multigas = true;
  copy_str(c->tz_posix, sizeof(c->tz_posix), "SGT-8");
  c->day_start_hour = 8;
  c->night_start_hour = 23;
  c->night_led_enabled = false;
  c->ir_nec_address = kDefaultIrAddress;
  for (int i = 0; i < kNumSpeeds; ++i) c->ir_codes[i] = kDefaultIrCodes[i];
  c->wake_require_speed1_prefix = true;
  c->ir_refresh_mins = 15;
}

bool config_validate(const AppConfig& c, char* err, size_t err_len) {
  if (c.manual_speed > 6) { set_err(err, err_len, "manual_speed must be 0..6"); return false; }
  if (c.manual_timeout_mins > 1440) { set_err(err, err_len, "manual_timeout_mins must be 0..1440"); return false; }
  if (!(c.room_volume_m3 >= 30.0f && c.room_volume_m3 <= 500.0f)) {
    set_err(err, err_len, "room_volume_m3 must be 30.0..500.0");
    return false;
  }
  if (!is_lower_hex(c.ag_serial) || strlen(c.ag_serial) > 16) {
    set_err(err, err_len, "ag_serial must be 1..16 lowercase hex characters");
    return false;
  }
  if (!is_host_chars(c.ag_target_host)) { set_err(err, err_len, "ag_target_host has invalid characters"); return false; }
  if (c.poll_interval_sec < 30 || c.poll_interval_sec > 600) {
    set_err(err, err_len, "poll_interval_sec must be 30..600 (algorithm is tuned for 60)");
    return false;
  }
  if (c.tz_posix[0] == '\0') { set_err(err, err_len, "tz_posix must not be empty"); return false; }
  if (c.day_start_hour > 23 || c.night_start_hour > 23 || c.day_start_hour == c.night_start_hour) {
    set_err(err, err_len, "day_start_hour/night_start_hour must be 0..23 and differ");
    return false;
  }
  if (!nec_word_is_valid(c.ir_nec_address)) {
    set_err(err, err_len, "invalid ir_nec_address NEC word (high byte = ~low byte)");
    return false;
  }
  for (int i = 0; i < kNumSpeeds; ++i) {
    if (!nec_word_is_valid(c.ir_codes[i])) {
      set_err(err, err_len, "ir_codes entries must be NEC words (high byte = ~low byte)");
      return false;
    }
  }
  if (c.ir_refresh_mins > 240) { set_err(err, err_len, "ir_refresh_mins must be 0..240"); return false; }
  return true;
}

size_t config_to_json(const AppConfig& c, char* out, size_t out_len) {
  std::lock_guard<std::recursive_mutex> guard(g_buf_mutex);
  JsonArena arena(g_cfg_json_buf, sizeof(g_cfg_json_buf));
  size_t n = 0;
  {
    JsonDocument doc(&arena);
    char hex[8];
    char vol[16];
    snprintf(vol, sizeof(vol), "%.1f", static_cast<double>(c.room_volume_m3));
    doc["operating_mode"] = operating_mode_to_str(c.operating_mode);
    doc["manual_speed"] = c.manual_speed;
    doc["manual_timeout_mins"] = c.manual_timeout_mins;
    doc["room_volume_m3"] = serialized(static_cast<const char*>(vol));  // "150.0"
    doc["ag_serial"] = c.ag_serial;
    doc["ag_target_host"] = c.ag_target_host;
    doc["poll_interval_sec"] = c.poll_interval_sec;
    doc["enable_multigas"] = c.enable_multigas;
    doc["tz_posix"] = c.tz_posix;
    doc["day_start_hour"] = c.day_start_hour;
    doc["night_start_hour"] = c.night_start_hour;
    doc["night_led_enabled"] = c.night_led_enabled;
    snprintf(hex, sizeof(hex), "0x%04X", c.ir_nec_address);
    doc["ir_nec_address"] = hex;  // copied (char* -> stored as string copy)
    JsonObject codes = doc["ir_codes"].to<JsonObject>();
    for (int i = 0; i < kNumSpeeds; ++i) {
      snprintf(hex, sizeof(hex), "0x%04X", c.ir_codes[i]);
      codes[kCodeKeys[i]] = hex;
    }
    doc["wake_require_speed1_prefix"] = c.wake_require_speed1_prefix;
    doc["ir_refresh_mins"] = c.ir_refresh_mins;
    if (!doc.overflowed()) {
      n = serializeJson(doc, out, out_len);
      if (n >= out_len) n = 0;
    }
  }
  arena.reset();
  return n;
}

namespace {
bool json_u16(JsonVariantConst v, uint16_t lo, uint16_t hi, uint16_t* out) {
  if (!v.is<long>()) return false;
  const long x = v.as<long>();
  if (x < lo || x > hi) return false;
  *out = static_cast<uint16_t>(x);
  return true;
}

bool json_hex16(JsonVariantConst v, uint16_t* out) {
  if (v.is<const char*>()) return parse_u16_hex(v.as<const char*>(), out);
  if (v.is<long>()) {
    const long x = v.as<long>();
    if (x < 0 || x > 0xFFFF) return false;
    *out = static_cast<uint16_t>(x);
    return true;
  }
  return false;
}
}  // namespace

bool config_apply_json_patch(AppConfig* cfg, const char* json, size_t len, uint32_t* changed_mask,
                             char* err, size_t err_len) {
  if (changed_mask) *changed_mask = CFG_CHANGED_NONE;
  AppConfig next = *cfg;
  uint32_t mask = CFG_CHANGED_NONE;
  bool ok = true;
  char msg[96] = "";

  std::unique_lock<std::recursive_mutex> guard(g_buf_mutex);
  JsonArena arena(g_cfg_json_buf, sizeof(g_cfg_json_buf));
  {
    JsonDocument doc(&arena);
    const DeserializationError de = deserializeJson(doc, json, len);
    if (de) {
      snprintf(msg, sizeof(msg), "invalid JSON: %s", de.c_str());
      ok = false;
    } else if (!doc.is<JsonObject>()) {
      snprintf(msg, sizeof(msg), "body must be a JSON object");
      ok = false;
    } else {
      for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        JsonVariantConst v = kv.value();
        uint16_t u = 0;
        if (strcmp(k, "operating_mode") == 0) {
          OperatingMode m;
          if (!v.is<const char*>() || !operating_mode_from_str(v.as<const char*>(), &m)) { ok = false; }
          else { next.operating_mode = m; mask |= CFG_CHANGED_MODE; }
        } else if (strcmp(k, "manual_speed") == 0) {
          if (!json_u16(v, 0, 6, &u)) ok = false; else { next.manual_speed = static_cast<uint8_t>(u); mask |= CFG_CHANGED_MANUAL_SPEED; }
        } else if (strcmp(k, "manual_timeout_mins") == 0) {
          if (!json_u16(v, 0, 1440, &u)) ok = false; else { next.manual_timeout_mins = u; mask |= CFG_CHANGED_MANUAL_TIMEOUT; }
        } else if (strcmp(k, "room_volume_m3") == 0) {
          if (!v.is<float>()) ok = false; else { next.room_volume_m3 = v.as<float>(); mask |= CFG_CHANGED_VOLUME; }
        } else if (strcmp(k, "ag_serial") == 0) {
          if (!v.is<const char*>() || !copy_str(next.ag_serial, sizeof(next.ag_serial), v.as<const char*>())) ok = false;
          else mask |= CFG_CHANGED_AG;
        } else if (strcmp(k, "ag_target_host") == 0) {
          if (!v.is<const char*>() || !copy_str(next.ag_target_host, sizeof(next.ag_target_host), v.as<const char*>())) ok = false;
          else mask |= CFG_CHANGED_AG;
        } else if (strcmp(k, "poll_interval_sec") == 0) {
          if (!json_u16(v, 30, 600, &u)) ok = false; else { next.poll_interval_sec = u; mask |= CFG_CHANGED_AG; }
        } else if (strcmp(k, "enable_multigas") == 0) {
          if (!v.is<bool>()) ok = false; else { next.enable_multigas = v.as<bool>(); mask |= CFG_CHANGED_ALGO; }
        } else if (strcmp(k, "tz_posix") == 0) {
          if (!v.is<const char*>() || !copy_str(next.tz_posix, sizeof(next.tz_posix), v.as<const char*>())) ok = false;
          else mask |= CFG_CHANGED_TZ;
        } else if (strcmp(k, "day_start_hour") == 0) {
          if (!json_u16(v, 0, 23, &u)) ok = false; else { next.day_start_hour = static_cast<uint8_t>(u); mask |= CFG_CHANGED_ALGO; }
        } else if (strcmp(k, "night_start_hour") == 0) {
          if (!json_u16(v, 0, 23, &u)) ok = false; else { next.night_start_hour = static_cast<uint8_t>(u); mask |= CFG_CHANGED_ALGO; }
        } else if (strcmp(k, "night_led_enabled") == 0) {
          if (!v.is<bool>()) ok = false; else { next.night_led_enabled = v.as<bool>(); mask |= CFG_CHANGED_LED; }
        } else if (strcmp(k, "ir_nec_address") == 0) {
          if (!json_hex16(v, &u)) ok = false; else { next.ir_nec_address = u; mask |= CFG_CHANGED_IR; }
        } else if (strcmp(k, "ir_codes") == 0) {
          if (!v.is<JsonObjectConst>()) {
            ok = false;
          } else {
            for (JsonPairConst ck : v.as<JsonObjectConst>()) {
              int idx = -1;
              for (int i = 0; i < kNumSpeeds; ++i) {
                if (strcmp(ck.key().c_str(), kCodeKeys[i]) == 0) idx = i;
              }
              if (idx < 0 || !json_hex16(ck.value(), &u)) {
                snprintf(msg, sizeof(msg), "ir_codes.%s invalid", ck.key().c_str());
                ok = false;
                break;
              }
              next.ir_codes[idx] = u;
            }
            if (ok) mask |= CFG_CHANGED_IR;
          }
        } else if (strcmp(k, "wake_require_speed1_prefix") == 0) {
          if (!v.is<bool>()) ok = false; else { next.wake_require_speed1_prefix = v.as<bool>(); mask |= CFG_CHANGED_IR; }
        } else if (strcmp(k, "ir_refresh_mins") == 0) {
          if (!json_u16(v, 0, 240, &u)) ok = false; else { next.ir_refresh_mins = u; mask |= CFG_CHANGED_IR; }
        } else {
          snprintf(msg, sizeof(msg), "unknown key '%s'", k);
          ok = false;
        }
        if (!ok) {
          if (msg[0] == '\0') snprintf(msg, sizeof(msg), "invalid value for '%s'", k);
          break;
        }
      }
    }
  }
  arena.reset();
  guard.unlock();

  if (ok && !config_validate(next, msg, sizeof(msg))) ok = false;
  if (!ok) {
    set_err(err, err_len, msg);
    return false;
  }
  *cfg = next;
  if (changed_mask) *changed_mask = mask;
  return true;
}

// ===========================================================================
// Device-only persistence
// ===========================================================================
#ifndef UNIT_TEST
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_log.h>

namespace {
constexpr const char* kTag = "cfg";
constexpr const char* kNvsNs = "iaqcfg";
constexpr const char* kNvsKeyCfg = "cfg";
constexpr const char* kNvsKeyAgIp = "ag_ip";
constexpr const char* kCfgPath = "/config.json";
bool g_fs_ok = false;
char g_io_buf[1024];

bool load_from_text(AppConfig* cfg, const char* text, size_t len, const char* src) {
  AppConfig tmp;
  config_set_defaults(&tmp);
  char err[96];
  uint32_t mask;
  if (!config_apply_json_patch(&tmp, text, len, &mask, err, sizeof(err))) {
    ESP_LOGW(kTag, "ignoring %s config: %s", src, err);
    return false;
  }
  *cfg = tmp;
  ESP_LOGI(kTag, "config loaded from %s", src);
  return true;
}
}  // namespace

bool config_storage_begin() {
  g_fs_ok = LittleFS.begin(true, "/littlefs", 5, "littlefs");
  if (!g_fs_ok) ESP_LOGW(kTag, "LittleFS mount failed (partition 'littlefs'); NVS only");
  return true;
}

bool config_load(AppConfig* cfg) {
  std::lock_guard<std::recursive_mutex> guard(g_buf_mutex);
  config_set_defaults(cfg);
  Preferences p;
  if (p.begin(kNvsNs, true)) {
    const size_t n = p.getString(kNvsKeyCfg, g_io_buf, sizeof(g_io_buf));
    p.end();
    if (n > 1 && load_from_text(cfg, g_io_buf, strlen(g_io_buf), "NVS")) return true;
  }
  if (g_fs_ok && LittleFS.exists(kCfgPath)) {
    File f = LittleFS.open(kCfgPath, "r");
    if (f) {
      const size_t n = f.readBytes(g_io_buf, sizeof(g_io_buf) - 1);
      f.close();
      g_io_buf[n] = '\0';
      if (n > 1 && load_from_text(cfg, g_io_buf, n, kCfgPath)) {
        config_save(*cfg);  // seed NVS
        return true;
      }
    }
  }
  ESP_LOGI(kTag, "using compiled defaults");
  return false;
}

bool config_save(const AppConfig& cfg) {
  std::lock_guard<std::recursive_mutex> guard(g_buf_mutex);
  const size_t n = config_to_json(cfg, g_io_buf, sizeof(g_io_buf));
  if (n == 0) {
    ESP_LOGE(kTag, "config serialization overflow");
    return false;
  }
  bool ok = true;
  bool changed = true;
  Preferences p;
  if (p.begin(kNvsNs, false)) {
    // config_to_json() above has finished using g_cfg_json_buf (3072 B) under g_buf_mutex.
    char* prev_buf = reinterpret_cast<char*>(g_cfg_json_buf);
    const size_t m = p.getString(kNvsKeyCfg, prev_buf, sizeof(g_io_buf));
    changed = (m == 0 || strcmp(g_io_buf, prev_buf) != 0);
    if (changed) ok = p.putString(kNvsKeyCfg, g_io_buf) == n;  // write only on change (flash wear)
    p.end();
  } else {
    ok = false;
  }
  if (g_fs_ok && changed) {
    static constexpr const char* kCfgTmpPath = "/config.json.tmp";
    File f = LittleFS.open(kCfgTmpPath, "w");
    if (f) {
      const size_t written = f.write(reinterpret_cast<const uint8_t*>(g_io_buf), n);
      f.close();
      if (written != n || !LittleFS.rename(kCfgTmpPath, kCfgPath)) {
        ESP_LOGW(kTag, "LittleFS atomic write failed");
        LittleFS.remove(kCfgTmpPath);
      }
    } else {
      ESP_LOGW(kTag, "LittleFS open %s failed", kCfgTmpPath);
    }
  }
  if (!ok) ESP_LOGE(kTag, "NVS write failed");
  return ok;
}

bool state_load_ag_cached_ip(uint32_t* ipv4) {
  Preferences p;
  if (!p.begin(kNvsNs, true)) return false;
  const uint32_t v = p.getUInt(kNvsKeyAgIp, 0);
  p.end();
  if (v == 0) return false;
  *ipv4 = v;
  return true;
}

bool state_save_ag_cached_ip(uint32_t ipv4) {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return false;
  bool ok = true;
  if (p.getUInt(kNvsKeyAgIp, 0) != ipv4) ok = p.putUInt(kNvsKeyAgIp, ipv4) == sizeof(uint32_t);
  p.end();
  return ok;
}

namespace {
constexpr const char* kNvsAuthNs = "iaqauth";
constexpr const char* kNvsKeyToken = "token";
}  // namespace

bool state_load_api_token(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) return false;
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNvsAuthNs, true)) return false;  // namespace absent: never paired
  const size_t n = p.getString(kNvsKeyToken, out, out_len);
  p.end();
  return n > 1;
}

bool state_save_api_token(const char* token) {
  Preferences p;
  if (!p.begin(kNvsAuthNs, false)) return false;
  const bool ok = p.putString(kNvsKeyToken, token) == strlen(token);
  p.end();
  if (!ok) ESP_LOGE(kTag, "API token write failed");
  return ok;
}

bool state_erase_api_token() {
  Preferences p;
  if (!p.begin(kNvsAuthNs, false)) return false;
  const bool ok = p.clear();
  p.end();
  return ok;
}
#endif  // UNIT_TEST
