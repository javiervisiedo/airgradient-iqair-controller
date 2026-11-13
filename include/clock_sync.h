// clock_sync.h: 4-tier resilient clock synchronization (SRS §2.3, prompt §4).
//   Tier 1  DHCP option 42 NTP server (esp_sntp_servermode_dhcp before Wi-Fi)
//   Tier 2  SNTP: [DHCP server | gateway IP], pool.ntp.org, time.google.com
//   Tier 3  after 30 s without SNTP: Matter Time Sync UTCTime, then HTTP Date
//           header from http://<gateway>/ and http://google.com/
//   Tier 4  RTC retention across warm reboots; cold boot without time ->
//           "unsynced_fallback_day" (Day Mode unless NIGHT_FORCE)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <atomic>

namespace clk {

constexpr time_t kValidEpoch = 1700000000;  // 2023-11-14; anything earlier = no clock

enum class Status : uint8_t {
  UNSYNCED_FALLBACK_DAY,
  RTC_RETAINED,
  SYNCED_DHCP_NTP,
  SYNCED_GATEWAY_NTP,   // gateway IP or public pool (lwIP doesn't report which server answered)
  SYNCED_MATTER,
  SYNCED_HTTP_DATE,
};
const char* status_to_str(Status s);

// Parses an RFC 7231 IMF-fixdate ("Sun, 09 Nov 2026 06:30:00 GMT") to UTC epoch.
bool parse_http_date(const char* s, time_t* out);

// Matter epoch (2000-01-01T00:00:00Z) in microseconds -> Unix seconds.
inline time_t matter_utc_us_to_unix(uint64_t us) {
  return static_cast<time_t>(us / 1000000ULL) + static_cast<time_t>(946684800);
}

}  // namespace clk

#ifndef UNIT_TEST
#include <functional>

// Bounded IPv4 DNS resolution (avoids lwIP getaddrinfo 15+ s blackhole stalls).
bool net_resolve_ipv4_bounded(const char* host, uint32_t timeout_ms, uint32_t* out_ipv4);

class ClockSync {
 public:
  using MatterTimeSource = std::function<bool(time_t* utc)>;

  // Call before WiFi.begin(): sets TZ, enables DHCP option 42 (tier 1),
  // detects RTC-retained time (tier 4).
  void begin(const char* tz_posix);
  void set_timezone(const char* tz_posix);
  void set_matter_time_source(MatterTimeSource src) { matter_src_ = src; }

  // Wi-Fi events (call from the network task / event handler).
  void on_got_ip(uint32_t gateway_ipv4, uint32_t now_ms);
  void on_disconnected();

  // Call periodically from the network task (may block up to ~6 s for tier 3).
  void loop(uint32_t now_ms);

  bool valid() const;                 // wall clock usable (any tier, incl. RTC retained)
  clk::Status status() const { return status_; }
  bool local_hour(int* hour) const;   // false if no valid clock
  // ISO 8601 local time with offset, e.g. 2026-11-09T14:30:00+08:00
  bool local_time_iso(char* out, size_t len) const;

 private:
  void start_sntp(uint32_t gateway_ipv4);
  bool try_http_date(const char* url, const char* host_header);
  void set_system_time(time_t utc, clk::Status src);

  std::atomic<clk::Status> status_{clk::Status::UNSYNCED_FALLBACK_DAY};  // net task writes, loop reads
  bool have_ip_ = false;
  bool sntp_started_ = false;
  bool dhcp_ntp_present_ = false;
  uint32_t got_ip_ms_ = 0;
  uint32_t last_fallback_try_ms_ = 0;
  bool fallback_tried_ = false;
  uint32_t gateway_ = 0;
  char gateway_str_[16] = "";
  MatterTimeSource matter_src_;
};
#endif
