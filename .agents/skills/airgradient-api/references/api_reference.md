# AirGradient Cloud API v1: Reference

Source: Swagger 2.0 at `https://api.airgradient.com/public/docs/api/v1/swagger.json`
(snapshot in [airgradient_swagger.json](airgradient_swagger.json)).

- Base URL: `https://api.airgradient.com/public/api/v1`
- Auth: `token` query parameter (place access token) on every request except `ping`.
- TLS: Let's Encrypt chain (`YR2` → `Root YR` → cross-signed by `ISRG Root X1`, as of 2026-11).

## Endpoints

| Method | Path | Returns | Notes |
| :--- | :--- | :--- | :--- |
| GET | `/ping` | 200 | Health check, no auth |
| GET | `/place` | `Place` | `{id, name, timezoneId, countryId, publicName, publicUrl}` |
| GET | `/locations/measures/current` | `Measure[]` | All locations of the place. Use it to **discover locationId** |
| GET | `/locations/{locationId}/measures/current` | `GoMeasure` | **Firmware primary endpoint** |
| GET | `/locations/{locationId}/measures/raw?from&to` | `GoMeasure[]` | Latest 200, or a window of at most 2 days. Basic ISO dates `20240328T120500Z` |
| GET | `/locations/{locationId}/measures/past?from&to` | `Measure[]` | 5-min or 60-min buckets |
| GET | `/locations/{ids}/measures/buckets/{bucketSize}` | `Measure[]` | Averages across locations |
| GET | `/alarms/triggered` | `TriggeredAlarm[]` | Alarms configured in the dashboard |
| GET/PUT | `/locations/{locationId}/sensor/config/leds/mode` | `LedMode` | `{"mode": "co2"\|"pm"\|"off"\|"default"}` |
| GET/PUT | `/sensors/{serialno}/config/leds/mode` | `LedMode` | Same, by serial number |
| POST | `/locations/{locationId}/sensor/co2/calibration` | – | Triggers CO2 calibration. **Don't call it from firmware** |
| POST | `/sensors/{serialno}/co2/calibration` | – | Same, by serial number |
| GET | `/world/locations/measures/current` | `Measure[]` | Public world data |
| GET | `/world/locations/{locationId}/measures/current` | `Measure` | Public location |

Response codes: `200` OK, `400` bad request, `401` not authorized (bad or missing token), `404` no data available.

## `Measure` fields (all optional)

| Field | Type | Unit / meaning |
| :--- | :--- | :--- |
| `locationId` | number | Location id |
| `locationName` | string | Dashboard name |
| `locationType` | string | `indoor` / `outdoor` |
| `serialno` | string | Sensor serial (hex) |
| `model` | string | e.g. `I-9PSL` (ONE), `O-1PST` (Open Air) |
| `pm01`, `pm02`, `pm10` | float | Raw PM1 / PM2.5 / PM10 µg/m³ (1 dp) |
| `pm01_corrected`, `pm02_corrected`, `pm10_corrected` | float | Corrected PM µg/m³. **Preferred** |
| `pm003Count` | int | Particles >0.3 µm per dL |
| `atmp` / `atmp_corrected` | float | Temperature °C |
| `rhum` / `rhum_corrected` | int | Relative humidity % |
| `rco2` / `rco2_corrected` | int | CO2 ppm |
| `tvoc` | float | TVOC ppb (only some sensors) |
| `tvocIndex` | int | Sensirion VOC index (1–500, 100 = baseline) |
| `noxIndex` | int | Sensirion NOx index (1–500, 1 = baseline) |
| `no2`, `o3` (+ electrode fields, `afeTemp`) | float | Only model `O-M-1PPSTON-CE` |
| `batteryVoltage`, `panelVoltage` | float | Volts (outdoor/solar models) |
| `wifi` | int | dBm |
| `datapoints` | int | Only for aggregates |
| `timestamp` | string | ISO 8601 UTC, e.g. `2022-03-28T12:07:40Z` |
| `firmwareVersion` | string | e.g. `9.2.6` |
| `latitude`, `longitude` | float | If available |
| `anomalies` | string[] | e.g. `["condensation"]` (raw measures) |

`GoMeasure` adds `pres` (hPa), `lat`, `lng`, `alt`, `speed`, `hdop`, `motion`.

## Example

```bash
curl -s "https://api.airgradient.com/public/api/v1/locations/12345/measures/current?token=$AG_TOKEN"
```

```json
{"locationId":12345,"locationName":"Living room","pm02":8.2,"pm02_corrected":6.9,
 "rco2":612,"atmp":26.1,"rhum":58,"tvocIndex":97,"noxIndex":1,
 "timestamp":"2026-11-06T04:01:40Z","serialno":"ecda3b1eaaaf","model":"I-9PSL"}
```
