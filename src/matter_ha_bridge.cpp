// matter_ha_bridge.cpp: Matter endpoints + REST API + mDNS delegate host.
#include "matter_ha_bridge.h"

#include <math.h>

// ---------------------------------------------------------------------------
// Pure helpers (compiled on both device and host UNIT_TEST builds)
// ---------------------------------------------------------------------------
namespace {
// Matter AirQualityEnum PM2.5 breakpoints (EPA 2024 aligned, ug/m3)
constexpr float kAqGoodMax = 9.0f;
constexpr float kAqFairMax = 35.4f;
constexpr float kAqModerateMax = 55.4f;
constexpr float kAqPoorMax = 125.4f;
constexpr float kAqVeryPoorMax = 225.4f;

// Matter LevelValueEnum breakpoints for Sensirion indices (spec v3)
constexpr float kTvocLowMax = 150.0f;
constexpr float kTvocMedMax = 250.0f;
constexpr float kTvocHighMax = 400.0f;
constexpr float kNoxMedMin = 4.0f;
constexpr float kNoxHighMin = 6.0f;
constexpr float kNoxCritMin = 20.0f;
}  // namespace

uint8_t matter_air_quality_from_pm25(float pm) {
  if (!isfinite(pm) || pm < 0.0f) return 0;  // Unknown
  if (pm <= kAqGoodMax) return 1;            // Good          (EPA 2024 Good)
  if (pm <= kAqFairMax) return 2;            // Fair          (Moderate)
  if (pm <= kAqModerateMax) return 3;        // Moderate      (USG)
  if (pm <= kAqPoorMax) return 4;            // Poor          (Unhealthy)
  if (pm <= kAqVeryPoorMax) return 5;        // VeryPoor      (Very Unhealthy)
  return 6;                                  // ExtremelyPoor (Hazardous)
}

uint8_t matter_level_from_tvoc_index(float idx) {  // spec v3: >200 moderate, >300 strong plume
  if (!isfinite(idx) || idx < 0.0f) return 0;
  if (idx <= kTvocLowMax) return 1;
  if (idx <= kTvocMedMax) return 2;
  if (idx <= kTvocHighMax) return 3;
  return 4;
}

uint8_t matter_level_from_nox_index(float idx) {  // spec v3: baseline 1, >=4 combustion (P95)
  if (!isfinite(idx) || idx < 0.0f) return 0;
  if (idx < kNoxMedMin) return 1;
  if (idx < kNoxHighMin) return 2;
  if (idx < kNoxCritMin) return 3;
  return 4;
}

bool ota_size_is_valid(size_t content_len, size_t partition_size) {
  return content_len >= kOtaMinImageBytes && content_len <= partition_size;
}

bool ota_header_is_valid(const uint8_t* data, size_t len) {
  if (data == nullptr || len < kOtaHeaderBytes) return false;
  if (data[0] != kOtaImageMagic) return false;
  const uint16_t chip_id = static_cast<uint16_t>(data[12]) | (static_cast<uint16_t>(data[13]) << 8);
  return chip_id == kOtaChipIdEsp32S3;
}

#ifndef UNIT_TEST
#include <Matter.h>
#include <app/clusters/air-quality-server/air-quality-server.h>
#include <app/clusters/time-synchronization-server/DefaultTimeSyncDelegate.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <mdns.h>
#include <platform/PlatformManager.h>
#include <system/SystemClock.h>

#include <atomic>
#include <errno.h>
#include <new>

#include "clock_sync.h"
#include "iaq_algorithm.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace {
constexpr const char* kTag = "bridge";
constexpr const char* kMdnsHost = "atoms3-iaq";
constexpr size_t kMaxBody = 1536;
constexpr int kBodyRecvRetries = 1;          // extra httpd_req_recv timeouts tolerated per body
constexpr int kOtaRecvRetries = 3;           // extra httpd_req_recv timeouts tolerated per OTA chunk
constexpr uint16_t kHttpRecvTimeoutS = 3;    // per-recv socket timeout (bounds slow clients)
constexpr uint16_t kHttpSendTimeoutS = 3;
constexpr uint16_t kHttpCtrlPort = 32790;    // must not clash with Matter (5540) or other services
constexpr size_t kHttpStackBytes = 6144;     // REST + OTA flash writes (large buffers are heap-allocated)
constexpr uint16_t kHttpMaxSockets = 4;
constexpr uint16_t kHttpMaxUriHandlers = 8;

// Set while our own state is pushed into Matter. Publishing runs as scheduled
// work in the CHIP task, so controller writes (also handled in the CHIP task)
// can never interleave with it; the resulting attribute callbacks, which run
// synchronously, are recognised as self-updates.
bool g_publishing = false;
std::atomic<bool> g_work_scheduled{false};
std::atomic<bool> g_commissioned{false};
std::atomic<int> g_fabrics{0};
std::atomic<bool> g_ota_busy{false};
std::atomic<bool> g_matter_utc_synced{false};
std::atomic<int64_t> g_matter_utc_unix{0};
std::atomic<uint32_t> g_matter_utc_set_ms{0};
MatterHaBridge* g_self = nullptr;
constexpr uint32_t kOnboardingReprintMs = 30000;
char g_onboarding_code[24] = "";
char g_onboarding_qr[160] = "";
uint32_t g_last_onboarding_print_ms = 0;

void print_onboarding_banner() {
  if (g_onboarding_code[0] == '\0') return;
  Serial.println();
  Serial.println("==== Matter commissioning (Apple Home / Google Home) ====");
  Serial.printf("Manual pairing code: %s\n", g_onboarding_code);
  Serial.printf("QR code URL:         %s\n", g_onboarding_qr);
  Serial.println("=========================================================");
}

class TimeSyncDelegate : public chip::app::Clusters::TimeSynchronization::DefaultTimeSyncDelegate {
 public:
  CHIP_ERROR UpdateTimeFromPlatformSource(
      chip::Callback::Callback<chip::app::Clusters::TimeSynchronization::OnTimeSyncCompletion>* cb) override {
    // SNTP and HTTP Date are managed by ClockSync; only accept Matter-sourced time here.
    (void)cb;
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
  }
  void UTCTimeAvailabilityChanged(uint64_t utc_us) override {
    const time_t unix_s = clk::matter_utc_us_to_unix(utc_us);
    if (unix_s >= clk::kValidEpoch) {
      g_matter_utc_unix.store(static_cast<int64_t>(unix_s));
      g_matter_utc_set_ms.store(millis());
      g_matter_utc_synced = true;
      ESP_LOGI(kTag, "Matter TimeSynchronization UTCTime updated (unix=%lld)", static_cast<long long>(unix_s));
    } else {
      ESP_LOGW(kTag, "Matter TimeSynchronization UTCTime ignored (before valid epoch: %lld)",
               static_cast<long long>(unix_s));
    }
  }
};
TimeSyncDelegate g_time_sync_delegate;

MatterFan g_fan;
MatterOnOffPlugin g_night_lock;

