// ag_client.cpp: /measures/current parser (host + device), discovery + HTTP (device).
#include "ag_client.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

#include "sys_config.h"  // JsonArena, AppConfig

namespace ag {

namespace {
bool read_num(JsonVariantConst v, float lo, float hi, float* out) {
  if (v.isNull() || !v.is<float>()) return false;  // is<float>() accepts integers too
  const float f = v.as<float>();
  if (isnan(f) || f < lo || f > hi) return false;
  *out = f;
  return true;
}
}  // namespace

const char* parse_result_to_str(ParseResult r) {
  switch (r) {
    case ParseResult::OK: return "ok";
    case ParseResult::BAD_JSON: return "bad_json";
    case ParseResult::SERIAL_MISMATCH: return "serial_mismatch";
    default: return "no_pm25";
  }
}

ParseResult parse_measures(const char* json, size_t len, const char* expected_serial, Measurement* out) {
  Measurement m;
  ParseResult res = ParseResult::OK;
  // Transient 4 KB arena: single allocation prevents node fragmentation during polls
  // while keeping 0 B in .bss during Matter + NimBLE boot.
  constexpr size_t kArenaBytes = 4096;
  uint8_t* arena_mem = static_cast<uint8_t*>(malloc(kArenaBytes));
  if (arena_mem == nullptr) {
    *out = m;
    return ParseResult::BAD_JSON;
  }
  JsonArena arena(arena_mem, kArenaBytes);
  {
    // Only the keys used below are kept, so a larger payload from newer monitor
    // firmware cannot exhaust the static arena.
    JsonDocument filter(&arena);
    for (const char* k : {"serialno", "pm02Compensated", "pm02", "tvocIndex", "noxIndex", "rco2", "atmp", "rhum", "boot"}) {
      filter[k] = true;
    }
    JsonDocument doc(&arena);
    if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter)) || !doc.is<JsonObject>()) {
      res = ParseResult::BAD_JSON;
    } else {
      const char* sn = doc["serialno"] | "";
      strncpy(m.serialno, sn, sizeof(m.serialno) - 1);
      // With a configured serial, the reply must carry the same serial (SRS §2.2):
      // a missing serialno is a mismatch, so another HTTP device can't drive the purifier.
      if (expected_serial && expected_serial[0] && strcmp(m.serialno, expected_serial) != 0) {
        res = ParseResult::SERIAL_MISMATCH;
      } else {
        m.pm25_valid = read_num(doc["pm02Compensated"], 0.0f, 1000.0f, &m.pm25);
        if (!m.pm25_valid) m.pm25_valid = read_num(doc["pm02"], 0.0f, 1000.0f, &m.pm25);
        m.tvoc_valid = read_num(doc["tvocIndex"], 0.0f, 500.0f, &m.tvoc_index);
        m.nox_valid = read_num(doc["noxIndex"], 0.0f, 500.0f, &m.nox_index);
        m.co2_valid = read_num(doc["rco2"], 0.0f, 40000.0f, &m.co2_ppm);
        m.temp_valid = read_num(doc["atmp"], -40.0f, 85.0f, &m.temp_c);
        m.rh_valid = read_num(doc["rhum"], 0.0f, 100.0f, &m.rh_pct);
        float boot;
        if (read_num(doc["boot"], 0.0f, 4.0e9f, &boot)) {
          m.boot_valid = true;
          m.boot = static_cast<uint32_t>(boot);
        }
        if (!m.pm25_valid) res = ParseResult::NO_PM25;
      }
    }
  }
  arena.reset();
  free(arena_mem);
  *out = m;
  return res;
}

bool StaleDetector::update(const Measurement& m) {
  if (!m.boot_valid) return stale();
  if (have_ && m.boot == last_boot_) {
    if (unchanged_ < 1000) ++unchanged_;
  } else {
    unchanged_ = 0;
  }
  have_ = true;
  last_boot_ = m.boot;
  return stale();
}

}  // namespace ag

#ifndef UNIT_TEST
#include <IPAddress.h>
#include <mdns.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <lwip/inet.h>

#include "clock_sync.h"

