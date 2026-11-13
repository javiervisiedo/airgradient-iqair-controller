// ag_client.h: AirGradient ONE local API client (SRS §2.2, §3.1, prompt §5).
// GET http://<ip>/measures/current with a 4-step discovery cascade:
//   1 ag_target_host  2 cached IP (RAM/NVS, 2.5 s)  3 mDNS airgradient_<serial>
//   4 mDNS service scan (_airgradient._tcp, then _http._tcp) + JSON signature check
// The JSON parser is pure (host-testable). Networking is device-only and runs in
// the network task (blocking calls with timeouts are allowed there).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ag {

struct Measurement {
  bool pm25_valid = false;  float pm25 = 0;        // pm02Compensated, fallback pm02
  bool tvoc_valid = false;  float tvoc_index = 0;  // tvocIndex 0..500
  bool nox_valid = false;   float nox_index = 0;   // noxIndex 0..500
  bool co2_valid = false;   float co2_ppm = 0;     // rco2
  bool temp_valid = false;  float temp_c = 0;      // atmp
  bool rh_valid = false;    float rh_pct = 0;      // rhum
  bool boot_valid = false;  uint32_t boot = 0;     // measurement-cycle counter
  char serialno[24] = "";
};

enum class ParseResult : uint8_t { OK, BAD_JSON, SERIAL_MISMATCH, NO_PM25 };
const char* parse_result_to_str(ParseResult r);

// expected_serial may be empty (no signature check). A reading without a valid
// PM2.5 value is NO_PM25 (counts as a failed poll for the watchdog).
ParseResult parse_measures(const char* json, size_t len, const char* expected_serial, Measurement* out);

// The monitor's `boot` counter (advances every measurement cycle) did not
// advance between polls (SRS §3.1 rule 5):
//   - stale():  3 consecutive unchanged readings -> warning (ag_data_stale), data still used;
//   - frozen(): from the 4th consecutive unchanged reading on -> the reading counts
//               as a failed poll, so the network watchdog caps apply.
class StaleDetector {
 public:
  static constexpr int kStalePolls = 3;
  static constexpr int kFrozenPolls = kStalePolls + 1;
  // Returns true while stale (>= kStalePolls consecutive unchanged readings).
  bool update(const Measurement& m);
  bool stale() const { return unchanged_ >= kStalePolls; }
  bool frozen() const { return unchanged_ >= kFrozenPolls; }
  int unchanged() const { return unchanged_; }

 private:
  bool have_ = false;
  uint32_t last_boot_ = 0;
  int unchanged_ = 0;
};

}  // namespace ag

#ifndef UNIT_TEST
struct AppConfig;

class AgClient {
 public:
  enum class Source : uint8_t { NONE, TARGET_HOST, CACHED_IP, MDNS_HOST, MDNS_SCAN };

  void begin();
  // One poll (blocking, bounded by per-step timeouts). Returns true on HTTP 200
  // with a parseable payload that contains a valid PM2.5 value.
  bool poll(const AppConfig& cfg, ag::Measurement* out);
  void invalidate_cache();  // e.g. after ag_serial / ag_target_host changes

  uint32_t resolved_ip() const { return resolved_ip_; }   // network byte order (lwIP)
  Source last_source() const { return source_; }
  static const char* source_to_str(Source s);
  int last_http_status() const { return last_http_; }
  ag::ParseResult last_parse() const { return last_parse_; }

 private:
  bool fetch(const char* host, uint32_t timeout_ms, const char* serial, ag::Measurement* out,
             const char* host_header = nullptr);
  bool try_ip(uint32_t ip, uint32_t timeout_ms, const char* serial, ag::Measurement* out);
  void remember(uint32_t ip);

  uint32_t resolved_ip_ = 0;
  uint32_t cached_ip_ = 0;
  Source source_ = Source::NONE;
  int last_http_ = 0;
  ag::ParseResult last_parse_ = ag::ParseResult::OK;
};
#endif