// ---------------------------------------------------------------------------
// EP2: composite Air Quality + Temperature + Humidity sensor with a fault flag.
// Subclasses MatterEndPoint so Arduino's attribute router accepts it and so the
// protected BooleanState setter (code-driven cluster in ESP-Matter 1.5+) is usable.
// ---------------------------------------------------------------------------
class AirQualityEndpoint : public MatterEndPoint {
 public:
  bool begin() {
    endpoint::air_quality_sensor::config_t ep_cfg;
    endpoint_t* ep = endpoint::air_quality_sensor::create(node::get(), &ep_cfg, ENDPOINT_FLAG_NONE, this);
    if (ep == nullptr) {
      ESP_LOGE(kTag, "air_quality_sensor endpoint create failed");
      return false;
    }
    // AirQuality enum Good..ExtremelyPoor requires all optional features.
    cluster_t* aq = cluster::get(ep, AirQuality::Id);
    if (aq != nullptr) {
      cluster::air_quality::feature::fair::add(aq);
      cluster::air_quality::feature::moderate::add(aq);
      cluster::air_quality::feature::very_poor::add(aq);
      cluster::air_quality::feature::extremely_poor::add(aq);
    }
    namespace cm = cluster::concentration_measurement;
    // PM2.5 numeric in ug/m3
    {
      cluster::pm25_concentration_measurement::config_t c;
      c.measurement_medium = 0;  // air
      c.feature_flags = cm::feature::numeric_measurement::get_id();
      c.features.numeric_measurement.measurement_unit = 4;  // MeasurementUnitEnum::kUgm3
      c.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
      c.features.numeric_measurement.max_measured_value = nullable<float>(1000.0f);
      cluster::pm25_concentration_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
    }
    // CO2 numeric in ppm
    {
      cluster::carbon_dioxide_concentration_measurement::config_t c;
      c.measurement_medium = 0;
      c.feature_flags = cm::feature::numeric_measurement::get_id();
      c.features.numeric_measurement.measurement_unit = 0;  // kPpm
      c.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
      c.features.numeric_measurement.max_measured_value = nullable<float>(10000.0f);
      cluster::carbon_dioxide_concentration_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
    }
    // TVOC and NO2: the AirGradient reports Sensirion *indices*, not concentrations,
    // so these clusters use LevelIndication (Low/Medium/High/Critical) instead of
    // publishing a fake numeric value.
    {
      cluster::total_volatile_organic_compounds_concentration_measurement::config_t c;
      c.measurement_medium = 0;
      c.feature_flags = cm::feature::level_indication::get_id() | cm::feature::medium_level::get_id() |
                        cm::feature::critical_level::get_id();
      cluster::total_volatile_organic_compounds_concentration_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
    }
    {
      cluster::nitrogen_dioxide_concentration_measurement::config_t c;
      c.measurement_medium = 0;
      c.feature_flags = cm::feature::level_indication::get_id() | cm::feature::medium_level::get_id() |
                        cm::feature::critical_level::get_id();
      cluster::nitrogen_dioxide_concentration_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
    }
    // Temperature + humidity on the same endpoint (composite device types)
    {
      cluster::temperature_measurement::config_t c;
      cluster::temperature_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
      cluster::relative_humidity_measurement::config_t h;
      cluster::relative_humidity_measurement::create(ep, &h, CLUSTER_FLAG_SERVER);
      endpoint::add_device_type(ep, endpoint::temperature_sensor::get_device_type_id(),
                                endpoint::temperature_sensor::get_device_type_version());
      endpoint::add_device_type(ep, endpoint::humidity_sensor::get_device_type_id(),
                                endpoint::humidity_sensor::get_device_type_version());
    }
    // Sensor-unreachable fault flag (StateValue = true -> AirGradient unreachable >= 5 min)
    {
      cluster::boolean_state::config_t b;
      b.state_value = false;
      cluster::boolean_state::create(ep, &b, CLUSTER_FLAG_SERVER);
    }
    setEndPointId(endpoint::get_id(ep));
    ESP_LOGI(kTag, "EP%u air quality sensor ready", getEndPointId());
    return true;
  }

  bool attributeChangeCB(uint16_t, uint32_t, uint32_t, esp_matter_attr_val_t*) override {
    return true;  // read-only sensor endpoint
  }

  bool set_float(uint32_t cluster_id, uint32_t attr_id, bool valid, float v) {
    esp_matter_attr_val_t val = esp_matter_nullable_float(valid ? nullable<float>(v) : nullable<float>());
    return updateAttributeVal(cluster_id, attr_id, &val);
  }
  bool set_enum8(uint32_t cluster_id, uint32_t attr_id, uint8_t v) {
    esp_matter_attr_val_t val = esp_matter_enum8(v);
    return updateAttributeVal(cluster_id, attr_id, &val);
  }
  bool set_temperature(bool valid, float c) {
    esp_matter_attr_val_t val = esp_matter_nullable_int16(
        valid ? nullable<int16_t>(static_cast<int16_t>(lroundf(c * 100.0f))) : nullable<int16_t>());
    return updateAttributeVal(TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
  }
  bool set_humidity(bool valid, float pct) {
    esp_matter_attr_val_t val = esp_matter_nullable_uint16(
        valid ? nullable<uint16_t>(static_cast<uint16_t>(lroundf(pct * 100.0f))) : nullable<uint16_t>());
    return updateAttributeVal(RelativeHumidityMeasurement::Id,
                              RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
  }
  bool set_air_quality(uint8_t v) {
    if (aq_inst_ == nullptr) {
      if (!emberAfContainsServer(getEndPointId(), AirQuality::Id)) {
        ESP_LOGW(kTag, "AirQuality server not ready on EP%u", getEndPointId());
        return false;
      }
      const chip::BitMask<AirQuality::Feature> features(
          AirQuality::Feature::kFair, AirQuality::Feature::kModerate,
          AirQuality::Feature::kVeryPoor, AirQuality::Feature::kExtremelyPoor);
      auto* inst = new (std::nothrow) AirQuality::Instance(getEndPointId(), features);
      if (inst == nullptr || inst->Init() != CHIP_NO_ERROR) {
        ESP_LOGE(kTag, "AirQuality instance init failed on EP%u", getEndPointId());
        delete inst;
        return false;
      }
      aq_inst_ = inst;
    }
    return aq_inst_->UpdateAirQuality(static_cast<AirQuality::AirQualityEnum>(v)) ==
           chip::Protocols::InteractionModel::Status::Success;
  }
  bool set_fault(bool fault) { return setBooleanStateValue(fault); }

 private:
  AirQuality::Instance* aq_inst_ = nullptr;
};

AirQualityEndpoint g_aq;

bool post(const BridgeCmd& c, TickType_t wait = 0) {
  if (g_self == nullptr || g_self->queue() == nullptr) return false;
  if (xQueueSend(g_self->queue(), &c, wait) != pdTRUE) {
    ESP_LOGW(kTag, "command queue full, dropped");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// REST API (buffers allocated only once start_http() starts httpd after IP)
// ---------------------------------------------------------------------------
struct HttpBuffers {
  alignas(8) uint8_t arena[4096];
  char out[2048];
  char body[kMaxBody + 1];
};
HttpBuffers* g_http = nullptr;

void put_num(JsonObject o, const char* k, float v, int decimals) {
  if (isnan(v)) {
    o[k] = nullptr;
  } else {
    const float p = powf(10.0f, static_cast<float>(decimals));
    o[k] = roundf(v * p) / p;
  }
}

esp_err_t send_json(httpd_req_t* req, const char* status, const char* body, size_t len) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, static_cast<ssize_t>(len));
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* msg) {
  JsonArena arena(g_http->arena, sizeof(g_http->arena));
  size_t n = 0;
  {
    JsonDocument doc(&arena);
    doc["error"] = msg;
    n = serializeJson(doc, g_http->out, sizeof(g_http->out));
  }
  arena.reset();
  return send_json(req, status, g_http->out, n);
}

// After an early rejection the request body may be unread. Returning ESP_FAIL
// makes esp_http_server close the socket instead of draining the remaining
// Content-Length, so a slow or huge body can't pin the single httpd task.
// Call after the response was sent; set kConnClose first so clients know.
constexpr const char* kConnClose = "close";
esp_err_t finish_unread(httpd_req_t* req) { return req->content_len > 0 ? ESP_FAIL : ESP_OK; }

esp_err_t reject(httpd_req_t* req, const char* status, const char* msg) {
  if (req->content_len > 0) httpd_resp_set_hdr(req, "Connection", kConnClose);
  send_error(req, status, msg);
  return finish_unread(req);
}

// Lockout key of the client that sent `req` (SRS §5.3: lockouts are per client).
uint32_t peer_key(httpd_req_t* req) {
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0) {
    ESP_LOGW(kTag, "httpd_req_to_sockfd failed (%d); using fallback peer bucket", fd);
    return 0xFFFFFFFFu;
  }
  struct sockaddr_storage ss = {};
  socklen_t len = sizeof(ss);
  if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) {
    ESP_LOGW(kTag, "getpeername failed (fd=%d, errno=%d); using fallback peer bucket", fd, errno);
    return 0xFFFFFFFFu;
  }
  if (ss.ss_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
    return auth::peer_key_ipv4(reinterpret_cast<struct sockaddr_in*>(&ss)->sin_addr.s_addr);
  }
  if (ss.ss_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
    return auth::peer_key_ipv6(reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_addr.s6_addr);
  }
  ESP_LOGW(kTag, "unexpected peer address family %d (len=%u)", static_cast<int>(ss.ss_family),
           static_cast<unsigned>(len));
  return 0xFFFFFFFFu;
}