namespace {
constexpr const char* kTag = "ag";
constexpr uint32_t kHostTimeoutMs = 3000;
constexpr uint32_t kCachedTimeoutMs = 2500;   // SRS §2.2 step 2
constexpr uint32_t kMdnsTimeoutMs = 2000;     // SRS §2.2 step 3
constexpr int kMaxScanCandidates = 4;
constexpr size_t kMaxAgBodyBytes = 2048;

struct ScopedBuf {
  char* ptr;
  explicit ScopedBuf(size_t n) : ptr(static_cast<char*>(malloc(n))) {}
  ~ScopedBuf() { free(ptr); }
  ScopedBuf(const ScopedBuf&) = delete;
  ScopedBuf& operator=(const ScopedBuf&) = delete;
};

void ip_to_str(uint32_t ip, char* out, size_t len) {
  snprintf(out, len, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}
}  // namespace

const char* AgClient::source_to_str(Source s) {
  switch (s) {
    case Source::TARGET_HOST: return "target_host";
    case Source::CACHED_IP: return "cached_ip";
    case Source::MDNS_HOST: return "mdns_host";
    case Source::MDNS_SCAN: return "mdns_scan";
    default: return "none";
  }
}

void AgClient::begin() {
  uint32_t ip = 0;
  if (state_load_ag_cached_ip(&ip)) {
    cached_ip_ = ip;
    char s[16];
    ip_to_str(ip, s, sizeof(s));
    ESP_LOGI(kTag, "cached AirGradient IP from NVS: %s", s);
  }
}

void AgClient::invalidate_cache() {
  cached_ip_ = 0;
  resolved_ip_ = 0;
  state_save_ag_cached_ip(0);
}

void AgClient::remember(uint32_t ip) {
  resolved_ip_ = ip;
  if (ip != cached_ip_) {
    cached_ip_ = ip;
    state_save_ag_cached_ip(ip);  // NVS write only when the address changes
  }
}

bool AgClient::fetch(const char* host, uint32_t timeout_ms, const char* serial, ag::Measurement* out,
                     const char* host_header) {
  ScopedBuf body(kMaxAgBodyBytes);
  if (body.ptr == nullptr) return false;
  char url[96];
  snprintf(url, sizeof(url), "http://%s/measures/current", host);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = static_cast<int>(timeout_ms);
  cfg.disable_auto_redirect = true;
  cfg.buffer_size = 1024;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return false;
  if (host_header != nullptr && host_header[0] != '\0') {
    esp_http_client_set_header(c, "Host", host_header);
  }

  bool ok = false;
  last_http_ = 0;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    last_http_ = esp_http_client_get_status_code(c);
    if (last_http_ == 200) {
      int total = 0;
      while (total < static_cast<int>(kMaxAgBodyBytes) - 1) {
        const int n = esp_http_client_read(c, body.ptr + total, static_cast<int>(kMaxAgBodyBytes) - 1 - total);
        if (n <= 0) break;
        total += n;
      }
      body.ptr[total] = '\0';
      last_parse_ = ag::parse_measures(body.ptr, static_cast<size_t>(total), serial, out);
      ok = last_parse_ == ag::ParseResult::OK;
      if (!ok) ESP_LOGW(kTag, "%s: %s", url, ag::parse_result_to_str(last_parse_));
    } else {
      ESP_LOGW(kTag, "%s: HTTP %d", url, last_http_);
    }
  } else {
    ESP_LOGD(kTag, "%s: connect failed", url);
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return ok;
}

bool AgClient::try_ip(uint32_t ip, uint32_t timeout_ms, const char* serial, ag::Measurement* out) {
  if (ip == 0) return false;
  char host[16];
  ip_to_str(ip, host, sizeof(host));
  return fetch(host, timeout_ms, serial, out);
}

bool AgClient::poll(const AppConfig& cfg, ag::Measurement* out) {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    source_ = Source::NONE;
    return false;
  }
  const char* serial = cfg.ag_serial;

