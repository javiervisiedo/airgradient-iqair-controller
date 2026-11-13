// sim_inject.cpp: Wokwi serial command parser (host-testable + WOKWI_SIM).
#include "sim_inject.h"

#if defined(WOKWI_SIM) || defined(UNIT_TEST)
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace sim {
namespace {

bool parse_float_range(const char* s, float lo, float hi, float* out) {
  if (s == nullptr || *s == '\0' || isspace(static_cast<unsigned char>(*s))) return false;
  char* end = nullptr;
  const float v = strtof(s, &end);
  if (end == s || *end != '\0' || !isfinite(v) || v < lo || v > hi) return false;
  *out = v;
  return true;
}

bool next_token(const char** cursor, char* out, size_t out_len) {
  const char* p = *cursor;
  while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) ++p;
  if (*p == '\0') {
    *cursor = p;
    return false;
  }
  size_t n = 0;
  while (*p != '\0' && !isspace(static_cast<unsigned char>(*p))) {
    if (n + 1 >= out_len) return false;
    out[n++] = *p++;
  }
  out[n] = '\0';
  *cursor = p;
  return true;
}

}  // namespace

bool parse_inject_line(const char* line, InjectCmd* out) {
  if (line == nullptr || out == nullptr) return false;
  const char* p = line;
  char tok[64];
  if (!next_token(&p, tok, sizeof(tok))) return false;
  if (strcmp(tok, "inject") != 0) return false;

  InjectCmd cmd;
  bool has_pm25 = false;
  while (true) {
    // Distinguish end-of-line from an oversized token.
    const char* saved = p;
    while (*saved != '\0' && isspace(static_cast<unsigned char>(*saved))) ++saved;
    if (*saved == '\0') break;
    if (!next_token(&p, tok, sizeof(tok))) return false;

    if (strcmp(tok, "fail") == 0) {
      if (cmd.fail || has_pm25) return false;
      cmd.fail = true;
      continue;
    }

    char* eq = strchr(tok, '=');
    if (eq == nullptr || eq == tok || eq[1] == '\0') return false;
    *eq = '\0';
    const char* key = tok;
    const char* val = eq + 1;

    if (strcmp(key, "pm25") == 0) {
      if (has_pm25 || cmd.fail || !parse_float_range(val, 0.0f, 1000.0f, &cmd.pm25)) return false;
      has_pm25 = true;
    } else if (strcmp(key, "tvoc") == 0) {
      if (cmd.tvoc_valid || cmd.fail || !parse_float_range(val, 0.0f, 500.0f, &cmd.tvoc)) return false;
      cmd.tvoc_valid = true;
    } else if (strcmp(key, "nox") == 0) {
      if (cmd.nox_valid || cmd.fail || !parse_float_range(val, 0.0f, 500.0f, &cmd.nox)) return false;
      cmd.nox_valid = true;
    } else if (strcmp(key, "co2") == 0) {
      if (cmd.co2_valid || cmd.fail || !parse_float_range(val, 0.0f, 40000.0f, &cmd.co2)) return false;
      cmd.co2_valid = true;
    } else if (strcmp(key, "night") == 0) {
      if (cmd.has_night) return false;
      if (strcmp(val, "0") == 0) {
        cmd.night = false;
      } else if (strcmp(val, "1") == 0) {
        cmd.night = true;
      } else {
        return false;
      }
      cmd.has_night = true;
    } else {
      return false;
    }
  }

  if (cmd.fail && (has_pm25 || cmd.tvoc_valid || cmd.nox_valid || cmd.co2_valid)) return false;
  if (!cmd.fail && !has_pm25) return false;

  *out = cmd;
  return true;
}

}  // namespace sim
#endif