// Copies header `name` into buf; nullptr if absent or longer than buf.
const char* header(httpd_req_t* req, const char* name, char* buf, size_t buf_len) {
  const size_t n = httpd_req_get_hdr_value_len(req, name);
  if (n == 0 || n >= buf_len) return nullptr;
  return httpd_req_get_hdr_value_str(req, name, buf, buf_len) == ESP_OK ? buf : nullptr;
}

enum class Body : uint8_t { OK, MISSING, TOO_LARGE, IO_ERROR };

Body read_body(httpd_req_t* req, size_t* len) {
  if (req->content_len == 0) return Body::MISSING;
  if (req->content_len > kMaxBody) return Body::TOO_LARGE;
  size_t got = 0;
  int timeouts = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, g_http->body + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= kBodyRecvRetries) continue;  // bounded
    if (r <= 0) return Body::IO_ERROR;
    got += static_cast<size_t>(r);
  }
  g_http->body[got] = '\0';
  *len = got;
  return Body::OK;
}

// Reads the body or sends the matching error. Returns true when the body is ready.
bool read_body_or_reject(httpd_req_t* req, size_t* len, esp_err_t* ret) {
  switch (read_body(req, len)) {
    case Body::OK: return true;
    case Body::MISSING: *ret = reject(req, "400 Bad Request", "missing body"); return false;
    case Body::TOO_LARGE: *ret = reject(req, "413 Payload Too Large", "body too large"); return false;
    default: *ret = reject(req, "400 Bad Request", "incomplete body"); return false;
  }
}

esp_err_t h_status(httpd_req_t* req) {
  StatusSnapshot s;
  g_self->copy_status(&s);
  JsonArena arena(g_http->arena, sizeof(g_http->arena));
  size_t n = 0;
  {
    JsonDocument doc(&arena);
    doc["current_speed"] = s.current_speed;
    doc["operating_mode"] = operating_mode_to_str(s.operating_mode);
    doc["pm_target_speed"] = s.pm_target_speed;
    doc["gas_target_speed"] = s.gas_target_speed;
    doc["dominant_trigger"] = s.dominant_trigger;
    doc["current_airflow_m3h"] = static_cast<int>(s.current_airflow_m3h);
    doc["effective_ach"] = roundf(s.effective_ach * 100.0f) / 100.0f;
    doc["room_volume_m3"] = roundf(s.room_volume_m3 * 10.0f) / 10.0f;
    doc["rho_volume_factor"] = roundf(s.rho_volume_factor * 1000.0f) / 1000.0f;
    doc["dwell_stepdown_mins"] = s.dwell_stepdown_mins;
    doc["min_stepdown_hold_mins"] = s.min_stepdown_hold_mins;
    doc["is_night_mode"] = s.is_night_mode;
    doc["clock_sync_status"] = s.clock_sync_status;
    if (s.local_time_iso[0]) doc["local_time_iso"] = s.local_time_iso; else doc["local_time_iso"] = nullptr;
    doc["ag_status"] = s.ag_status;
    doc["alarm_sensor_unreachable"] = s.alarm_sensor_unreachable;
    if (s.ag_resolved_ip[0]) doc["ag_resolved_ip"] = s.ag_resolved_ip; else doc["ag_resolved_ip"] = nullptr;
    doc["ag_source"] = s.ag_source;
    doc["ag_fail_count"] = s.ag_fail_count;
    doc["ag_data_stale"] = s.ag_data_stale;
    doc["api_paired"] = s.api_paired;
    doc["api_pairing_active"] = s.api_pairing_active;
    JsonObject t = doc["telemetry"].to<JsonObject>();
    put_num(t, "pm25_raw", s.pm25_raw, 1);
    put_num(t, "pm25_sma5", s.pm25_sma5, 1);
    put_num(t, "tvoc_raw", s.tvoc_raw, 1);
    put_num(t, "tvoc_sma5", s.tvoc_sma5, 1);
    put_num(t, "nox_raw", s.nox_raw, 1);
    put_num(t, "nox_sma5", s.nox_sma5, 1);
    put_num(t, "co2_ppm", s.co2_ppm, 0);
    put_num(t, "temperature_c", s.temperature_c, 1);
    put_num(t, "humidity_pct", s.humidity_pct, 1);
    doc["matter_commissioned"] = s.matter_commissioned;
    doc["matter_fabrics_count"] = s.matter_fabrics_count;
    doc["speed_changes_today"] = s.speed_changes_today;
    doc["wifi_rssi_dbm"] = s.wifi_rssi_dbm;
    doc["free_heap_bytes"] = s.free_heap_bytes;
    doc["min_free_heap_bytes"] = s.min_free_heap_bytes;
    doc["uptime_s"] = s.uptime_s;
    doc["ir_frames_sent"] = s.ir_frames_sent;
    n = doc.overflowed() ? 0 : serializeJson(doc, g_http->out, sizeof(g_http->out));
  }
  arena.reset();
  if (n == 0 || n >= sizeof(g_http->out)) return send_error(req, "500 Internal Server Error", "status overflow");
  return send_json(req, "200 OK", g_http->out, n);
}

