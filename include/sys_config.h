// sys_config.h: pins, operating modes, persistent application configuration.
//
// Requirements: docs/main_requirement_spec.md (SRS v2.0) §1.2, §2.2, §5.2.
// The pure part (defaults, validation, JSON <-> struct) compiles on the host
// (UNIT_TEST) so it can be unit tested; NVS/LittleFS persistence is device-only.
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Hardware (M5Stack AtomS3 Lite, verified against M5Unified board tables)
// ---------------------------------------------------------------------------
namespace pins {
constexpr int kIrTx = 4;      // internal 940 nm IR LED, RMT TX, active-high
constexpr int kRgbLed = 35;   // WS2812C-2020
constexpr int kButton = 41;   // tactile button, active-low
constexpr int kGroveG1 = 1;   // reserved: optional external IR unit
constexpr int kGroveG2 = 2;
}  // namespace pins

// ---------------------------------------------------------------------------
// Operating modes (SRS §3.3)
// ---------------------------------------------------------------------------
enum class OperatingMode : uint8_t { AUTO = 0, MANUAL = 1, NIGHT_FORCE = 2 };

const char* operating_mode_to_str(OperatingMode m);
bool operating_mode_from_str(const char* s, OperatingMode* out);

// ---------------------------------------------------------------------------
// Persistent configuration (SRS §5.2 schema)
// ---------------------------------------------------------------------------
constexpr int kNumSpeeds = 7;  // 0 (off) .. 6

struct AppConfig {
  OperatingMode operating_mode;
  uint8_t manual_speed;            // 0..6
  uint16_t manual_timeout_mins;    // 0 = indefinite
  float room_volume_m3;            // 30..500
  char ag_serial[24];              // AirGradient serial (lowercase hex)
  char ag_target_host[64];         // optional static IP / hostname override
  uint16_t poll_interval_sec;      // 60 (allowed 30..600)
  bool enable_multigas;
  char tz_posix[48];               // e.g. "SGT-8"
  uint8_t day_start_hour;          // 0..23
  uint8_t night_start_hour;        // 0..23, != day_start_hour
  bool night_led_enabled;          // if true, P5..P8 stay lit at night (day brightness x 0.25)
  uint16_t ir_nec_address;         // 16-bit NEC address word, low byte transmitted first
  uint16_t ir_codes[kNumSpeeds];   // 16-bit NEC command words (inverse<<8 | command)
  bool wake_require_speed1_prefix;
  uint16_t ir_refresh_mins;        // 0 = disabled
};

// Default IR address word. NOTE: the SRS lists "0x00FF", but the verified IQAir
// GC MultiGas frames (Flipper-IRDB IQAir_GC_Series.ir + user table, see
// .agents/skills/ir-code-capture/references/iqair_gc_multigas.md) use NEC address
// byte 0x00 followed by its inverse 0xFF. With the same low-byte-first convention
// the command words use (0xE817 -> 0x17 then 0xE8), that address word is 0xFF00.
constexpr uint16_t kDefaultIrAddress = 0xFF00;
constexpr uint16_t kDefaultIrCodes[kNumSpeeds] = {0xFF00, 0xE817, 0xFC03, 0xFD02,
                                                  0xFA05, 0xFB04, 0xE916};

void config_set_defaults(AppConfig* cfg);

// Validates a complete config. On failure writes a human-readable reason.
bool config_validate(const AppConfig& cfg, char* err, size_t err_len);

// True if `word` is a well-formed NEC 8-bit value + inverse ((~lo & 0xFF) == hi).
bool nec_word_is_valid(uint16_t word);

// 0x00FF is the SRS's byte-swapped form of the verified address word 0xFF00
// (it would transmit address 0xFF). Used to warn at boot / on PATCH.
bool ir_address_looks_swapped(uint16_t addr);

// JSON (ArduinoJson v7). Serialization writes the SRS §5.2 schema.
// Returns bytes written (0 on overflow).
size_t config_to_json(const AppConfig& cfg, char* out, size_t out_len);

