# AirGradient Local Server API (LAN)

Available on AirGradient ONE / Open Air monitors with firmware **3.0.10 or later**.
Source: `airgradienthq/arduino` repo, `docs/local-server.md`.

- Discovery: mDNS hostname `airgradient_<serialno>.local` (serial is lowercase hex).
- Plain HTTP on port 80. No auth, no TLS.
- On ESP32: `MDNS.begin(...)`, then `MDNS.queryHost("airgradient_<serial>")` to get the IP.
  Cache the IP and re-resolve when a request fails.

## GET `/measures/current`

```json
{"wifi":-46,"serialno":"ecda3b1eaaaf","rco2":447,"pm01":3,"pm02":7,"pm10":8,
 "pm003Count":442,"atmp":25.87,"atmpCompensated":24.47,"rhum":43,"rhumCompensated":49,
 "tvocIndex":100,"tvocRaw":33051,"noxIndex":1,"noxRaw":16307,"boot":6,"bootCount":6,
 "ledMode":"pm","firmware":"3.1.3","model":"I-9PSL","monitorDisplayCompensatedValues":true}
```

Field names differ from the cloud API:

| Cloud | Local |
| :--- | :--- |
| `pm02_corrected` | `pm02Compensated` (firmware 3.1.4 or later) |
| `atmp_corrected` | `atmpCompensated` |
| `rhum_corrected` | `rhumCompensated` |
| `rco2_corrected` | (none, use `rco2`) |
| `timestamp` | (none: treat the time of a successful fetch as the timestamp) |

Extra local fields: `pm01Standard`, `pm02Standard`, `pm10Standard`,
`pm005Count`, `pm01Count`, `pm02Count`, `pm50Count`, `pm10Count`, `tvocRaw`,
`noxRaw`, `boot` ("counts every measurement cycle; low boot counts indicate restarts",
per AirGradient's `docs/local-server.md`; `bootCount` is the same value, deprecated). The
firmware uses `boot` as the freshness counter for stale/frozen detection (SRS §3.1 rule 5).

## GET `/config` / PUT `/config`

Read or modify the monitor configuration (`ledBarMode`, `abcDays`,
`co2CalibrationRequested`...). Only works if `configurationControl` is
`local` or `both`. **The controller firmware must not PUT config**, because it's a
read-only consumer.

## Firmware Usage (Decided 2026-11-09, SRS v2.2)

- As of `SRS v2.2`, the controller firmware uses the **local API exclusively**
  (`src/ag_client.cpp` via `esp_http_client`).
- Discovery follows the 4-step cascade (`ag_target_host` -> cached NVS IP ->
  `airgradient_<ag_serial>.local` -> `_airgradient._tcp` / `_http._tcp` scan).
- When `ag_serial` is configured, `serialno` must match exactly or the poll is
  rejected (`SERIAL_MISMATCH`).
- Needs the AtomS3 and monitor on the same L2 subnet for mDNS; across subnets/VLANs
  or when mDNS is blocked, set `ag_target_host` to the monitor's IP address.