esp_err_t h_config_get(httpd_req_t* req) {
  AppConfig copy;
  xSemaphoreTake(g_self->config_mutex(), portMAX_DELAY);
  copy = *g_self->config();
  xSemaphoreGive(g_self->config_mutex());
  const size_t n = config_to_json(copy, g_http->out, sizeof(g_http->out));
  if (n == 0) return send_error(req, "500 Internal Server Error", "config overflow");
  return send_json(req, "200 OK", g_http->out, n);
}

esp_err_t h_config_patch(httpd_req_t* req) {
  if (g_self->auth_require(req) != 0) return finish_unread(req);  // 401 / 429 already sent
  size_t len = 0;
  esp_err_t ret = ESP_OK;
  if (!read_body_or_reject(req, &len, &ret)) return ret;
  char err[96] = "";
  uint32_t mask = 0;
  AppConfig copy, previous, commit;
  bool ok;
  if (uxQueueSpacesAvailable(g_self->queue()) == 0) return send_error(req, "503 Service Unavailable", "busy, retry");
  xSemaphoreTake(g_self->config_mutex(), portMAX_DELAY);
  previous = *g_self->config();
  copy = previous;
  ok = config_apply_json_patch(&copy, g_http->body, len, &mask, err, sizeof(err));
  BridgeCmd cmd{BridgeCmdType::CONFIG_PATCHED, copy.manual_speed, false, mask,
                static_cast<uint8_t>(copy.operating_mode)};
  if (ok) {
    // Mode fields are owned by the loop (it applies them to the mode controller and
    // writes them back), so commit everything except them here.
    commit = copy;
    commit.operating_mode = previous.operating_mode;
    commit.manual_speed = previous.manual_speed;
    *g_self->config() = commit;
  }
  xSemaphoreGive(g_self->config_mutex());
  if (!ok) return send_error(req, "422 Unprocessable Entity", err);
  if (!post(cmd, pdMS_TO_TICKS(100))) {
    // The loop would never apply or persist this change: roll it back so the
    // device is never left half-configured (unless something else changed it since).
    xSemaphoreTake(g_self->config_mutex(), portMAX_DELAY);
    if (memcmp(g_self->config(), &commit, sizeof(AppConfig)) == 0) *g_self->config() = previous;
    xSemaphoreGive(g_self->config_mutex());
    return send_error(req, "503 Service Unavailable", "busy, not applied; retry");
  }
  const size_t n = config_to_json(copy, g_http->out, sizeof(g_http->out));
  if (n == 0) return send_error(req, "500 Internal Server Error", "config overflow");
  return send_json(req, "200 OK", g_http->out, n);
}

esp_err_t h_command(httpd_req_t* req) {
  if (g_self->auth_require(req) != 0) return finish_unread(req);  // 401 / 429 already sent
  size_t len = 0;
  esp_err_t ret = ESP_OK;
  if (!read_body_or_reject(req, &len, &ret)) return ret;
  BridgeCmd cmd{BridgeCmdType::SET_AUTO, 0, false, 0, 0};
  const char* error = nullptr;
  JsonArena arena(g_http->arena, sizeof(g_http->arena));
  {
    JsonDocument doc(&arena);
    if (deserializeJson(doc, g_http->body, len) || !doc.is<JsonObject>()) {
      error = "invalid JSON";
    } else {
      const char* action = doc["action"] | "";
      JsonVariantConst sp = doc["speed"];
      const bool has_speed = sp.is<int>() && sp.as<int>() >= 0 && sp.as<int>() <= 6;
      if (strcmp(action, "set_speed") == 0) {
        const char* mode = doc["mode"] | "manual";
        if (strcmp(mode, "auto") == 0) {
          cmd.type = BridgeCmdType::SET_AUTO;
        } else if (strcmp(mode, "manual") == 0 && has_speed) {
          cmd.type = BridgeCmdType::SET_MANUAL;
          cmd.speed = static_cast<uint8_t>(sp.as<int>());
        } else {
          error = "set_speed needs mode 'auto', or mode 'manual' with speed 0..6";
        }
      } else if (strcmp(action, "test_ir") == 0) {
        if (!has_speed) error = "test_ir needs speed 0..6";
        cmd.type = BridgeCmdType::TEST_IR;
        cmd.speed = has_speed ? static_cast<uint8_t>(sp.as<int>()) : 0;
      } else if (strcmp(action, "reboot") == 0) {
        cmd.type = BridgeCmdType::REBOOT;
      } else {
        error = "unknown action (set_speed | test_ir | reboot)";
      }
    }
  }
  arena.reset();
  if (error != nullptr) return send_error(req, "400 Bad Request", error);
  if (!post(cmd, pdMS_TO_TICKS(100))) return send_error(req, "503 Service Unavailable", "busy, retry");
  static const char kOk[] = "{\"accepted\":true}";
  return send_json(req, "202 Accepted", kOk, sizeof(kOk) - 1);
}

// POST /api/v1/pair: only succeeds inside the button-opened pairing window.
// Returns the new token once; it replaces any previous token.
esp_err_t h_pair(httpd_req_t* req) {
  // DNS rebinding / CSRF guard (SRS §5.3): checked before the window is used.
  char host[72], origin[96];
  const char* h = header(req, "Host", host, sizeof(host));
  const char* o = header(req, "Origin", origin, sizeof(origin));
  const bool origin_too_long = o == nullptr && httpd_req_get_hdr_value_len(req, "Origin") > 0;
  if (origin_too_long || !auth::pairing_request_allowed(h, o, kMdnsHost, g_self->device_ipv4())) {
    ESP_LOGW(kTag, "REST API: pairing request rejected (Host/Origin not this device)");
    return reject(req, "403 Forbidden", "pairing must be requested directly from atoms3-iaq.local or the device IP");
  }
  char token[auth::kTokenChars + 1];
  if (!g_self->auth_try_pair(token)) {
    return reject(req, "403 Forbidden", "pairing mode is not active: hold the device button 3-10 s, then retry");
  }
  // Persisted by the loop (auth_unsaved_token), retried until the NVS write succeeds.
  char body[96];
  const int n = snprintf(body, sizeof(body), "{\"token\":\"%s\",\"token_type\":\"Bearer\"}", token);
  auth::secure_zero(token, sizeof(token));
  ESP_LOGW(kTag, "REST API client paired; previous token revoked");  // never log the token itself
  if (req->content_len > 0) httpd_resp_set_hdr(req, "Connection", kConnClose);
  send_json(req, "200 OK", body, static_cast<size_t>(n));
  auth::secure_zero(body, sizeof(body));
  return finish_unread(req);
}