  // Step 1: static host override (supports IP, IP:port, .local[:port], hostname[:port])
  if (cfg.ag_target_host[0]) {
    char host_only[64];
    const char* colon = strchr(cfg.ag_target_host, ':');
    const size_t hlen = colon ? static_cast<size_t>(colon - cfg.ag_target_host) : strlen(cfg.ag_target_host);
    if (hlen > 0 && hlen < sizeof(host_only)) {
      memcpy(host_only, cfg.ag_target_host, hlen);
      host_only[hlen] = '\0';
      const char* port_part = colon ? colon : "";
      IPAddress ip;
      if (ip.fromString(host_only)) {
        if (fetch(cfg.ag_target_host, kHostTimeoutMs, serial, out)) {
          source_ = Source::TARGET_HOST;
          resolved_ip_ = static_cast<uint32_t>(ip);
          return true;
        }
      } else {
        uint32_t resolved = 0;
        bool ok_dns = false;
        if (hlen > 6 && strcasecmp(host_only + hlen - 6, ".local") == 0) {
          char stem[64];
          memcpy(stem, host_only, hlen - 6);
          stem[hlen - 6] = '\0';
          esp_ip4_addr_t ma = {};
          if (mdns_query_a(stem, kMdnsTimeoutMs, &ma) == ESP_OK && ma.addr != 0) {
            resolved = ma.addr;
            ok_dns = true;
          }
        } else {
          ok_dns = net_resolve_ipv4_bounded(host_only, kHostTimeoutMs, &resolved);
        }
        esp_task_wdt_reset();
        if (ok_dns && resolved != 0) {
          char target[32];
          snprintf(target, sizeof(target), "%u.%u.%u.%u%s", resolved & 0xFF, (resolved >> 8) & 0xFF,
                   (resolved >> 16) & 0xFF, (resolved >> 24) & 0xFF, port_part);
          if (fetch(target, kHostTimeoutMs, serial, out, cfg.ag_target_host)) {
            source_ = Source::TARGET_HOST;
            resolved_ip_ = resolved;
            return true;
          }
        }
      }
    }
    esp_task_wdt_reset();
  }
  // Step 2: cached IPv4 fast path
  if (try_ip(cached_ip_, kCachedTimeoutMs, serial, out)) {
    source_ = Source::CACHED_IP;
    resolved_ip_ = cached_ip_;
    return true;
  }
  esp_task_wdt_reset();
  // Step 3: targeted mDNS hostname
  // (ESP-IDF mdns API: shares the responder instance used by the Matter stack)
  char host[40];
  snprintf(host, sizeof(host), "airgradient_%s", serial);
  esp_ip4_addr_t a = {};
  if (mdns_query_a(host, kMdnsTimeoutMs, &a) == ESP_OK && a.addr != 0) {
    esp_task_wdt_reset();
    if (try_ip(a.addr, kCachedTimeoutMs, serial, out)) {
      source_ = Source::MDNS_HOST;
      remember(a.addr);
      return true;
    }
  }
  esp_task_wdt_reset();
  // Step 4: service scan with JSON signature verification
  static const char* const kServices[] = {"_airgradient", "_http"};
  for (const char* svc : kServices) {
    esp_task_wdt_reset();
    mdns_result_t* results = nullptr;
    if (mdns_query_ptr(svc, "_tcp", kMdnsTimeoutMs, kMaxScanCandidates, &results) != ESP_OK) continue;
    bool found = false;
    for (mdns_result_t* r = results; r != nullptr && !found; r = r->next) {
      for (mdns_ip_addr_t* ia = r->addr; ia != nullptr && !found; ia = ia->next) {
        if (ia->addr.type != ESP_IPADDR_TYPE_V4) continue;
        const uint32_t cand = ia->addr.u_addr.ip4.addr;
        if (cand == 0 || cand == cached_ip_) continue;
        esp_task_wdt_reset();
        ag::Measurement m;
        if (try_ip(cand, kCachedTimeoutMs, serial, &m) && m.serialno[0]) {  // must carry the signature
          *out = m;
          source_ = Source::MDNS_SCAN;
          remember(cand);
          found = true;
        }
      }
    }
    mdns_query_results_free(results);
    if (found) return true;
  }
  source_ = Source::NONE;
  return false;
}
#endif  // UNIT_TEST
