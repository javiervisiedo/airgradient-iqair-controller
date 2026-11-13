---
name: airgradient-api
description: >-
  Use this skill whenever code or questions involve the AirGradient ONE
  monitor data: the local LAN HTTP API (/measures/current), the 4-step
  discovery cascade, serialno verification, boot-counter stale/frozen
  detection, JSON field names and units (pm02Compensated, pm02, rco2,
  tvocIndex, noxIndex, boot), mocking /measures/current, or optional host-side
  inspection of the cloud REST API.
---

# AirGradient API

**Architecture decision (2026-11-09, SRS v2.2 §2.2 & §3.1):** The firmware uses
the **local LAN HTTP API only** (`GET http://<ip>/measures/current`, plain HTTP
on port 80). No cloud API, token (`AG_TOKEN`), `locationId`, or TLS is used in
firmware.

Implementation: `include/ag_client.h`, `src/ag_client.cpp`.

## Local LAN API (Firmware Primary Path)

- Reference: [local_api.md](references/local_api.md).
- **Polling cadence:** every `poll_interval_sec` (default `60` s, matching the
  monitor's 60.0 s reporting cadence) in `net_task` (Core 0) using
  `esp_http_client`.
- **4-step discovery cascade (`AgClient::poll`):**
  1. `ag_target_host` (static IP or hostname override, `3000 ms` timeout) if set.
  2. Cached IPv4 from RAM/NVS (`iaqcfg` / `ag_ip`, `2500 ms` timeout).
  3. Targeted mDNS host `airgradient_<ag_serial>.local` (`2000 ms` timeout via
     `mdns_query_a`, sharing the ESP-IDF mDNS instance with Matter).
  4. mDNS service scan (`_airgradient._tcp`, then `_http._tcp`, up to 4
     candidates), caching the discovered IPv4 in NVS only when it changes.
- **Strict `serialno` verification (SRS §2.2):** when `ag_serial` is configured
  (default `"9888e0eefcec"`), every response MUST contain a matching `serialno`
  field. Missing or mismatched `serialno` returns `ParseResult::SERIAL_MISMATCH`
  and counts as a failed poll.
- **Fields parsed (`ag::parse_measures`, filtered `JsonArena`):**
  - `pm02Compensated`, fallback `pm02` (`0..1000` µg/m³) → `pm25` (**required**;
    missing/`null`/`NaN` returns `NO_PM25` and counts as a failed poll).
  - `tvocIndex` (`0..500`), `noxIndex` (`0..500`), `rco2` (`0..40000` ppm),
    `atmp` (`-40..85` °C), `rhum` (`0..100` %), `boot` (measurement-cycle counter).
- **Stale and frozen detection (`ag::StaleDetector`, SRS §3.1 rule 5):**
  - The AirGradient `boot` counter advances each measurement cycle; air values
    themselves may legitimately stay constant.
  - 1–3 consecutive unchanged `boot` values: still valid; on the 3rd,
    `ag_data_stale = true` and a warning is logged.
  - 4th and subsequent unchanged `boot` values (`frozen`): treated as **failed
    polls** so the network watchdog caps and unreachable alarm engage.
  - Next changed `boot` clears `ag_data_stale` and recovers normally.
- **Read-only consumer:** never call `PUT /config` or CO₂ calibration endpoints
  on the AirGradient from controller firmware.

## Mock Server (Local & Cloud Shapes, No Token Needed)

```bash
python3 .agents/skills/airgradient-api/scripts/mock_airgradient.py --port 8080 --scenario ok
curl 'localhost:8080/_mock/set?scenario=frozen'   # hold boot counter steady to test TC-09
curl 'localhost:8080/_mock/set?scenario=null_pm'  # test missing PM2.5 watchdog
```

The mock increments `boot` on every valid local response and supports `ok`,
`frozen`, `wrong_serial`, `null_pm`, `missing`, `stale`, `unauthorized`,
`no_data`, `server_error`, `slow`, `malformed`, and `html` scenarios, plus
`--serial`, `--nox`, `--pm25-series`, and `--sequence`. Useful for host/LAN
bench testing or Wokwi via Private Gateway (`ag_target_host = "host.wokwi.internal:8080"`).

## Host-Side Exploration (Local + Legacy Cloud)

```bash
# Query local monitor directly on the LAN (no token needed):
python3 .agents/skills/airgradient-api/scripts/ag_query.py local 9888e0eefcec

# Optional cloud API inspection (only if debugging monitor cloud uploads; needs AG_TOKEN):
export AG_TOKEN=...
python3 .agents/skills/airgradient-api/scripts/ag_query.py locations
python3 .agents/skills/airgradient-api/scripts/ag_query.py current <locationId>
```

Legacy cloud API docs (not used by firmware): [api_reference.md](references/api_reference.md)
and [airgradient_swagger.json](references/airgradient_swagger.json). Never log
tokens if using `ag_query.py` with `AG_TOKEN`.
