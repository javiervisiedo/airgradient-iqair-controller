// clock_sync.cpp: pure helpers (host + device) and the 4-tier sync (device).
#include "clock_sync.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace clk {

const char* status_to_str(Status s) {
  switch (s) {
    case Status::RTC_RETAINED: return "rtc_retained";
    case Status::SYNCED_DHCP_NTP: return "synced_dhcp_ntp";
    case Status::SYNCED_GATEWAY_NTP: return "synced_ntp";
    case Status::SYNCED_MATTER: return "synced_matter";
    case Status::SYNCED_HTTP_DATE: return "synced_http_date";
    default: return "unsynced_fallback_day";
  }
}

namespace {
int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

bool parse_http_date(const char* s, time_t* out) {
  static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (s == nullptr) return false;
  while (*s == ' ') ++s;
  char wday[4] = {0}, mon[4] = {0}, tz[4] = {0};
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(s, "%3s %d %3s %d %d:%d:%d %3s", wday, &day, mon, &year, &hh, &mm, &ss, tz) != 8) {
    // tolerate "Sun, 09 Nov ..." where %3s captured "Sun," (comma) -> retry with explicit comma
    if (sscanf(s, "%3[A-Za-z], %d %3s %d %d:%d:%d %3s", wday, &day, mon, &year, &hh, &mm, &ss, tz) != 8) {
      return false;
    }
  }
  if (strcmp(tz, "GMT") != 0 && strcmp(tz, "UTC") != 0) return false;
  int m = -1;
  for (int i = 0; i < 12; ++i) {
    if (strncmp(mon, kMon[i], 3) == 0) m = i + 1;
  }
  if (m < 0 || day < 1 || day > 31 || year < 2000 || year > 2200 ||
      hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
    return false;
  }
  const int64_t t = days_from_civil(year, static_cast<unsigned>(m), static_cast<unsigned>(day)) * 86400 +
                    hh * 3600 + mm * 60 + ss;
  *out = static_cast<time_t>(t);
  return true;
}

}  // namespace clk

#ifndef UNIT_TEST
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>
#include <stdlib.h>
#include <sys/time.h>

namespace {
constexpr const char* kTag = "clock";
constexpr uint32_t kSntpGraceMs = 30000;          // tier 3 after 30 s without SNTP
constexpr uint32_t kFallbackRetryMs = 300000;     // retry tier 3 every 5 min while unsynced
std::atomic<bool> g_sntp_synced{false};

void on_sntp_sync(struct timeval*) { g_sntp_synced.store(true); }

struct DnsSlot {
  std::atomic<uint8_t> state{0};  // 0=FREE, 1=PENDING, 2=COMPLETED, 3=ABANDONED
  SemaphoreHandle_t sem = nullptr;
  char name[64] = "";
  bool ok = false;
  uint32_t ipv4 = 0;
};
constexpr uint8_t kDnsFree = 0, kDnsPending = 1, kDnsCompleted = 2, kDnsAbandoned = 3;
DnsSlot g_dns_slots[4];

void on_dns_found(const char*, const ip_addr_t* ipaddr, void* arg) {
  DnsSlot* s = static_cast<DnsSlot*>(arg);
  if (s->state.load() == kDnsAbandoned) {
    s->state.store(kDnsFree);
    return;
  }
  s->ok = (ipaddr != nullptr && IP_IS_V4(ipaddr));
  s->ipv4 = s->ok ? ip_2_ip4(ipaddr)->addr : 0;
  uint8_t expected = kDnsPending;
  if (s->state.compare_exchange_strong(expected, kDnsCompleted)) {
    xSemaphoreGive(s->sem);
  } else {
    s->state.store(kDnsFree);
  }
}

void dns_start_cb(void* arg) {
  DnsSlot* s = static_cast<DnsSlot*>(arg);
  if (s->state.load() != kDnsPending) {
    s->state.store(kDnsFree);
    return;
  }
  ip_addr_t addr = {};
  const err_t err = dns_gethostbyname(s->name, &addr, on_dns_found, s);
  if (err == ERR_OK) {
    on_dns_found(s->name, &addr, s);
  } else if (err != ERR_INPROGRESS) {
    on_dns_found(s->name, nullptr, s);
  }
}

struct DateCapture {
  char date[40];
};

esp_err_t date_event(esp_http_client_event_t* evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
      strcasecmp(evt->header_key, "Date") == 0) {
    DateCapture* cap = static_cast<DateCapture*>(evt->user_data);
    strncpy(cap->date, evt->header_value, sizeof(cap->date) - 1);
    cap->date[sizeof(cap->date) - 1] = '\0';
  }
  return ESP_OK;
}
}  // namespace