esp_err_t h_root(httpd_req_t* req) {
  static const char kIdx[] =
      "{\"device\":\"atoms3-iaq-matter-controller\",\"endpoints\":[\"GET /api/v1/status\",\"GET /api/v1/config\","
      "\"PATCH /api/v1/config (auth)\",\"POST /api/v1/command (auth)\",\"POST /api/v1/ota (auth)\","
      "\"POST /api/v1/pair (pairing mode)\"],\"auth\":\"Authorization: Bearer <token>\"}";
  return send_json(req, "200 OK", kIdx, sizeof(kIdx) - 1);
}

constexpr size_t kOtaChunkBytes = 2048;

esp_err_t h_ota(httpd_req_t* req) {
  if (g_self->auth_require(req) != 0) return finish_unread(req);  // 401 / 429 already sent

  if (g_ota_busy.exchange(true)) {
    return reject(req, "503 Service Unavailable", "OTA update already in progress");
  }
  struct BusyGuard {
    bool keep = false;
    uint8_t* buf = nullptr;
    ~BusyGuard() {
      free(buf);
      if (!keep) g_ota_busy = false;
    }
  } busy_guard;

  if (req->content_len == 0) {
    httpd_resp_set_hdr(req, "Connection", kConnClose);
    send_error(req, "411 Length Required", "Content-Length required");
    return ESP_FAIL;
  }

  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (part == nullptr) {
    return reject(req, "500 Internal Server Error", "no OTA partition available");
  }
  if (req->content_len < kOtaMinImageBytes) {
    return reject(req, "400 Bad Request", "firmware image too small");
  }
  if (!ota_size_is_valid(req->content_len, part->size)) {
    return reject(req, "413 Payload Too Large", "firmware exceeds OTA partition size");
  }

  busy_guard.buf = static_cast<uint8_t*>(malloc(kOtaChunkBytes));
  if (busy_guard.buf == nullptr) {
    return reject(req, "503 Service Unavailable", "low memory for OTA buffer");
  }
  uint8_t* ota_buf = busy_guard.buf;

  esp_ota_handle_t handle = 0;
  bool ota_started = false;
  size_t remaining = req->content_len;
  size_t received = 0;
  size_t ota_buf_used = 0;
  int timeouts = 0;
  size_t next_log_pct = 25;

  while (remaining > 0) {
    const size_t space = !ota_started ? (kOtaChunkBytes - ota_buf_used) : kOtaChunkBytes;
    const size_t to_read = remaining < space ? remaining : space;
    char* dst = !ota_started ? reinterpret_cast<char*>(ota_buf + ota_buf_used)
                             : reinterpret_cast<char*>(ota_buf);
    const int r = httpd_req_recv(req, dst, to_read);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts <= kOtaRecvRetries) {
        vTaskDelay(1);
        continue;
      }
      ESP_LOGE(kTag, "OTA receive timed out at %u/%u bytes", static_cast<unsigned>(received),
               static_cast<unsigned>(req->content_len));
      if (ota_started) esp_ota_abort(handle);
      httpd_resp_set_hdr(req, "Connection", kConnClose);
      send_error(req, "408 Request Timeout", "OTA upload timed out");
      return ESP_FAIL;
    }
    if (r <= 0) {
      ESP_LOGE(kTag, "OTA connection aborted at %u/%u bytes", static_cast<unsigned>(received),
               static_cast<unsigned>(req->content_len));
      if (ota_started) esp_ota_abort(handle);
      return ESP_FAIL;
    }
    timeouts = 0;
    received += static_cast<size_t>(r);
    remaining -= static_cast<size_t>(r);

    size_t write_len = static_cast<size_t>(r);
    if (!ota_started) {
      ota_buf_used += static_cast<size_t>(r);
      static_assert(kOtaImageMagic == ESP_IMAGE_HEADER_MAGIC, "OTA magic mismatch");
      static_assert(kOtaChipIdEsp32S3 == ESP_CHIP_ID_ESP32S3, "OTA chip_id mismatch");
      if (ota_buf_used >= 1 && ota_buf[0] != kOtaImageMagic) {
        ESP_LOGW(kTag, "OTA rejected: invalid image magic (0x%02X)", ota_buf[0]);
        return reject(req, "400 Bad Request", "invalid firmware image header");
      }
      if (ota_buf_used < kOtaHeaderBytes) continue;
      if (!ota_header_is_valid(ota_buf, ota_buf_used)) {
        const uint16_t cid =
            static_cast<uint16_t>(ota_buf[12]) | (static_cast<uint16_t>(ota_buf[13]) << 8);
        ESP_LOGW(kTag, "OTA rejected: wrong chip_id (0x%04X)", cid);
        return reject(req, "400 Bad Request", "firmware image is not for ESP32-S3");
      }
      const esp_err_t berr = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
      if (berr != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(berr));
        return reject(req, "500 Internal Server Error", "esp_ota_begin failed");
      }
      ota_started = true;
      write_len = ota_buf_used;
      ESP_LOGW(kTag, "OTA upload started: %u bytes -> %s",
               static_cast<unsigned>(req->content_len), part->label);
    }

    const esp_err_t werr = esp_ota_write(handle, ota_buf, write_len);
    if (werr != ESP_OK) {
      ESP_LOGE(kTag, "esp_ota_write failed at %u: %s", static_cast<unsigned>(received), esp_err_to_name(werr));
      esp_ota_abort(handle);
      return reject(req, "500 Internal Server Error", "flash write failed");
    }

    const size_t pct = (received * 100u) / req->content_len;
    if (pct >= next_log_pct) {
      ESP_LOGI(kTag, "OTA progress: %u%% (%u/%u bytes)", static_cast<unsigned>(pct),
               static_cast<unsigned>(received), static_cast<unsigned>(req->content_len));
      next_log_pct += 25;
    }
    vTaskDelay(1);  // yield so loop() and net_task keep feeding their 15 s TWDTs
  }

  if (!ota_started) {
    httpd_resp_set_hdr(req, "Connection", kConnClose);
    send_error(req, "400 Bad Request", "incomplete firmware header");
    return ESP_OK;
  }

  const esp_err_t eerr = esp_ota_end(handle);
  if (eerr != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(eerr));
    httpd_resp_set_hdr(req, "Connection", kConnClose);
    send_error(req, "400 Bad Request", "firmware validation failed");
    return ESP_OK;
  }

  const esp_err_t serr = esp_ota_set_boot_partition(part);
  if (serr != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(serr));
    httpd_resp_set_hdr(req, "Connection", kConnClose);
    send_error(req, "500 Internal Server Error", "failed to set boot partition");
    return ESP_OK;
  }

  ESP_LOGW(kTag, "OTA complete (%u bytes) -> %s; scheduling reboot",
           static_cast<unsigned>(received), part->label);
  busy_guard.keep = true;  // stay busy until the scheduled reboot resets the device
  httpd_resp_set_hdr(req, "Connection", kConnClose);
  static const char kOtaOk[] = "{\"status\":\"ok\",\"rebooting\":true}";
  send_json(req, "200 OK", kOtaOk, sizeof(kOtaOk) - 1);
  if (!post(BridgeCmd{BridgeCmdType::REBOOT, 0, false, 0, 0}, pdMS_TO_TICKS(500))) {
    ESP_LOGW(kTag, "reboot command queue full; requesting reboot via loop flag");
    g_self->request_reboot();
  }
  return ESP_OK;
}
}  // namespace

