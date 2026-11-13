// api_auth.cpp: pure implementation (host + device).
#include "api_auth.h"

#include <stdio.h>
#include <string.h>

namespace auth {

namespace {
bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Constant-time comparison of two kTokenChars strings (no early exit on mismatch).
bool token_equal(const char* a, const char* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kTokenChars; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

// Case-insensitive match of host[0..len) against `want`, after dropping an
// optional ":80" port and a trailing dot.
bool host_matches(const char* host, size_t len, const char* want) {
  if (len >= 3 && memcmp(host + len - 3, ":80", 3) == 0) len -= 3;
  if (len >= 1 && host[len - 1] == '.') --len;
  if (len == 0 || strlen(want) != len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (lower(host[i]) != lower(want[i])) return false;
  }
  return true;
}

bool host_is_device(const char* host, size_t len, const char* mdns_host, uint32_t ipv4) {
  char want[64];
  if (mdns_host != nullptr && mdns_host[0] != '\0') {
    snprintf(want, sizeof(want), "%s.local", mdns_host);
    if (host_matches(host, len, want)) return true;
  }
  if (ipv4 != 0) {  // lwIP byte order: first octet in the low byte
    snprintf(want, sizeof(want), "%u.%u.%u.%u", static_cast<unsigned>(ipv4 & 0xFF),
             static_cast<unsigned>((ipv4 >> 8) & 0xFF), static_cast<unsigned>((ipv4 >> 16) & 0xFF),
             static_cast<unsigned>((ipv4 >> 24) & 0xFF));
    if (host_matches(host, len, want)) return true;
  }
  return false;
}
}  // namespace

void token_from_bytes(const uint8_t bytes[kTokenBytes], char out[kTokenChars + 1]) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kTokenBytes; ++i) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 0x0F];
  }
  out[kTokenChars] = '\0';
}

bool token_is_well_formed(const char* s) {
  if (s == nullptr) return false;
  for (size_t i = 0; i < kTokenChars; ++i) {
    if (!is_hex_lower(s[i])) return false;
  }
  return s[kTokenChars] == '\0';
}

uint32_t peer_key_ipv4(uint32_t ipv4) { return ipv4; }

uint32_t peer_key_ipv6(const uint8_t a[16]) {
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  if (memcmp(a, kMapped, sizeof(kMapped)) == 0) {  // ::ffff:a.b.c.d -> same key as IPv4
    return static_cast<uint32_t>(a[12]) | (static_cast<uint32_t>(a[13]) << 8) |
           (static_cast<uint32_t>(a[14]) << 16) | (static_cast<uint32_t>(a[15]) << 24);
  }
  uint32_t h = 2166136261u;  // FNV-1a
  for (int i = 0; i < 16; ++i) {
    h ^= a[i];
    h *= 16777619u;
  }
  return h;
}

bool pairing_request_allowed(const char* host, const char* origin, const char* mdns_host, uint32_t device_ipv4) {
  if (host == nullptr || host[0] == '\0') return false;  // HTTP/1.1 clients always send Host
  if (!host_is_device(host, strlen(host), mdns_host, device_ipv4)) return false;
  if (origin == nullptr || origin[0] == '\0') return true;  // non-browser client (curl, HA)
  static const char kScheme[] = "http://";
  if (strncmp(origin, kScheme, sizeof(kScheme) - 1) != 0) return false;  // incl. "null"
  const char* o = origin + sizeof(kScheme) - 1;
  return host_is_device(o, strlen(o), mdns_host, device_ipv4);
}

void ApiAuth::set_active(const char* token) {
  if (token_is_well_formed(token)) {
    memcpy(active_, token, kTokenChars + 1);
  } else {
    active_[0] = '\0';
  }
}

void ApiAuth::clear() {
  secure_zero(active_, sizeof(active_));
  close_pairing();
  for (Peer& p : peers_) p = Peer{};
}

void ApiAuth::open_pairing(const char candidate[kTokenChars + 1], uint32_t now_ms) {
  if (!token_is_well_formed(candidate)) return;
  memcpy(candidate_, candidate, kTokenChars + 1);
  window_open_ = true;
  window_start_ms_ = now_ms;
}

void ApiAuth::close_pairing() {
  window_open_ = false;
  secure_zero(candidate_, sizeof(candidate_));
}