// Applies a partial JSON object (PATCH semantics) onto `cfg`. Unknown keys are
// rejected. The result is validated as a whole; `cfg` is only modified if the
// patch is fully valid. `changed_mask` reports which groups changed.
enum ConfigChange : uint32_t {
  CFG_CHANGED_NONE = 0,
  CFG_CHANGED_MODE = 1u << 0,            // operating_mode
  CFG_CHANGED_VOLUME = 1u << 1,          // room_volume_m3 -> recompute thresholds
  CFG_CHANGED_AG = 1u << 2,              // ag_serial / ag_target_host / poll interval
  CFG_CHANGED_ALGO = 1u << 3,            // enable_multigas / schedule hours
  CFG_CHANGED_TZ = 1u << 4,              // tz_posix
  CFG_CHANGED_LED = 1u << 5,             // night_led_enabled
  CFG_CHANGED_IR = 1u << 6,              // ir_* / wake prefix / refresh
  CFG_CHANGED_MANUAL_SPEED = 1u << 7,    // manual_speed
  CFG_CHANGED_MANUAL_TIMEOUT = 1u << 8,  // manual_timeout_mins
};
bool config_apply_json_patch(AppConfig* cfg, const char* json, size_t len, uint32_t* changed_mask,
                             char* err, size_t err_len);

// Parses "0x1234" / "1234" (hex) / decimal JSON numbers into uint16.
bool parse_u16_hex(const char* s, uint16_t* out);

#ifndef UNIT_TEST
// ---------------------------------------------------------------------------
// Persistence (device only): NVS (primary) + LittleFS /config.json (backup and
// first-boot defaults from data/config.json).
// ---------------------------------------------------------------------------
bool config_storage_begin();                 // mounts LittleFS, opens NVS namespace
bool config_load(AppConfig* cfg);            // NVS -> /config.json -> compiled defaults
bool config_save(const AppConfig& cfg);      // writes NVS + /config.json (only if changed)

// Small NVS helpers for runtime state that is not part of the user config.
bool state_load_ag_cached_ip(uint32_t* ipv4);
bool state_save_ag_cached_ip(uint32_t ipv4);

// REST API bearer token (SRS §5.3). Kept in its own NVS namespace, never in the
// config JSON, never logged. `out` must hold 33 chars.
bool state_load_api_token(char* out, size_t out_len);
bool state_save_api_token(const char* token);
bool state_erase_api_token();
#endif

// ---------------------------------------------------------------------------
// JsonArena: ArduinoJson v7 allocator backed by a caller-owned static buffer.
// Bump allocation; the arena is reset when the document is destroyed. No heap
// is touched, so periodic JSON work cannot fragment the heap. One arena per
// task (not thread-safe by design). The backing buffer must be alignas(8).
// ---------------------------------------------------------------------------
#include <ArduinoJson.h>
#include <string.h>

class JsonArena : public ArduinoJson::Allocator {
 public:
  JsonArena(uint8_t* buf, size_t size) : buf_(buf), size_(size) {}
  void reset() { used_ = 0; last_ = nullptr; }
  size_t used() const { return used_; }
  size_t peak() const { return peak_; }

  void* allocate(size_t n) override {
    n = align(n);
    if (used_ + kHdr + n > size_) return nullptr;
    uint8_t* p = buf_ + used_;
    store_size(p, n);
    used_ += kHdr + n;
    if (used_ > peak_) peak_ = used_;
    last_ = p + kHdr;
    return last_;
  }
  void deallocate(void* ptr) override {
    if (ptr != nullptr && ptr == last_) {  // roll back the most recent block
      used_ = static_cast<size_t>(static_cast<uint8_t*>(ptr) - kHdr - buf_);
      last_ = nullptr;
    }
  }
  void* reallocate(void* ptr, size_t n) override {
    if (ptr == nullptr) return allocate(n);
    n = align(n);
    uint8_t* p = static_cast<uint8_t*>(ptr);
    const size_t old = load_size(p - kHdr);
    if (ptr == last_) {  // grow / shrink in place
      const size_t base = static_cast<size_t>(p - buf_);
      if (base + n > size_) return nullptr;
      store_size(p - kHdr, n);
      used_ = base + n;
      if (used_ > peak_) peak_ = used_;
      return ptr;
    }
    if (n <= old) return ptr;
    void* q = allocate(n);
    if (q != nullptr) memcpy(q, ptr, old);
    return q;
  }

 private:
  static constexpr size_t kHdr = 8;  // keeps payloads 8-byte aligned (buffer must be alignas(8))
  static size_t align(size_t n) { return (n + 7u) & ~static_cast<size_t>(7u); }
  static void store_size(uint8_t* p, size_t n) { memcpy(p, &n, sizeof(n)); }
  static size_t load_size(const uint8_t* p) {
    size_t n;
    memcpy(&n, p, sizeof(n));
    return n;
  }
  uint8_t* buf_;
  size_t size_;
  size_t used_ = 0;
  size_t peak_ = 0;
  void* last_ = nullptr;
};