// ---------------------------------------------------------------------------
bool MatterHaBridge::begin(AppConfig* cfg, SemaphoreHandle_t cfg_mutex, QueueHandle_t cmd_queue) {
  g_self = this;
  cfg_ = cfg;
  cfg_mutex_ = cfg_mutex;
  cmd_q_ = cmd_queue;
  status_mutex_ = xSemaphoreCreateMutex();
  if (auth_mutex_ == nullptr) auth_mutex_ = xSemaphoreCreateMutex();

  Matter.selectNetwork(MATTER_NETWORK_WIFI);
  Matter.setVendorName("DIY");
  Matter.setProductName("AtomS3 IAQ Purifier Controller");
  Matter.setBLEMemoryReleaseEnabled(true);  // library releases BLE RAM safely after commissioning
  ble_released_.store(!Matter.isBLECommissioningEnabled());
  Matter.onEvent([](matterEvent_t ev, const chip::DeviceLayer::ChipDeviceEvent*) {
    if (g_self == nullptr) return;
    switch (ev) {
      case MATTER_COMMISSIONING_SESSION_STARTED:
        g_self->commissioning_active_.store(true);
        break;
      case MATTER_COMMISSIONING_SESSION_STOPPED:
        if (!chip::Server::GetInstance().GetFailSafeContext().IsFailSafeArmed()) {
          g_self->commissioning_active_.store(false);
        }
        break;
      case MATTER_COMMISSIONING_COMPLETE:
        g_commissioned.store(true);
        g_self->commissioning_active_.store(false);
        break;
      case MATTER_FAIL_SAFE_TIMER_EXPIRED:
        g_self->commissioning_active_.store(false);
        break;
      case MATTER_BLE_DEINITIALIZED:
        g_self->commissioning_active_.store(false);
        g_self->ble_released_.store(true);
        break;
      default:
        break;
    }
  });
  Matter.onBLEMemoryReleased([]() {
    if (g_self != nullptr) {
      g_self->ble_released_.store(true);
      g_self->commissioning_active_.store(false);
    }
    ESP_LOGI(kTag, "BLE memory released (free=%u)", static_cast<unsigned>(esp_get_free_heap_size()));
  });

  // EP1: purifier fan (Off/Low/Med/High/Auto + percent slider)
  if (!g_fan.begin(0, MatterFan::FAN_MODE_AUTO, MatterFan::FAN_MODE_SEQ_OFF_LOW_MED_HIGH_AUTO)) return false;
#if IAQ_MATTER_AIR_PURIFIER_TYPE
  endpoint_t* fan_ep = endpoint::get(node::get(), g_fan.getEndPointId());
  if (fan_ep != nullptr) {
    endpoint::add_device_type(fan_ep, endpoint::air_purifier::get_device_type_id(),
                              endpoint::air_purifier::get_device_type_version());
  }
#endif
  g_fan.onChangeMode([](MatterFan::FanMode_t m) -> bool {
    if (g_publishing) return true;
    switch (m) {
      case MatterFan::FAN_MODE_AUTO: post(BridgeCmd{BridgeCmdType::SET_AUTO, 0, false, 0, 0}); break;
      case MatterFan::FAN_MODE_OFF: post(BridgeCmd{BridgeCmdType::SET_MANUAL, 0, false, 0, 0}); break;
      case MatterFan::FAN_MODE_LOW: post(BridgeCmd{BridgeCmdType::SET_MANUAL, 2, false, 0, 0}); break;
      case MatterFan::FAN_MODE_MEDIUM: post(BridgeCmd{BridgeCmdType::SET_MANUAL, 4, false, 0, 0}); break;
      default: post(BridgeCmd{BridgeCmdType::SET_MANUAL, 6, false, 0, 0}); break;  // High / On
    }
    return true;
  });
  g_fan.onChangeSpeedPercent([](uint8_t pct) -> bool {
    if (g_publishing || pct > 100) return true;  // self-update, or null/invalid PercentSetting
    post(BridgeCmd{BridgeCmdType::SET_MANUAL, static_cast<uint8_t>(iaq::percent_to_speed(pct)), false, 0, 0});
    return true;
  });

  // EP2: composite air quality sensor
  if (!g_aq.begin()) return false;

  // EP3: Night Quiet Lock
  if (!g_night_lock.begin(false)) return false;
  g_night_lock.onChangeOnOff([](bool on) -> bool {
    if (g_publishing) return true;
    post(BridgeCmd{BridgeCmdType::SET_NIGHT_FORCE, 0, on, 0, 0});
    return true;
  });

  // EP0: Time Synchronization cluster (0x0038) for Tier-3 clock sync.
  endpoint_t* root_ep = endpoint::get(node::get(), 0);
  if (root_ep != nullptr) {
    cluster::time_synchronization::config_t ts_cfg;
    ts_cfg.delegate = &g_time_sync_delegate;
    if (cluster::time_synchronization::create(root_ep, &ts_cfg, CLUSTER_FLAG_SERVER) == nullptr) {
      ESP_LOGW(kTag, "TimeSynchronization cluster create failed");
    }
  }

  Matter.begin();
  started_ = true;
  g_commissioned = Matter.isDeviceCommissioned();

  if (!Matter.isDeviceCommissioned()) {
    snprintf(g_onboarding_code, sizeof(g_onboarding_code), "%s", Matter.getManualPairingCode().c_str());
    snprintf(g_onboarding_qr, sizeof(g_onboarding_qr), "%s", Matter.getOnboardingQRCodeUrl().c_str());
    g_last_onboarding_print_ms = millis();
    print_onboarding_banner();
  } else {
    ESP_LOGI(kTag, "Matter commissioned");
  }
  return true;
}

bool MatterHaBridge::start_http() {
  if (httpd_ != nullptr) return true;
  if (g_http == nullptr) {
    g_http = static_cast<HttpBuffers*>(calloc(1, sizeof(HttpBuffers)));
    if (g_http == nullptr) {
      ESP_LOGE(kTag, "no heap for HTTP buffers (free=%u)", static_cast<unsigned>(esp_get_free_heap_size()));
      return false;
    }
  }
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
  c.ctrl_port = kHttpCtrlPort;
  c.stack_size = kHttpStackBytes;
  c.max_uri_handlers = kHttpMaxUriHandlers;
  c.lru_purge_enable = true;
  c.max_open_sockets = kHttpMaxSockets;
  c.recv_wait_timeout = kHttpRecvTimeoutS;
  c.send_wait_timeout = kHttpSendTimeoutS;
  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &c) != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed");
    return false;
  }
  const httpd_uri_t uris[] = {
      {"/", HTTP_GET, h_root, nullptr},
      {"/api/v1/status", HTTP_GET, h_status, nullptr},
      {"/api/v1/config", HTTP_GET, h_config_get, nullptr},
      {"/api/v1/config", HTTP_PATCH, h_config_patch, nullptr},
      {"/api/v1/command", HTTP_POST, h_command, nullptr},
      {"/api/v1/ota", HTTP_POST, h_ota, nullptr},
      {"/api/v1/pair", HTTP_POST, h_pair, nullptr},
  };
  for (const auto& u : uris) httpd_register_uri_handler(h, &u);
  httpd_ = h;
  http_started_.store(true);
  ESP_LOGI(kTag, "REST API on port 80");
  return true;
}

