// api_auth.h: REST API bearer-token authentication with physical-presence
// pairing (SRS §5.3). Pure C++ (no Arduino / ESP-IDF) so it is host-testable;
// the caller supplies time, randomness, the client's address and locking.
//
// Model (Philips Hue / Nanoleaf style):
//   - The device has at most one active token (32 hex chars = 128 bits).
//   - A button hold of 3–10 s opens a pairing window with a freshly generated
//     candidate token. The first POST /api/v1/pair inside the window receives it;
//     it then replaces the previous token and the window closes.
//   - /api/v1/pair only accepts requests addressed to the device itself (Host)
//     and not sent from another web origin (Origin): blocks DNS rebinding / CSRF.
//   - Mutating endpoints require "Authorization: Bearer <token>".
//   - Repeated bad tokens lock out the offending client address only.
//   - No token stored (new device / after factory reset) -> every check fails.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace auth {

constexpr size_t kTokenBytes = 16;                  // 128-bit random token
constexpr size_t kTokenChars = kTokenBytes * 2;     // lowercase hex
constexpr uint32_t kPairWindowMs = 60000;           // pairing window after the button hold
constexpr int kMaxFailures = 5;                     // bad tokens (per client) before lockout
constexpr uint32_t kFailureWindowMs = 60000;        // failures counted within this window
constexpr uint32_t kLockoutMs = 60000;              // lockout duration
constexpr size_t kMaxTrackedPeers = 8;              // client addresses tracked for lockout

// Hex-encodes kTokenBytes random bytes into out[kTokenChars + 1].
void token_from_bytes(const uint8_t bytes[kTokenBytes], char out[kTokenChars + 1]);
// True if `s` is exactly kTokenChars lowercase hex characters.
bool token_is_well_formed(const char* s);
// Non-optimizable wipe for stack/state buffers holding sensitive tokens.
inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
}

// Stable 32-bit key for a client address: the IPv4 address itself (also for
// IPv4-mapped IPv6), otherwise a hash of the 16-byte IPv6 address.
uint32_t peer_key_ipv4(uint32_t ipv4);
uint32_t peer_key_ipv6(const uint8_t addr[16]);

// Pairing request origin check (SRS §5.3). `host` is the Host header (required),
// `origin` the Origin header (nullptr / "" when absent, e.g. curl). Allowed:
// "<mdns_host>.local" or the device's dotted IPv4 (optional ":80", trailing dot,
// case-insensitive). A present Origin must be "http://" + an allowed host.
bool pairing_request_allowed(const char* host, const char* origin, const char* mdns_host, uint32_t device_ipv4);

enum class Check : uint8_t { OK, MISSING, INVALID, LOCKED };
enum class Pair : uint8_t { OK, CLOSED };

class ApiAuth {
 public:
  // Active token loaded from storage; nullptr / malformed -> no token (locked down).
  void set_active(const char* token);
  bool has_token() const { return active_[0] != '\0'; }
  void clear();  // factory reset: forget the token, close any window, drop lockouts

  // Button hold released after 3–10 s: open the window with a new candidate token.
  void open_pairing(const char candidate[kTokenChars + 1], uint32_t now_ms);
  void close_pairing();
  // Time-based queries also clear expired windows / lockouts (millis()-wrap safe).
  bool pairing_open(uint32_t now_ms);
  uint32_t pairing_remaining_ms(uint32_t now_ms);

  // POST /api/v1/pair (after pairing_request_allowed). On OK, `out` receives the
  // token, which becomes active.
  Pair try_pair(uint32_t now_ms, char out[kTokenChars + 1]);

  // Validates an Authorization header value (may be nullptr when absent) from
  // the client identified by `peer` (see peer_key_*).
  Check check(const char* authorization, uint32_t peer, uint32_t now_ms);
  uint32_t lockout_remaining_ms(uint32_t peer, uint32_t now_ms);

  // Copies the active token (for persisting after a pairing).
  void copy_active(char out[kTokenChars + 1]) const;
  // Increments on every successful pairing; the persister saves until it catches up.
  uint32_t generation() const { return generation_; }

 private:
  struct Peer {
    bool used = false;
    uint32_t key = 0;
    int failures = 0;
    uint32_t first_failure_ms = 0;
    bool locked = false;
    uint32_t lock_start_ms = 0;
    uint32_t last_ms = 0;
  };
  void expire(uint32_t now_ms);
  Peer* find(uint32_t key);
  Peer* find_or_add(uint32_t key, uint32_t now_ms);

  char active_[kTokenChars + 1] = {};
  char candidate_[kTokenChars + 1] = {};
  bool window_open_ = false;
  uint32_t window_start_ms_ = 0;
  Peer peers_[kMaxTrackedPeers];
  uint32_t generation_ = 0;
};

}  // namespace auth
