---
trigger: always_on
---

# Secrets & Hardware Safety

## Secrets
- WiFi SSID and password, the AirGradient place token, and location IDs go only
  in a gitignored secrets header (`include/secrets.h` by convention; the hook
  guards this name) or in shell env vars (`AG_TOKEN`, `AG_LOCATION_ID`). A
  tracked `*.example` template with placeholder values is fine.
- Home Assistant long-lived tokens (`HA_TOKEN`), MQTT credentials, and the device's own
  API password are secrets too. Same storage rules.
- The device settings API must treat secrets as write-only. It never returns them in
  responses, discovery payloads, or logs.
- The Wokwi CI token (`WOKWI_CLI_TOKEN`) is a secret too. It goes in the env or the gitignored `.env` only.
- Don't echo a token into chat, logs, commit messages, or serial output.
  When printing URLs, mask the `token=` query value.
- Don't put a token in `platformio.ini` `build_flags`, because they show up in
  verbose build logs.

## Simulation
- Wokwi's Public IoT Gateway is monitored by Wokwi. Simulation builds must never
  contain the real AirGradient token or real WiFi credentials. Use `Wokwi-GUEST`
  and a mock or injected data source.
- Wokwi minutes are a limited quota. Keep `--timeout` tight and don't loop simulations unattended.

## TLS
- `WiFiClientSecure::setInsecure()` is allowed only behind
  `#ifdef AG_ALLOW_INSECURE_TLS` for local debugging. Never enable it by default.

## Physical hardware
- Before running `pio run -t upload`, `esptool ... erase_flash`, or anything else
  that opens the serial port, say which port and environment you're using.
- Only one process can hold the USB serial port at a time. Stop
  `pio device monitor` before uploading.
- IR commands change a real appliance. During development, don't loop IR sends
  faster than one command every 2 s.