bool net_resolve_ipv4_bounded(const char* host, uint32_t timeout_ms, uint32_t* out_ipv4) {
  if (host == nullptr || host[0] == '\0' || out_ipv4 == nullptr) return false;
  ip4_addr_t literal;
  if (ip4addr_aton(host, &literal)) {
    *out_ipv4 = literal.addr;
    return true;
  }
  if (strlen(host) >= sizeof(g_dns_slots[0].name)) return false;

  DnsSlot* slot = nullptr;
  for (DnsSlot& s : g_dns_slots) {
    uint8_t expected = kDnsFree;
    if (s.state.compare_exchange_strong(expected, kDnsPending)) {
      slot = &s;
      break;
    }
  }
  if (slot == nullptr) return false;
  if (slot->sem == nullptr) slot->sem = xSemaphoreCreateBinary();
  if (slot->sem == nullptr) {
    slot->state.store(kDnsFree);
    return false;
  }
  xSemaphoreTake(slot->sem, 0);
  strncpy(slot->name, host, sizeof(slot->name) - 1);
  slot->name[sizeof(slot->name) - 1] = '\0';
  slot->ok = false;
  slot->ipv4 = 0;

  if (tcpip_callback(dns_start_cb, slot) != ERR_OK) {
    slot->state.store(kDnsFree);
    return false;
  }

  const uint32_t start_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  bool signaled = false;
  while (!signaled) {
    esp_task_wdt_reset();
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) - start_ms;
    if (elapsed >= timeout_ms) break;
    const uint32_t rem = timeout_ms - elapsed;
    const uint32_t wait_ms = rem < 500u ? rem : 500u;
    if (xSemaphoreTake(slot->sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
      signaled = true;
    }
  }
  esp_task_wdt_reset();

  if (!signaled) {
    uint8_t expected = kDnsPending;
    if (slot->state.compare_exchange_strong(expected, kDnsAbandoned)) {
      return false;
    }
    // on_dns_found won the CAS to kDnsCompleted and is about to give sem.
    xSemaphoreTake(slot->sem, pdMS_TO_TICKS(1000));
  }
  const bool ok = slot->ok;
  if (ok) *out_ipv4 = slot->ipv4;
  slot->state.store(kDnsFree);
  return ok;
}

void ClockSync::begin(const char* tz_posix) {
  set_timezone(tz_posix);
  // Tier 1: accept an NTP server from DHCP option 42. Must happen before DHCP.
#if defined(CONFIG_LWIP_DHCP_GET_NTP_SRV) && CONFIG_LWIP_DHCP_GET_NTP_SRV
  esp_sntp_servermode_dhcp(true);
  ESP_LOGI(kTag, "DHCP option 42 NTP enabled");
#else
  ESP_LOGW(kTag, "CONFIG_LWIP_DHCP_GET_NTP_SRV disabled in this core; tier 1 unavailable");
#endif
  // Tier 4: wall clock retained across a warm reboot (RTC keeps running).
  if (time(nullptr) >= clk::kValidEpoch) {
    status_ = clk::Status::RTC_RETAINED;
    ESP_LOGI(kTag, "RTC time retained across reboot");
  } else {
    status_ = clk::Status::UNSYNCED_FALLBACK_DAY;
  }
}

void ClockSync::set_timezone(const char* tz_posix) {
  setenv("TZ", tz_posix, 1);
  tzset();
}

void ClockSync::on_got_ip(uint32_t gateway_ipv4, uint32_t now_ms) {
  have_ip_ = true;
  got_ip_ms_ = now_ms;
  fallback_tried_ = false;
  gateway_ = gateway_ipv4;
  snprintf(gateway_str_, sizeof(gateway_str_), "%u.%u.%u.%u", gateway_ipv4 & 0xFF, (gateway_ipv4 >> 8) & 0xFF,
           (gateway_ipv4 >> 16) & 0xFF, (gateway_ipv4 >> 24) & 0xFF);
  start_sntp(gateway_ipv4);
}

void ClockSync::on_disconnected() {
  have_ip_ = false;
  if (sntp_started_) {
    esp_sntp_stop();
    ip_addr_t any = {};
    ip_addr_set_zero(&any);
    esp_sntp_setserver(0, &any);
  }
}

void ClockSync::start_sntp(uint32_t) {
  const bool was_started = sntp_started_;
  if (was_started) {
    esp_sntp_stop();
  } else {
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);  // first sync steps the clock
    sntp_set_sync_interval(3UL * 3600UL * 1000UL);
  }

  // Tier 2 server list. Slot 0 keeps a DHCP-provided server if one was offered.
  const char* srv0_name = esp_sntp_getservername(0);
  const ip_addr_t* dhcp_srv = esp_sntp_getserver(0);
  dhcp_ntp_present_ = (srv0_name == nullptr) && dhcp_srv != nullptr && !ip_addr_isany(dhcp_srv);
  int slot = 0;
  if (!dhcp_ntp_present_) esp_sntp_setservername(slot, gateway_str_);
  ++slot;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
  esp_sntp_setservername(slot++, "pool.ntp.org");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
  esp_sntp_setservername(slot++, "time.google.com");