void MatterHaBridge::update_mdns(uint32_t ipv4) {
  if (ipv4 != 0) device_ip_ = ipv4;  // pairing Host check (httpd task reads it)
  if (ipv4 == 0 || (mdns_registered_ && ipv4 == mdns_ip_)) return;
  const esp_err_t init = mdns_init();  // ESP_ERR_INVALID_STATE if the Matter stack already did it
  if (init != ESP_OK && init != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "mdns_init: %s", esp_err_to_name(init));
    return;
  }
  mdns_ip_addr_t addr = {};
  addr.addr.type = ESP_IPADDR_TYPE_V4;
  addr.addr.u_addr.ip4.addr = ipv4;
  addr.next = nullptr;
  if (mdns_registered_) {
    mdns_delegate_hostname_set_address(kMdnsHost, &addr);
  } else if (mdns_delegate_hostname_add(kMdnsHost, &addr) == ESP_OK) {
    mdns_service_add_for_host("AtomS3 IAQ", "_http", "_tcp", kMdnsHost, 80, nullptr, 0);
    mdns_registered_ = true;
  }
  mdns_ip_ = ipv4;
  ESP_LOGI(kTag, "mDNS: http://%s.local", kMdnsHost);
}

void MatterHaBridge::copy_status(StatusSnapshot* out) const {
  xSemaphoreTake(status_mutex_, portMAX_DELAY);
  *out = status_;
  xSemaphoreGive(status_mutex_);
}

void MatterHaBridge::publish(const StatusSnapshot& s) {
  xSemaphoreTake(status_mutex_, portMAX_DELAY);
  status_ = s;
  matter_pending_ = s;
  xSemaphoreGive(status_mutex_);
  if (!g_commissioned.load() && g_onboarding_code[0] != '\0') {
    const uint32_t now = millis();
    if (now - g_last_onboarding_print_ms >= kOnboardingReprintMs) {
      g_last_onboarding_print_ms = now;
      print_onboarding_banner();
    }
  }
  // Never block the loop on the CHIP lock: hand the update to the CHIP task.
  if (started_ && !g_work_scheduled.exchange(true)) {
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(publish_work, reinterpret_cast<intptr_t>(this)) != CHIP_NO_ERROR) {
      g_work_scheduled = false;
    }
  }
}

void MatterHaBridge::publish_work(intptr_t arg) {
  MatterHaBridge* self = reinterpret_cast<MatterHaBridge*>(arg);
  g_work_scheduled = false;
  StatusSnapshot s;
  xSemaphoreTake(self->status_mutex_, portMAX_DELAY);
  s = self->matter_pending_;
  xSemaphoreGive(self->status_mutex_);
  g_commissioned = Matter.isDeviceCommissioned();
  g_fabrics = static_cast<int>(chip::Server::GetInstance().GetFabricTable().FabricCount());
  self->publish_matter(s);
}

void MatterHaBridge::publish_matter(const StatusSnapshot& s) {
  g_publishing = true;  // CHIP task only (see publish_work)

  // EP1 fan
  const int mode = static_cast<int>(s.operating_mode);
  if (s.current_speed != pub_speed_ || mode != pub_mode_) {
    MatterFan::FanMode_t fm;
    if (s.operating_mode != OperatingMode::MANUAL) {
      fm = MatterFan::FAN_MODE_AUTO;
    } else if (s.current_speed == 0) {
      fm = MatterFan::FAN_MODE_OFF;
    } else if (s.current_speed <= 2) {
      fm = MatterFan::FAN_MODE_LOW;
    } else if (s.current_speed <= 4) {
      fm = MatterFan::FAN_MODE_MEDIUM;
    } else {
      fm = MatterFan::FAN_MODE_HIGH;
    }
    g_fan.setMode(fm);
    if (fm == MatterFan::FAN_MODE_AUTO) {
      // In Auto the FanControl server keeps PercentSetting null; writing it would
      // flip the mode (0 -> Off). Report the actual speed via PercentCurrent only.
      esp_matter_attr_val_t pc = esp_matter_uint8(iaq::speed_to_percent(s.current_speed));
      attribute::update(g_fan.getEndPointId(), FanControl::Id, FanControl::Attributes::PercentCurrent::Id, &pc);
    } else {
      g_fan.setSpeedPercent(iaq::speed_to_percent(s.current_speed));  // after mode: overrides presets
    }
    pub_speed_ = s.current_speed;
    pub_mode_ = mode;
  }

  // EP3 night lock mirrors NIGHT_FORCE
  const bool night_lock = s.operating_mode == OperatingMode::NIGHT_FORCE;
  if (!pub_night_valid_ || night_lock != pub_night_lock_) {
    g_night_lock.setOnOff(night_lock);
    pub_night_lock_ = night_lock;
    pub_night_valid_ = true;
  }

  // EP2 sensors (unavailable while the AirGradient is unreachable)
  const bool avail = s.have_reading && !s.alarm_sensor_unreachable;
  const float pm = avail ? s.pm25_sma5 : NAN;
  const int aq = matter_air_quality_from_pm25(pm);
  if (aq != pub_aq_) {
    if (g_aq.set_air_quality(static_cast<uint8_t>(aq))) pub_aq_ = aq;
  }
  auto changed = [](float a, float b, float eps) { return (isnan(a) != isnan(b)) || (!isnan(a) && fabsf(a - b) >= eps); };
  if (changed(pm, pub_pm_, 0.1f)) {
    g_aq.set_float(Pm25ConcentrationMeasurement::Id, Pm25ConcentrationMeasurement::Attributes::MeasuredValue::Id,
                   !isnan(pm), pm);
    pub_pm_ = pm;
  }
  const float co2 = avail ? s.co2_ppm : NAN;
  if (changed(co2, pub_co2_, 1.0f)) {
    g_aq.set_float(CarbonDioxideConcentrationMeasurement::Id,
                   CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, !isnan(co2), co2);
    pub_co2_ = co2;
  }
  const int tl = matter_level_from_tvoc_index(avail ? s.tvoc_sma5 : NAN);
  if (tl != pub_tvoc_lvl_) {
    g_aq.set_enum8(TotalVolatileOrganicCompoundsConcentrationMeasurement::Id,
                   TotalVolatileOrganicCompoundsConcentrationMeasurement::Attributes::LevelValue::Id,
                   static_cast<uint8_t>(tl));
    pub_tvoc_lvl_ = tl;
  }
  const int nl = matter_level_from_nox_index(avail ? s.nox_sma5 : NAN);
  if (nl != pub_no2_lvl_) {
    g_aq.set_enum8(NitrogenDioxideConcentrationMeasurement::Id,
                   NitrogenDioxideConcentrationMeasurement::Attributes::LevelValue::Id, static_cast<uint8_t>(nl));
    pub_no2_lvl_ = nl;
  }
  const float tc = avail ? s.temperature_c : NAN;
  if (changed(tc, pub_temp_, 0.1f)) {
    g_aq.set_temperature(!isnan(tc), tc);
    pub_temp_ = tc;
  }
  const float rh = avail ? s.humidity_pct : NAN;
  if (changed(rh, pub_rh_, 0.5f)) {
    g_aq.set_humidity(!isnan(rh), rh);
    pub_rh_ = rh;
  }
  if (!pub_fault_valid_ || s.alarm_sensor_unreachable != pub_fault_) {
    g_aq.set_fault(s.alarm_sensor_unreachable);
    pub_fault_ = s.alarm_sensor_unreachable;
    pub_fault_valid_ = true;
  }

  g_publishing = false;
}

