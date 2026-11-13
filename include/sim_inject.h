// sim_inject.h: pure parser for Wokwi simulation serial injection commands.
// Host-testable (UNIT_TEST) and active in WOKWI_SIM builds.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sim {

struct InjectCmd {
  bool fail = false;
  float pm25 = 0.0f;
  bool tvoc_valid = false;
  float tvoc = 0.0f;
  bool nox_valid = false;
  float nox = 0.0f;
  bool co2_valid = false;
  float co2 = 0.0f;
  bool has_night = false;
  bool night = false;
};

// Parses:
//   inject pm25=<0..1000> [tvoc=<0..500>] [nox=<0..500>] [co2=<0..40000>] [night=0|1]
//   inject fail [night=0|1]
bool parse_inject_line(const char* line, InjectCmd* out);

}  // namespace sim