// Clears every timed state whose period has elapsed. Called on every public
// time-based query; the loop polls pairing_open() continuously, so expired
// state is cleared within milliseconds and a millis() wrap (~49.7 days) can
// never make an old window or lockout look active again.
void ApiAuth::expire(uint32_t now_ms) {
  if (window_open_ && (now_ms - window_start_ms_) >= kPairWindowMs) close_pairing();
  for (Peer& p : peers_) {
    if (!p.used) continue;
    if (p.locked && (now_ms - p.lock_start_ms) >= kLockoutMs) {
      p.locked = false;
      p.failures = 0;
    }
    if (p.failures > 0 && (now_ms - p.first_failure_ms) >= kFailureWindowMs) p.failures = 0;
    if (!p.locked && p.failures == 0) p = Peer{};  // nothing left to remember
  }
}

ApiAuth::Peer* ApiAuth::find(uint32_t key) {
  for (Peer& p : peers_) {
    if (p.used && p.key == key) return &p;
  }
  return nullptr;
}

// Returns the entry for `key`, creating it if needed. When the table is full,
// the least recently active unlocked entry is replaced (or, if every entry is
// locked, the oldest lockout).
ApiAuth::Peer* ApiAuth::find_or_add(uint32_t key, uint32_t now_ms) {
  if (Peer* p = find(key)) return p;
  Peer* victim = nullptr;
  for (Peer& p : peers_) {
    if (!p.used) {
      victim = &p;
      break;
    }
  }
  if (victim == nullptr) {
    for (Peer& p : peers_) {
      if (!p.locked && (victim == nullptr || (now_ms - p.last_ms) > (now_ms - victim->last_ms))) victim = &p;
    }
  }
  if (victim == nullptr) {
    for (Peer& p : peers_) {
      if (victim == nullptr || (now_ms - p.lock_start_ms) > (now_ms - victim->lock_start_ms)) victim = &p;
    }
  }
  *victim = Peer{};
  victim->used = true;
  victim->key = key;
  return victim;
}

bool ApiAuth::pairing_open(uint32_t now_ms) {
  expire(now_ms);
  return window_open_;
}

uint32_t ApiAuth::pairing_remaining_ms(uint32_t now_ms) {
  return pairing_open(now_ms) ? kPairWindowMs - (now_ms - window_start_ms_) : 0;
}

Pair ApiAuth::try_pair(uint32_t now_ms, char out[kTokenChars + 1]) {
  if (!pairing_open(now_ms)) return Pair::CLOSED;
  memcpy(active_, candidate_, kTokenChars + 1);  // replaces (revokes) the previous token
  memcpy(out, candidate_, kTokenChars + 1);
  close_pairing();                               // one client per window
  for (Peer& p : peers_) p = Peer{};             // physical presence: drop all lockouts
  ++generation_;
  return Pair::OK;
}

Check ApiAuth::check(const char* authorization, uint32_t peer, uint32_t now_ms) {
  expire(now_ms);
  Peer* known = find(peer);
  if (known != nullptr && known->locked) return Check::LOCKED;
  if (authorization == nullptr || authorization[0] == '\0') return Check::MISSING;

  // "Bearer <token>": scheme is case-insensitive, then one or more spaces;
  // trailing spaces/tabs are optional whitespace (RFC 7230) and ignored.
  static const char kScheme[] = "bearer";
  const char* p = authorization;
  bool scheme_ok = true;
  for (size_t i = 0; i < sizeof(kScheme) - 1; ++i) {
    if (lower(p[i]) != kScheme[i]) {
      scheme_ok = false;
      break;
    }
  }
  const char* tok = nullptr;
  size_t tok_len = 0;
  if (scheme_ok && p[sizeof(kScheme) - 1] == ' ') {
    tok = p + sizeof(kScheme) - 1;
    while (*tok == ' ') ++tok;
    tok_len = strlen(tok);
    while (tok_len > 0 && (tok[tok_len - 1] == ' ' || tok[tok_len - 1] == '\t')) --tok_len;
  }

  const bool ok = has_token() && tok != nullptr && tok_len == kTokenChars && token_equal(tok, active_);
  if (ok) return Check::OK;  // failures keep counting in their own window (no reset on success)
  Peer* e = find_or_add(peer, now_ms);
  e->last_ms = now_ms;
  if (e->failures == 0) e->first_failure_ms = now_ms;
  if (++e->failures >= kMaxFailures) {
    e->locked = true;
    e->lock_start_ms = now_ms;
  }
  return Check::INVALID;
}

uint32_t ApiAuth::lockout_remaining_ms(uint32_t peer, uint32_t now_ms) {
  expire(now_ms);
  const Peer* e = find(peer);
  return (e != nullptr && e->locked) ? kLockoutMs - (now_ms - e->lock_start_ms) : 0;
}

void ApiAuth::copy_active(char out[kTokenChars + 1]) const { memcpy(out, active_, kTokenChars + 1); }

}  // namespace auth