bool MatterHaBridge::commissioned() const { return g_commissioned.load(); }

int MatterHaBridge::fabric_count() const { return g_fabrics.load(); }

bool MatterHaBridge::matter_utc(time_t* utc) const {
  if (utc == nullptr) return false;
  if (!chip::DeviceLayer::PlatformMgr().TryLockChipStack()) return false;  // never block the net task
  bool ok = false;
  esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
  attribute_t* a = attribute::get(0, TimeSynchronization::Id, TimeSynchronization::Attributes::UTCTime::Id);
  if (a != nullptr && attribute::get_val(a, &v) == ESP_OK &&
      v.type == ESP_MATTER_VAL_TYPE_NULLABLE_UINT64) {
    nullable<uint64_t> us(v.val.u64);
    if (!us.is_null()) {
      *utc = clk::matter_utc_us_to_unix(us.value());
      ok = true;
    }
  }
  if (!ok && g_matter_utc_synced.load()) {
    const int64_t base = g_matter_utc_unix.load();
    if (base >= clk::kValidEpoch) {
      const uint32_t elapsed_s = (millis() - g_matter_utc_set_ms.load()) / 1000u;
      *utc = static_cast<time_t>(base + static_cast<int64_t>(elapsed_s));
      ok = true;
    } else {
      chip::System::Clock::Microseconds64 cur_us;
      if (chip::System::SystemClock().GetClock_RealTime(cur_us) == CHIP_NO_ERROR) {
        *utc = static_cast<time_t>(cur_us.count() / 1000000ULL);
        ok = true;
      }
    }
  }
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  return ok;
}

void MatterHaBridge::decommission() {
  ESP_LOGW(kTag, "decommissioning Matter (factory reset of fabrics)");
  Matter.decommission();
}

// ---------------------------------------------------------------------------
// REST authentication (SRS §5.3). The pure state machine lives in api_auth;
// here it is guarded by auth_mutex_ (loop task + httpd task). The token is
// never logged.
// ---------------------------------------------------------------------------
void MatterHaBridge::auth_set_active(const char* token) {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  auth_.set_active(token);
  const bool has = auth_.has_token();
  xSemaphoreGive(auth_mutex_);
  ESP_LOGI(kTag, "REST API auth: %s", has ? "paired token loaded" : "no token yet (hold button 3-10 s to pair)");
}

void MatterHaBridge::auth_open_pairing(uint32_t now_ms) {
  uint8_t rnd[auth::kTokenBytes];
  esp_fill_random(rnd, sizeof(rnd));  // hardware RNG (RF subsystem running)
  char candidate[auth::kTokenChars + 1];
  auth::token_from_bytes(rnd, candidate);
  auth::secure_zero(rnd, sizeof(rnd));
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  auth_.open_pairing(candidate, now_ms);
  xSemaphoreGive(auth_mutex_);
  auth::secure_zero(candidate, sizeof(candidate));
  ESP_LOGW(kTag, "REST API pairing mode open for %u s: POST /api/v1/pair",
           static_cast<unsigned>(auth::kPairWindowMs / 1000));
}

bool MatterHaBridge::auth_pairing_open(uint32_t now_ms) {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  const bool open = auth_.pairing_open(now_ms);
  xSemaphoreGive(auth_mutex_);
  return open;
}

bool MatterHaBridge::auth_has_token() {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  const bool has = auth_.has_token();
  xSemaphoreGive(auth_mutex_);
  return has;
}

bool MatterHaBridge::auth_unsaved_token(char* out33, uint32_t* generation) {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  const uint32_t gen = auth_.generation();
  const bool unsaved = gen != auth_saved_gen_;
  if (unsaved) {
    auth_.copy_active(out33);
    *generation = gen;
  }
  xSemaphoreGive(auth_mutex_);
  return unsaved;
}

void MatterHaBridge::auth_mark_saved(uint32_t generation) {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  auth_saved_gen_ = generation;  // a newer pairing (higher generation) stays unsaved
  auth_unsaved_.store(auth_.generation() != auth_saved_gen_);
  xSemaphoreGive(auth_mutex_);
}

void MatterHaBridge::auth_clear() {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  auth_.clear();
  auth_saved_gen_ = auth_.generation();  // nothing left to persist
  auth_unsaved_.store(false);
  xSemaphoreGive(auth_mutex_);
}

bool MatterHaBridge::auth_try_pair(char* out33) {
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  const bool ok = auth_.try_pair(millis(), out33) == auth::Pair::OK;
  if (ok) auth_unsaved_.store(true);
  xSemaphoreGive(auth_mutex_);
  return ok;
}

int MatterHaBridge::auth_require(void* httpd_req) {
  httpd_req_t* req = static_cast<httpd_req_t*>(httpd_req);
  char hdr[96];
  const char* value = nullptr;
  const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len > 0 && len < sizeof(hdr) && httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
    value = hdr;
  } else if (len >= sizeof(hdr)) {
    value = "invalid";  // oversized: counts as a bad token
  }
  const uint32_t now = millis();
  const uint32_t peer = peer_key(req);
  xSemaphoreTake(auth_mutex_, portMAX_DELAY);
  const auth::Check c = auth_.check(value, peer, now);
  const uint32_t retry_ms = auth_.lockout_remaining_ms(peer, now);
  xSemaphoreGive(auth_mutex_);
  auth::secure_zero(hdr, sizeof(hdr));
  if (c == auth::Check::OK) return 0;
  // The caller returns finish_unread(): the connection closes if a body was sent.
  if (req->content_len > 0) httpd_resp_set_hdr(req, "Connection", kConnClose);
  if (c == auth::Check::LOCKED) {
    char retry[12];
    snprintf(retry, sizeof(retry), "%u", static_cast<unsigned>((retry_ms + 999) / 1000));
    httpd_resp_set_hdr(req, "Retry-After", retry);
    send_error(req, "429 Too Many Requests", "too many invalid tokens; retry later");
    return 429;
  }
  if (c == auth::Check::INVALID) ESP_LOGW(kTag, "REST API: rejected invalid token");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"atoms3-iaq\"");
  send_error(req, "401 Unauthorized",
             c == auth::Check::MISSING ? "authorization required: pair via POST /api/v1/pair"
                                       : "invalid token: re-pair via the device button");
  return 401;
}
#endif  // UNIT_TEST