#endif
  esp_sntp_init();
  sntp_started_ = true;
  ESP_LOGI(kTag, "SNTP %s: slot0=%s, pool.ntp.org, time.google.com (max servers %d)",
           was_started ? "restarted" : "started",
           dhcp_ntp_present_ ? "DHCP opt42" : gateway_str_, CONFIG_LWIP_SNTP_MAX_SERVERS);
}

void ClockSync::set_system_time(time_t utc, clk::Status src) {
  struct timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  status_ = src;
  ESP_LOGI(kTag, "clock set via %s", clk::status_to_str(src));
}

bool ClockSync::try_http_date(const char* url, const char* host_header) {
  DateCapture cap = {};
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_HEAD;
  cfg.timeout_ms = 3000;
  cfg.event_handler = date_event;
  cfg.user_data = &cap;
  cfg.disable_auto_redirect = true;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return false;
  if (host_header != nullptr && host_header[0] != '\0') {
    esp_http_client_set_header(c, "Host", host_header);
  }
  const esp_err_t err = esp_http_client_perform(c);
  esp_http_client_cleanup(c);
  time_t t;
  if (err == ESP_OK && cap.date[0] && clk::parse_http_date(cap.date, &t) && t >= clk::kValidEpoch) {
    set_system_time(t, clk::Status::SYNCED_HTTP_DATE);
    return true;
  }
  ESP_LOGW(kTag, "HTTP Date from %s failed (%s)", url, esp_err_to_name(err));
  return false;
}

void ClockSync::loop(uint32_t now_ms) {
  if (g_sntp_synced.exchange(false)) {
    status_ = dhcp_ntp_present_ ? clk::Status::SYNCED_DHCP_NTP : clk::Status::SYNCED_GATEWAY_NTP;
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);  // later corrections slew via adjtime
    ESP_LOGI(kTag, "SNTP synchronized (%s)", clk::status_to_str(status_));
  }
  const bool synced_by_network = status_ != clk::Status::UNSYNCED_FALLBACK_DAY &&
                                 status_ != clk::Status::RTC_RETAINED;
  if (!have_ip_ || synced_by_network) return;
  if (now_ms - got_ip_ms_ < kSntpGraceMs) return;
  if (fallback_tried_ && now_ms - last_fallback_try_ms_ < kFallbackRetryMs) return;
  fallback_tried_ = true;
  last_fallback_try_ms_ = now_ms;

  // Tier 3a: Matter Time Synchronization cluster (if a controller set UTCTime).
  time_t t;
  if (matter_src_ && matter_src_(&t) && t >= clk::kValidEpoch) {
    set_system_time(t, clk::Status::SYNCED_MATTER);
    return;
  }
  // Tier 3b: HTTP Date header (gateway first, then internet).
  char url[40];
  snprintf(url, sizeof(url), "http://%s/", gateway_str_);
  esp_task_wdt_reset();
  if (try_http_date(url, nullptr)) return;
  esp_task_wdt_reset();
  uint32_t google_ip = 0;
  if (net_resolve_ipv4_bounded("google.com", 3000, &google_ip)) {
    snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", google_ip & 0xFF, (google_ip >> 8) & 0xFF,
             (google_ip >> 16) & 0xFF, (google_ip >> 24) & 0xFF);
    esp_task_wdt_reset();
    try_http_date(url, "google.com");
  } else {
    ESP_LOGW(kTag, "HTTP Date DNS lookup for google.com timed out");
  }
}

bool ClockSync::valid() const { return time(nullptr) >= clk::kValidEpoch; }

bool ClockSync::local_hour(int* hour) const {
  if (!valid()) return false;
  const time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  *hour = lt.tm_hour;
  return true;
}

bool ClockSync::local_time_iso(char* out, size_t len) const {
  if (!valid()) {
    if (len) out[0] = '\0';
    return false;
  }
  const time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char tz[8] = {};
  const size_t tzn = strftime(tz, sizeof(tz), "%z", &lt);  // +0800
  char base[24] = {};
  if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &lt) == 0) {
    if (len) out[0] = '\0';
    return false;
  }
  if (tzn == 5 && (tz[0] == '+' || tz[0] == '-')) {
    snprintf(out, len, "%s%c%c%c:%c%c", base, tz[0], tz[1], tz[2], tz[3], tz[4]);
  } else {
    snprintf(out, len, "%s", base);
  }
  return true;
}
#endif  // UNIT_TEST
