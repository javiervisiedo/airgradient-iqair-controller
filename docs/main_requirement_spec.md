# Firmware System Requirement Specification (SRS): M5Stack AtomS3 Lite Automated IAQ & Purifier Controller (v2.0)

## Document Metadata & Companion Reference

| Attribute | Specification Details |
| :--- | :--- |
| **Document ID** | `SRS-FW-ATOMS3-IAQ-002` |
| **Version** | 2.2 (Matter Multi-Admin + Discrete NEC IR + 4-Tier Clock Sync; watchdog aligned with algorithm spec v3 §3.1; REST API pairing) |
| **Revision History** | **2.2 (2026-11-09):** (1) §5.3 REST API authentication: per-device bearer token issued through a button-initiated pairing window. (2) §3.3 button: the 2–4 s IR re-send gesture is removed; releasing after 3–10 s opens pairing mode; §4 adds the pairing LED patterns. (3) §3.1 rule 5: frozen AirGradient readings escalate to failed polls. (4) Firmware-review amendments: per-client lockout and pairing Host/Origin guard (§5.3), request robustness rules (§5.3 item 8), strict serial check (§2.2), button ignored until released after boot (§3.3), recovery re-send after any outage (§3.1), `night_led_enabled` defined (§4). **2.1 (2026-11-09):** (1) §3.1 network watchdog now follows the companion spec v3 §3.1 speed caps (Night ≤ Speed 1 after 3 failed polls, Day ≤ Speed 3 after 15); the 5-poll alarm no longer switches the purifier Off. (2) §3.2 / §5.2 NEC address word default corrected to `0xFF00` and command/inverse byte columns corrected. (3) §3.3 Manual mode is exempt from the watchdog caps. **2.0:** initial Matter release. |
| **Companion Algorithm Spec** | `automated_iaq_multigas_fan_controller_spec_v3.md` (v3.0 Dual-Path PM2.5 + TVOC/NOx Hysteresis Algorithm) |
| **Target MCU Hardware** | M5Stack AtomS3 Lite (`ESP32-S3FN8`, Dual-Core Xtensa LX7 @ 240 MHz, 512 KB SRAM, 8 MB SPI Flash) [1][3][7] |
| **Upstream Sensor Node** | AirGradient ONE Indoor Air Quality Monitor (`I-9PSL`, Serial `9888e0eefcec`, Location ID `212774`) [2][11] |
| **Downstream Actuator** | IQAir GC MultiGas Air Purifier (Controlled via 38 kHz NEC Discrete Infrared Codes, Speeds 0–6) [5][17][24] |
| **Smart Home Ecosystem** | **Matter over Wi-Fi (Multi-Admin)** for simultaneous **Apple Home** + **Google Home** (and future Home Assistant) + Local HTTP REST API [26][29][30] |

---

## 1. System Overview & Architectural Topology

### 1.1 Purpose & Scope
This document defines the finalized, production-ready hardware, network, Matter Multi-Admin, clock-synchronization, state-machine, memory, and API requirements for the embedded firmware running on the **M5Stack AtomS3 Lite** microcontroller [1][7]. The firmware bridges an **AirGradient ONE** indoor air quality monitor (`I-9PSL`) [2] and an **IQAir GC MultiGas** air purifier [5][17], executing the companion **v3.0 Dual-Path Control Algorithm** every 60 seconds and emitting discrete 38 kHz NEC infrared (IR) commands to regulate fan speed [24][25].

This v2.0 specification incorporates all finalized product decisions so that an autonomous AI coding swarm can implement, verify, and build the firmware without any remaining open questions:
1. **Simultaneous Apple Home & Google Home Integration via Matter over Wi-Fi (Multi-Admin):** Uses the official Espressif `ESP-Matter` / `Arduino-ESP32 Matter` SDK (`esp32` Arduino Core $\ge 3.1.0$) so a single device pairs natively with **Apple Home** (HomeKit) and **Google Home** simultaneously without cloud dependencies, and can be shared with **Home Assistant** in the future via Matter Multi-Admin [26][29][30].
2. **Discrete NEC Infrared Command Table:** Uses the exact discrete NEC IR codes for the IQAir remote (`0xFF00` Off, `0xE817` Speed 1/On, `0xFC03` Speed 2, `0xFD02` Speed 3, `0xFA05` Speed 4, `0xFB04` Speed 5, `0xE916` Speed 6), eliminating open-loop step-counting drift [24][25].
3. **4-Tier Resilient Local & Internet Clock Synchronization:** Synchronizes the ESP32-S3 Real-Time Clock (RTC) via (1) **DHCP Option 42** local router NTP discovery, (2) **Local Gateway/Router NTP (`WiFi.gatewayIP()`)** + public NTP pool, (3) **Matter Time Synchronization Cluster (`0x0038`)** from Apple HomePod/Apple TV or Google Nest hubs, and (4) **HTTP `Date` header fallback** from local web servers [27][28].
4. **Nighttime LED Silence with Active Fault Override:** Keeps normal status LEDs completely **Off (`0%` brightness)** during Night Mode (`23:00–07:59`) while overriding nighttime dimming (`40%` brightness) if a critical sensor-unreachable alarm occurs [1][7].

### 1.2 Hardware Pinout & Physical Interfaces
The M5Stack AtomS3 Lite integrates an Espressif `ESP32-S3FN8` SoC in a 24.0 × 24.0 × 9.5 mm enclosure weighing 5.3 g, powered via 5V USB Type-C with an onboard SY8089 5V-to-3.3V DC-DC buck converter [3][7]. The firmware shall bind to the following physical GPIO assignments [1][7]:

| Peripheral / Interface | GPIO Pin | Hardware Component | Firmware Driver & Electrical Configuration |
| :--- | :--- | :--- | :--- |
| **Infrared (IR) Transmitter** | `GPIO4` | Internal 940 nm IR LED | ESP32-S3 **RMT (Remote Control Transceiver)** peripheral, Channel 0, 38 kHz carrier, 33% duty cycle, active-high. |
| **Status RGB LEDs** | `GPIO35` | WS2812C-2020 Addressable RGB LEDs (beneath button) | ESP32-S3 **RMT** Channel 1 or `FastLED` / `Adafruit_NeoPixel` (800 kHz GRB protocol), non-blocking state machine. |
| **User Push Button** | `GPIO41` | Tactile Button (Beneath RGB Lens) | Internal pull-up enabled (`INPUT_PULLUP`, active-low), 50 ms debounce, short-press (mode cycle), 3–10 s hold (REST API pairing mode), & 10 s long-press (Matter decommissioning / factory reset) [26][29]. |
| **Grove HY2.0-4P Port** | `GPIO1`, `GPIO2` | 4-Pin Expansion Connector (5V, GND, G1, G2) | Reserved for optional external high-power IR blaster module (`M5Stack IR Unit`) if line-of-sight distance exceeds internal LED range. |
| **Bottom Expansion Header** | `G5, G6, G7, G8, G38, G39` | 6× Logic-Level 3.3V GPIOs | Unused in v2.0; configured as high-impedance inputs. |
| **USB Type-C Port** | `GPIO19`, `GPIO20` | Native USB-CDC / JTAG | Serial logging (`115200 baud`), Matter QR/pairing code printout, and firmware flashing [26][29]. |

---

## 2. Core Architectural Decisions (System Architect Trade-Off Analysis)

### 2.1 Smart Home Architecture: Matter over Wi-Fi (Multi-Admin) + Local REST API
* **Architectural Decision:** **Implement a Native Matter over Wi-Fi Node (Composite Air Purifier + Air Quality Sensor Endpoints) paired with a Lightweight Local HTTP REST JSON API for Advanced Engineering Parameters.**
* **Why Matter over Wi-Fi Is Ideal for Apple Home + Google Home + Future Home Assistant:**
  1. **Native Multi-Admin Support:** Matter (formerly Project CHIP) allows a single ESP32-S3 node to join multiple smart home fabrics simultaneously (**Multi-Admin**) over your local IPv4/IPv6 Wi-Fi network without any cloud dependency [26][29][30]. You commission the AtomS3 Lite first in **Apple Home** using the iPhone Home app (via QR code or 11-digit manual pairing code over BLE), then tap *"Turn On Pairing Mode"* in Apple Home to add it simultaneously to **Google Home** (and later **Home Assistant**) [26][29].
  2. **Supported Native UI Controls in Apple Home & Google Home:**
     * **Endpoint 1 — Matter Air Purifier / Fan Device (`0x002D` / `0x002B`):** Exposes the **On/Off Cluster (`0x0006`)** and **Fan Control Cluster (`0x0202`)** [29][30]. Both Apple Home and Google Home render an interactive Purifier/Fan tile with:
       * **Mode Selector (`FanMode` / `FanModeSequence`):** Switches seamlessly between **`Auto` (`operating_mode = "auto"`)** and **`Manual` (`operating_mode = "manual"`)**.
       * **6-Step Speed Slider (`SpeedSetting` / `SpeedCurrent`, `SpeedMax = 6`, or `PercentSetting` in $16.67\%$ steps):** Dragging the slider in Apple Home or saying *"Hey Siri / Hey Google, set the living room air purifier to speed 4"* immediately activates `MANUAL` override at Speed 4 (`0xFA05`). Switching the tile back to `Auto` resumes the v3.0 Dual-Path algorithm.
     * **Endpoint 2 — Matter Air Quality Sensor (`0x002C`) + Temperature (`0x0302`) + Humidity (`0x0307`):** Forwards the polled AirGradient ONE telemetry directly into Apple Home and Google Home rooms (`AirQuality` enum: Good / Fair / Moderate / Poor / VeryPoor / ExtremelyPoor, `PM2.5ConcentrationMeasurement` in $\mu\text{g/m}^3$, `TotalVolatileOrganicCompoundsConcentrationMeasurement`, `NitrogenDioxideConcentrationMeasurement`, `CarbonDioxideConcentrationMeasurement` in ppm, Temperature in $^\circ\text{C}$, and Relative Humidity in $\%$) [2][9][29].
     * **Endpoint 3 — Night Quiet Force Switch (`OnOffPlugInUnit` `0x010A` named `"Purifier Night Quiet Lock"`):** Allows Apple Home / Google Home scenes (e.g., *"Goodnight"* or *"Movie Time"*) to force the purifier into Night Mode thresholds (`NIGHT_FORCE`) with a single tap or voice command [26][29].
  3. **Why Keep the Lightweight HTTP REST API (`/api/v1/config`) Alongside Matter:**
     Standard Matter clusters in Apple Home and Google Home UI do not display custom text-input boxes for numeric engineering parameters like `room_volume_m3` (`150.0`) or `ag_target_host` (`192.168.1.x`). By running a tiny local HTTP REST server (`GET/PATCH /api/v1/config` and `GET /api/v1/status`) on port 80 alongside Matter, you (or any future Home Assistant REST integration) can inspect or tweak `room_volume_m3`, hysteresis deadbands, and target IP addresses via `curl` or a browser at `http://atoms3-iaq.local/api/v1/config`.

### 2.2 Configuration Provisioning & AirGradient Discovery Waterfall
* **Provisioning Flow:**
  1. **Wi-Fi Commissioning via Matter BLE:** When uncommissioned, the AtomS3 Lite advertises over Bluetooth Low Energy (BLE). Scanning the Matter QR code (or entering the 11-digit manual code printed on USB-CDC serial at `115200 baud`) in Apple Home or Google Home securely provisions your home Wi-Fi SSID and password into ESP32 NVS—**no hardcoded Wi-Fi credentials or separate Captive Portal needed** [26][29][30]! Once commissioned onto Wi-Fi, the BLE stack is automatically de-initialized by `ESP-Matter` to free `~65 KB` of internal SRAM [29].
  2. **Persistent Application Config (`/config.json` in NVS / LittleFS):** Stores `room_volume_m3` (default `150.0`), `ag_serial` (default `"9888e0eefcec"`), `ag_target_host` (default `""`), diurnal schedule hours, and the discrete NEC IR hex table.
* **4-Step AirGradient ONE Discovery Cascade:**
  Every 60 seconds, the AtomS3 Lite polls the AirGradient ONE local HTTP server (`GET /measures/current`) [9][15][31] using the following resolution cascade:
  1. **Step 1 (Static IP / Hostname Override):** If `ag_target_host` is non-empty in NVS config, query `http://<ag_target_host>/measures/current` directly [15][23].
  2. **Step 2 (Cached NVS IPv4 Fast-Path):** Query the last successful IPv4 address cached in RAM/NVS (`ag_cached_ip`). If HTTP `200 OK` is returned within `2,500 ms` and JSON `serialno` matches, retain the cached IP with zero mDNS overhead.
  3. **Step 3 (Targeted Serial mDNS Lookup):** Query `MDNS.queryHost("airgradient_" + ag_serial)` (defaulting to `airgradient_9888e0eefcec.local`) with a `2,000 ms` timeout [11][15][31].
  4. **Step 4 (mDNS Service Discovery Scan):** Query `MDNS.queryService("airgradient", "tcp")` (and fallback `"http", "tcp"`), verify `/measures/current` JSON signature, and cache the discovered IPv4 address in NVS.
  * **Serial check (all steps):** when `ag_serial` is set, a reply is accepted only if its `serialno` equals `ag_serial`; a reply **without** `serialno` counts as a mismatch (failed poll), so no other HTTP device at that address can drive the purifier.

### 2.3 Multi-Tier Resilient Clock Synchronization Architecture (Local + Internet)
Because the v3.0 algorithm relies on local time (`08:00–22:59` Day Mode vs. `23:00–07:59` Night Mode), the firmware must synchronize its internal Real-Time Clock (RTC) reliably—even if external internet access is blocked by a firewall or ISP outage.
* **Architectural Analysis of Local Time Sources:**
  * **Local Router / Gateway via DHCP Option 42 & Gateway SNTP:** Standard home routers (OpenWrt, UniFi, AsusWRT, pfSense, Fritz!Box) can provide their own local IP as an NTP server via **DHCP Option 42** or answer SNTP queries directly on their gateway IP (`WiFi.gatewayIP()`) [27][28]. In `ESP-IDF` / `Arduino-ESP32`, calling `esp_sntp_servermode_dhcp(true)` **before** `WiFi.begin()` / DHCP lease acquisition automatically populates the router's local NTP server into SNTP Slot 0 [27][28].
  * **Matter Time Synchronization Cluster (`0x0038`):** Both **Apple Home Hubs** (HomePod, Apple TV) and **Google Nest Hubs** maintain high-precision UTC time and local timezone offsets on the Matter fabric. The firmware can read UTC time from the Matter fabric if UDP port 123 (SNTP) is blocked.
  * **AirGradient ONE & Router HTTP Headers (Peer Inspection):** Note that the AirGradient ONE (`I-9PSL`) local `/measures/current` JSON payload returns instantaneous sensor readings and uptime/boot counters rather than a wall-clock timestamp [9][15]. However, if the local router's web admin page (or a local NAS/hub on `WiFi.gatewayIP()`) responds to an HTTP `HEAD /` request, its standard HTTP `Date: <day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT` header provides a valid 1-second-resolution UTC timestamp over TCP port 80!
* **4-Tier Clock Sync Waterfall Implemented in Firmware:**
  1. **Tier 1 — DHCP Option 42 Local Router NTP (Pre-DHCP Hook):** Call `esp_sntp_servermode_dhcp(1)` prior to Wi-Fi connection so lwIP automatically accepts any local NTP server advertised by the home router's DHCP server [27][28].
  2. **Tier 2 — Multi-Server SNTP Pool (Local Gateway + Internet Pool):** Upon `IP_EVENT_STA_GOT_IP`, configure lwIP SNTP with three servers in priority order [27][28]:
     * `Server 0`: Dynamically acquired DHCP Option 42 server (or `WiFi.gatewayIP().toString()` if Option 42 was empty, querying the local router directly on UDP port 123).
     * `Server 1`: `"pool.ntp.org"` (Primary internet NTP pool) [28].
     * `Server 2`: `"time.google.com"` / `"time.apple.com"` (Secondary internet NTP fallback).
     * Configure POSIX timezone string via `setenv("TZ", tz_posix_str, 1); tzset();` (defaulting to `"SGT-8"` for `UTC+8`, configurable via `/api/v1/config`) with smooth clock discipline (`SNTP_SYNC_MODE_SMOOTH` via `adjtime`) every 3 hours [27][28].
  3. **Tier 3 — Matter Time Sync / HTTP `Date` Header Fallback:** If `sntp_get_sync_status()` has not completed within `30 seconds` of Wi-Fi connection (e.g., UDP port 123 is blocked), the firmware checks the Matter Time Sync attribute; if unavailable, it issues an HTTP `HEAD /` request to `http://<gateway_ip>/` and `http://google.com/`, parses the RFC 7231 `Date:` header (`strptime`), and seeds the ESP32-S3 RTC via `settimeofday()`.
  4. **Tier 4 — Hardware RTC Retention & Safe Day-Mode Fallback:** The ESP32-S3 internal RTC timer (`RTC_SLOW_CLK`) preserves wall-clock time across warm reboots and Wi-Fi dropouts [27]. If the device cold-boots during a total internet/router outage and no time source has succeeded yet (`time(nullptr) < 1700000000`), the controller sets `clock_sync_status = "unsynced_fallback_day"` and defaults to **Day Mode (`is_night = false`)** unless the user activates the `"Purifier Night Quiet Lock"` switch in Apple Home / Google Home.

### 2.4 Dynamic Room Volume Scaling (`room_volume_m3`) vs. Hardwired Thresholds
* **Architectural Decision:** **Hybrid Parametric Architecture — Hardwired Health Floor Thresholds + Dynamic Aerodynamic Scaling for Upper Plume Thresholds & Step-Down Dwell Timer.**
* **Memory & CPU Feasibility Analysis:**
  * The `ESP32-S3FN8` features a 240 MHz dual-core 32-bit Xtensa LX7 CPU with a hardware single-precision Floating-Point Unit (FPU) and 512 KB of internal SRAM [1][3][4].
  * Storing `room_volume_m3` as a 32-bit `float` (4 bytes) and recalculating 6 threshold floats plus 1 integer dwell timer whenever `room_volume_m3` is modified takes **< 5 microseconds** of CPU time and **< 64 bytes** of RAM.
* **Exact Parametric Formula Implemented in Firmware:**
  Let `V` in `[30.0, 500.0]` m³ be the configurable `room_volume_m3` parameter (default `V_ref = 150.0` m³).
  1. **Dimensionless Aerodynamic Volume Ratio (`ρ(V)`):**
     `ρ(V) = clamp(sqrt(150.0 / V), 0.65, 1.35)`
     *(At `V = 150.0 m³`, `ρ = 1.000`, reproducing our exact v3.0 150 m³ table. At `V = 250.0 m³`, `ρ = 0.775`, scaling the 150 m³ upper thresholds `[20, 35, 55, 85]` down to `[15.5, 27.1, 42.6, 65.9]` μg/m³, matching our 250 m³ curve).*
  2. **Dynamic Step-Down Dwell Timer (`T_hold` in minutes):**
     `T_hold(V) = clamp(round(4.0 * (V / 150.0)), 2, 8)`
     *(Yields `4 minutes` at `150 m³` and `7 minutes` at `250 m³`).*
  3. **Daytime PM2.5 Threshold Vectors (`U_day(V)`, `L_day(V)`):**
     * **Speed 0 ↔ 1 (Fixed WHO Annual):** `U_0 = 5.0 μg/m³`, `L_0 = 3.0 μg/m³`
     * **Speed 1 ↔ 2 (Fixed EPA/WHO):** `U_1 = 12.0 μg/m³`, `L_1 = 8.5 μg/m³`
     * **Speed 2 ↔ 3 (Scaled):** `U_2(V) = 20.0 * ρ(V)`, `L_2(V) = 14.5 * ρ(V)`
     * **Speed 3 ↔ 4 (Scaled):** `U_3(V) = 35.0 * ρ(V)`, `L_3(V) = 26.0 * ρ(V)`
     * **Speed 4 ↔ 5 (Scaled):** `U_4(V) = 55.0 * ρ(V)`, `L_4(V) = 42.0 * ρ(V)`
     * **Speed 5 ↔ 6 (Scaled):** `U_5(V) = 85.0 * ρ(V)`, `L_5(V) = 65.0 * ρ(V)`
  4. **Fixed Nighttime PM2.5 & Multi-Gas Matrices:** Nighttime PM2.5 thresholds (`U_night = [10, 15, 25, 35, 55, 100]`, `L_night = [6.5, 11, 19, 28, 42, 75]`) and the 2-Tier Capped Multi-Gas Floor (`TVOC > 200/300`, `NOx >= 4.0/6.0` Day; `TVOC > 300`, `NOx >= 5.0` Night) remain fixed as defined in `IAQ_MultiGas_Fan_Controller_Spec_v3.md`.
  5. **Real-Time ACH Telemetry Calculation:**
     Using the IQAir GC MultiGas airflow lookup table `Q = [0, 80, 150, 240, 310, 380, 510]` m³/h, the firmware computes live turnover:
     `ACH_current = Q[current_speed] / V`

---

## 3. Functional Requirements & State Machines

### 3.1 AirGradient Polling & Fault-Tolerant Network Watchdog
The microcontroller polls `GET http://<airgradient_ip>/measures/current` every **60 seconds** (`poll_interval_sec = 60`). The purifier speed during an outage follows the **network watchdog caps of the companion algorithm spec (v3 §3.1)**; the separate 5-poll **unreachable alarm** is a notification only and sends no IR command of its own.

A **failed poll** is any poll without a valid PM2.5 reading (timeout, HTTP error, unparseable JSON, serial mismatch, or `pm02` missing / `NaN`), **or a frozen reading** (rule 5). Thresholds are counted in **consecutive failed polls** (minutes at the default 60 s interval). "Night" is the resolved diurnal mode used by the algorithm: the `23:00–07:59` schedule, or `NIGHT_FORCE`; with an unsynced clock the §2.3 tier 4 Day fallback applies.

| Consecutive Failed Polls | Purifier Fan Speed Action (AUTO / NIGHT_FORCE) | LED Status (`GPIO35`) & Smart Home State |
| :--- | :--- | :--- |
| **0 (Healthy)** | Normal automatic evaluation (`Speed 0–6`). | **LED:** Normal Operational Display (or Off at night).<br>**Matter / API:** `ag_status = "online"`, `alarm_sensor_unreachable = false`. |
| **1 to 2** | **Hold:** keep the last commanded speed. | **LED:** **Warning — Slow Pulse Amber/Yellow (1 Hz)**.<br>**Matter / API:** `ag_status = "degraded"`, `ag_fail_count = 1..2`. |
| **≥ 3, Night** | **Night Cap:** immediately reduce to `min(current_speed, 1)` (one IR command if the speed changes) so a Wi-Fi dropout never leaves the purifier running audibly while occupants sleep. | Amber warning while `ag_fail_count ≤ 4`, then the alarm below. |
| **3 to 14, Day** | **Hold:** keep the last commanded speed. | Amber warning while `ag_fail_count ≤ 4`, then the alarm below. |
| **≥ 5 (Unreachable Alarm)** | **No IR action of its own.** The hold / caps in this table continue to govern the speed. | **LED:** **Alarm — Fast Strobe Red (3 Hz, 80% Day / 40% Night)**.<br>**Matter / API:** `ag_status = "unreachable_alarm"`, `alarm_sensor_unreachable = true`, Matter Air Quality sensor marked `Fault / Unavailable`. |
| **≥ 15, Day** | **Day Cap:** reduce to `min(current_speed, 3)` (one IR command if the speed changes) so the unit cannot remain stuck on Turbo indefinitely. | Red alarm (as above). |
| **Recovery (1 Valid Poll)** | **Automatic Resume:** clear the fault state, append the reading to the 5-min buffers, and continue the algorithm **from the (possibly capped) commanded speed** (fast attack upwards, normal dwell decay downwards). The resulting discrete code is re-transmitted even if unchanged. | **LED:** Restore Normal Operational Display.<br>**Matter / API:** `ag_status = "online"`, `alarm_sensor_unreachable = false`, `ag_fail_count = 0`. |

**Watchdog rules:**
1. **Caps never raise the speed.** Without valid data the firmware only holds or lowers the speed. Once capped, the speed stays at or below the cap until a valid reading arrives, even if the outage crosses from night into day.
2. **Day/night is re-evaluated on every failed poll.** If night begins during an outage that has already lasted ≥ 3 polls, the Night Cap applies at the next failed poll.
3. **`MANUAL` mode is exempt.** In `MANUAL` the purifier keeps the user's speed for the whole outage; the amber warning and the red alarm are still raised. If the user switches back to `AUTO` / `NIGHT_FORCE` during an outage, the applicable cap is applied immediately.
4. **Nothing commanded yet → nothing sent.** After a (re)boot with no valid reading and no command issued yet, the purifier state is unknown; the watchdog sends no IR command (there is nothing to hold or cap). The alarm is still raised at 5 failed polls.
5. **Stale and frozen data.** The AirGradient `boot` counter advances with every measurement cycle, so a reading whose counter has not changed since the previous poll is **unchanged** (the air values themselves may legitimately stay constant and are not compared).
   * **1–3 consecutive unchanged readings:** still valid polls. On the 3rd the firmware sets `ag_data_stale = true` and logs a warning.
   * **4th and later consecutive unchanged readings (frozen):** each counts as a **failed poll** for this table, so the warning LED, the night/day caps and the alarm follow from the failed-poll count. `ag_data_stale` stays `true`.
   * The first reading with a changed counter is valid again (normal recovery) and clears `ag_data_stale`.

### 3.2 Discrete NEC Infrared (IR) Actuator Subsystem (`GPIO4`)
Because your IQAir remote features dedicated discrete number buttons for each fan speed, the IR subsystem operates with **stateless, deterministic command transmission** (no open-loop arrow step-counting required) [24][25]:

1. **Hardware RMT Configuration:** Configure ESP32-S3 RMT TX Channel 0 on `GPIO4` (and optionally mirrored on `GPIO1` Grove TX pin) for the **NEC 38 kHz infrared protocol** (9.0 ms leading AGC burst, 4.5 ms space, 560 µs pulse bursts, 38 kHz carrier at 33% duty cycle) [1][25].
2. **IQAir Discrete NEC Command Lookup Table:**
   In the NEC protocol (`IRremoteESP8266` / `esp_driver_rmt`), every frame carries a 16-bit **address word** followed by a 16-bit **command word**. Each word is written as `0x<inverse byte><byte>`: the **low byte** (the address or command) is transmitted first, LSB first, followed by its bitwise inverse in the **high byte**. The IQAir address word `ir_nec_address` defaults to **`0xFF00`** (NEC address `0x00`, inverse `0xFF`; configurable in `/config.json`). The full 32-bit frame is `(command_word << 16) | address_word`, e.g. Speed 1 = `0xE817FF00` (the value IRremote-style decoders report). Each target speed $S \in \{0..6\}$ maps to its dedicated command word:

| Target State / Speed | Volumetric Airflow ($Q$) | IQAir Discrete Command Word (`16-bit Hex`) | Command Byte / Inverse Byte | Transmission Policy on Speed Change |
| :--- | :--- | :--- | :--- | :--- |
| **Speed 0 (Power Off)** | `0 m³/h` | **`0xFF00`** | `Cmd: 0x00` / `Inv: 0xFF` | Transmit frame **twice** (`100 ms` gap) when transitioning from `Speed 1..6` → `0`. |
| **Speed 1 (Whisper / On)** | `80 m³/h` (`25 dB`) | **`0xE817`** | `Cmd: 0x17` / `Inv: 0xE8` | If transitioning from `Speed 0 (Off)` → `Speed 1`, send `0xE817` twice (`100 ms` gap). |
| **Speed 2 (Low)** | `150 m³/h` (`36 dB`) | **`0xFC03`** | `Cmd: 0x03` / `Inv: 0xFC` | If waking from `Speed 0 (Off)` → `Speed S (S >= 2)`: send `0xE817` (On) first, wait `600 ms` wake delay, then send `0xFC03` twice (`100 ms` gap). |
| **Speed 3 (Medium-Low)** | `240 m³/h` (`44 dB`) | **`0xFD02`** | `Cmd: 0x02` / `Inv: 0xFD` | Direct transition if already On; or `0xE817` (On) + `600 ms` + `0xFD02` if waking from Off. |
| **Speed 4 (Medium-High)** | `310 m³/h` (`50 dB`) | **`0xFA05`** | `Cmd: 0x05` / `Inv: 0xFA` | Direct transition if already On; or `0xE817` (On) + `600 ms` + `0xFA05` if waking from Off. |
| **Speed 5 (High)** | `380 m³/h` (`54 dB`) | **`0xFB04`** | `Cmd: 0x04` / `Inv: 0xFB` | Direct transition if already On; or `0xE817` (On) + `600 ms` + `0xFB04` if waking from Off. |
| **Speed 6 (Turbo Max)** | `510 m³/h` (`59 dB`) | **`0xE916`** | `Cmd: 0x16` / `Inv: 0xE9` | Direct transition if already On; or `0xE817` (On) + `600 ms` + `0xE916` if waking from Off. |

* **Wake-From-Off Handshake Rule (`wake_require_speed1_prefix`, default `true`):** On many IQAir units, pressing `Speed 1 / On` (`0xE817`) wakes the unit from Standby, whereas pressing `Speed 2–6` while in Standby is ignored until the unit is powered on. Therefore, whenever `current_speed == 0` and the new target speed is $S \in \{2..6\}$, the IR driver automatically sends `0xE817` (`Speed 1 / On`), waits `600 ms` (non-blocking FreeRTOS timer), and then transmits the discrete code for Speed $S$.
* **Periodic State Enforcement (`ir_refresh_mins`, default `15 mins`):** Because discrete codes are idempotent (sending `0xFD02` when the purifier is already on Speed 3 simply keeps it on Speed 3 without toggling), the firmware can optionally re-broadcast the current discrete speed code every `15 minutes` during active operation (`current_speed >= 1`) to self-correct if a human walked in front of the IR beam during an earlier transition!

### 3.3 Operating Modes & Physical Button (`GPIO41`) Behavior
1. **`AUTO` Mode (Default):** Governed by the v3.0 Dual-Path (`PM2.5` + `TVOC`/`NOx`) Hysteresis State Machine and Day/Night schedule.
2. **`MANUAL` Override Mode:** Triggered when the user adjusts the fan speed slider in **Apple Home** or **Google Home**, calls `PATCH /api/v1/config`, or short-presses `GPIO41`. Holds the selected speed ($0..6$) until the user switches the Apple/Google Home tile back to `Auto` (or after `manual_timeout_mins`, default `0` = indefinite). Background 60-second AirGradient polling continues uninterrupted so the 5-minute moving average buffers stay warm. The §3.1 network watchdog caps do **not** apply in `MANUAL` (the user's explicit speed takes precedence); sensor warnings and the unreachable alarm are still raised.
3. **`NIGHT_FORCE` Mode:** Triggered via the `"Purifier Night Quiet Lock"` Matter switch in Apple Home / Google Home scenes. Forces the algorithm to use Night Mode thresholds (`Speed 0–1` normal, `Speed 1` gas cap) at any hour.
4. **Physical Button (`GPIO41`) Multi-Function Handler [1][26][29]:**
   * **Short Press (`< 1 s`):** Cycles `AUTO` → `MANUAL Speed 1` → `MANUAL Speed 2` → `MANUAL Speed 3` → `MANUAL Off (0)` → `AUTO` (and immediately updates Apple Home & Google Home UI state).
   * **Hold 3–10 s, then release (REST API pairing):** While the button is held past 3 s the LED shows **solid Cyan** ("release now to pair"). Releasing between 3 s and 10 s opens the **60 s pairing window** of §5.3, shown by a **Cyan heartbeat** (double blink every second). Holding on past 10 s switches to the factory-reset gesture below instead. Releases between 1 s and 3 s are ignored.
   * **Long Press (`≥ 10 s`):** Standard Matter Factory Reset / Decommissioning gesture—blinks Magenta rapidly, erases Matter fabric credentials (`Matter.decommission()`) **and the REST API token**, and re-opens BLE commissioning mode [26][29].
   * *(The former 2–4 s "re-send current IR command" gesture is removed in v2.2. The current code is still re-sent every `ir_refresh_mins`, and `POST /api/v1/command {"action":"test_ir"}` sends a code on demand.)*
   * **Held at boot:** a button that is already pressed at boot (or stuck) is ignored until it has been released once, so it can never trigger a factory reset or pairing on its own.

---

## 4. RGB LED Status & Diagnostic Language (`GPIO35`)

Confirmed per your preference, normal operational LEDs (P5–P8) turn **completely Off (`0%` brightness)** during Night Mode (`23:00–07:59` or when `NIGHT_FORCE` is active), while critical fault and warning states (P1–P3) remain illuminated so an offline sensor or network drop is immediately visible [1][7]. **`night_led_enabled`** (config, default `false`): when `true`, P5–P8 stay on at night at **25% of their day brightness** (rounded down) instead of Off; P0–P4 are unaffected:

| Priority | System State | LED Color & RGB Hex | Animation Pattern | Brightness (Day / Night) | Trigger Condition |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **P0 (User gesture)** | **Factory Reset Warning** | **Magenta** (`#FF00FF`) | **Rapid Blink (10 Hz)** | **100%** | Button held ≥ 10 s, until the decommission completes. |
| **P0 (User gesture)** | **Release to Pair** | **Cyan** (`#00E5FF`) | **Solid** | **80% Day / 40% Night** *(Overrides night silence)* | Button held 3–10 s (releasing now opens pairing, §3.3). |
| **P0 (User gesture)** | **REST API Pairing Mode** | **Cyan** (`#00E5FF`) | **Heartbeat** (100 ms ON, 100 ms OFF, 100 ms ON, 700 ms OFF) | **80% Day / 40% Night** *(Overrides night silence)* | 60 s pairing window open (§5.3); ends on a successful pairing or timeout. |
| **P1 (Highest status)** | **Sensor Unreachable Alarm (≥ 5 mins)** | **Red** (`#FF0000`) | **Fast Strobe (3 Hz)** (165 ms ON / 165 ms OFF) | **80% Day / 40% Night** *(Overrides night silence)* | `ag_fail_count >= 5` (AirGradient unreachable ≥ 5 polls; purifier speed governed by the §3.1 watchdog caps). |
| **P2** | **Wi-Fi Disconnected / Matter Uncommissioned** | **Blue** (`#0055FF`) / **Magenta** (`#FF00FF`) | **Fast Blink (2 Hz)** Blue (Wi-Fi) / Pulse Magenta (BLE Pairing) | **60% Day / 20% Night** | `WiFi.status() != WL_CONNECTED` or waiting for Matter BLE commissioning. |
| **P3** | **Sensor Poll Warning (1–4 mins)** | **Amber / Yellow** (`#FFAA00`) | **Slow Pulse (1 Hz)** (500 ms ON / 500 ms OFF) | **50% Day / 15% Night** | `1 <= ag_fail_count <= 4` (holding the last commanded speed, or night-capped per §3.1). |
| **P4** | **IR Transmission Event (Transient)** | **White** (`#FFFFFF`) | **150 ms Double Flash** | **70% Day / 10% Night** | Momentary flash whenever `GPIO4` transmits a discrete NEC IR command. |
| **P5** | **Manual Override Active** | **Purple / Violet** (`#9900FF`) | **Slow Breathing (0.25 Hz sine wave)** | **40% Day / 0% (Off) Night** | `operating_mode == MANUAL`. |
| **P6** | **Auto Mode — High Scrubbing (Speeds 4–6)** | **Orange-Red** (`#FF3300`) | **Solid** | **40% Day / 0% (Off) Night** | `operating_mode == AUTO` and `current_speed >= 4`. |
| **P7** | **Auto Mode — Moderate Scrubbing (Speeds 2–3)** | **Cyan / Teal** (`#00E5FF`) | **Solid** | **30% Day / 0% (Off) Night** | `operating_mode == AUTO` and `current_speed in (2, 3)`. |
| **P8 (Lowest)** | **Auto Mode — Clean / Idle (Speeds 0–1)** | **Emerald Green** (`#00C853`) | **Solid (Day) / Off (Night)** | **20% Day / 0% (Off) Night** | `operating_mode == AUTO` and `current_speed in (0, 1)`. |

---

## 5. Smart Home & REST API Integration Specification

### 5.1 Matter Endpoints & Clusters (Apple Home, Google Home, & Future Home Assistant)
Built on the official `Arduino-ESP32 Matter` / `ESP-Matter` SDK (`#include <Matter.h>`) [26][29], the firmware exposes a bridged/composite Matter Node with **3 Endpoints**:

| Matter Endpoint | Device Type & ID | Clusters Implemented | Apple Home & Google Home User Experience |
| :--- | :--- | :--- | :--- |
| **Endpoint 1** | **Air Purifier / Fan** (`0x002D` / `0x002B`) | `Identify (0x0003)`, `OnOff (0x0006)`, `FanControl (0x0202)` (`MultiSpeed`, `SpeedMax = 6`, `Auto` mode supported) | Appears as **"Living Room Air Purifier"**. Supports On/Off, **Auto vs. Manual** mode toggle, and **0–100% (6-step) speed slider** via Apple Home app, Google Home app, Siri, and Google Assistant [26][29][30]. |
| **Endpoint 2** | **Air Quality Sensor** (`0x002C`) + **Temp/RH** (`0x0302`, `0x0307`) | `AirQuality (0x005B)`, `PM2.5Concentration (0x042A)`, `TVOCConcentration (0x042E)`, `NO2Concentration (0x0413)`, `CO2Concentration (0x040D)`, `Temperature (0x0402)`, `RelativeHumidity (0x0405)`, `BooleanState (0x0045)` (Sensor Fault) | Displays live **Air Quality Status**, **PM2.5 ($\mu\text{g/m}^3$)**, **CO₂ (ppm)**, **Temperature ($^\circ\text{C}$)**, and **Humidity (%)** directly inside the Apple Home Climate summary and Google Home Environment tab. Triggers a native fault alert if `alarm_sensor_unreachable == true` ($\ge 5\text{ mins}$ offline) [29]. |
| **Endpoint 3** | **Night Quiet Lock Switch** (`0x010A` On/Off Plug-in Unit) | `OnOff (0x0006)` | Appears as **"Purifier Night Quiet Lock"**. Turning it **ON** activates `NIGHT_FORCE` mode; turning it **OFF** returns to the automatic `23:00–07:59` NTP schedule. |

### 5.2 Local HTTP REST API (`/api/v1/*`) for Engineering Configuration & Diagnostics
Alongside Matter, the firmware runs a lightweight HTTP server on port 80 (discoverable at `http://atoms3-iaq.local`) exposing all runtime configuration and telemetry in JSON format. Read-only endpoints (`GET`) are open on the LAN; endpoints that change the device (`PATCH /api/v1/config`, `POST /api/v1/command`) require the bearer token of §5.3:

#### Read/Write Configuration Schema (`GET` / `PATCH http://atoms3-iaq.local/api/v1/config`)
```json
{
  "operating_mode": "auto",
  "manual_speed": 0,
  "manual_timeout_mins": 0,
  "room_volume_m3": 150.0,
  "ag_serial": "9888e0eefcec",
  "ag_target_host": "",
  "poll_interval_sec": 60,
  "enable_multigas": true,
  "tz_posix": "SGT-8",
  "day_start_hour": 8,
  "night_start_hour": 23,
  "night_led_enabled": false,
  "ir_nec_address": "0xFF00",
  "ir_codes": {
    "off": "0xFF00",
    "speed_1": "0xE817",
    "speed_2": "0xFC03",
    "speed_3": "0xFD02",
    "speed_4": "0xFA05",
    "speed_5": "0xFB04",
    "speed_6": "0xE916"
  },
  "wake_require_speed1_prefix": true,
  "ir_refresh_mins": 15
}
```

#### Read-Only Live Status & Telemetry Schema (`GET http://atoms3-iaq.local/api/v1/status`)
```json
{
  "current_speed": 3,
  "operating_mode": "auto",
  "pm_target_speed": 3,
  "gas_target_speed": 2,
  "dominant_trigger": "pm25",
  "current_airflow_m3h": 240,
  "effective_ach": 1.60,
  "room_volume_m3": 150.0,
  "rho_volume_factor": 1.000,
  "dwell_stepdown_mins": 0,
  "min_stepdown_hold_mins": 4,
  "is_night_mode": false,
  "clock_sync_status": "synced_dhcp_ntp",
  "local_time_iso": "2026-11-09T14:30:00+08:00",
  "ag_status": "online",
  "alarm_sensor_unreachable": false,
  "ag_resolved_ip": "192.168.1.84",
  "ag_source": "cached_ip",
  "ag_fail_count": 0,
  "ag_data_stale": false,
  "api_paired": true,
  "api_pairing_active": false,
  "telemetry": {
    "pm25_raw": 22.4,
    "pm25_sma5": 21.8,
    "tvoc_raw": 215.0,
    "tvoc_sma5": 210.4,
    "nox_raw": 2.0,
    "nox_sma5": 2.0,
    "co2_ppm": 640,
    "temperature_c": 25.4,
    "humidity_pct": 63.8
  },
  "matter_commissioned": true,
  "matter_fabrics_count": 2,
  "speed_changes_today": 14,
  "wifi_rssi_dbm": -58,
  "free_heap_bytes": 245760,
  "min_free_heap_bytes": 201328,
  "uptime_s": 86400,
  "ir_frames_sent": 42
}
```

### 5.3 REST API Authentication & Button Pairing
Modelled on local-API pairing used by smart-home vendors (e.g. Philips Hue, Nanoleaf): physical access to the device is the proof of ownership, and there is no shared default password (ETSI EN 303 645 provision 5.1; UK PSTI; EU RED cybersecurity requirements).

1. **Token.** The device holds at most one API token: 128 bits from the hardware RNG, encoded as 32 lowercase hex characters. It is stored in its own NVS namespace, never in `/config.json`, and is **never returned by any GET endpoint, logged, or printed on serial**. A new device, or one after a factory reset, has **no token**: every authenticated request is rejected until a client pairs.
2. **Pairing window.** Holding the button 3–10 s and releasing (§3.3) generates a new candidate token and opens a **60 s** window (Cyan heartbeat LED, `api_pairing_active = true`).
3. **`POST /api/v1/pair`** (no body, no auth):
   * Inside the window: `200 OK` with `{"token":"<32 hex>","token_type":"Bearer"}`. The new token **replaces the previous one immediately** (all other clients must re-pair), the window closes (one client per window), and the main loop persists it to NVS.
   * Outside the window: `403 Forbidden` with a hint to use the button.
   * **Origin guard (checked first; a rejected request does not use up the window):** the request must be addressed to the device itself. The `Host` header must be `atoms3-iaq.local` or the device's IPv4 address (optional `:80`), and an `Origin` header, if present (browsers), must be `http://` plus one of those. Otherwise `403 Forbidden`. This stops a web page open on a LAN computer from taking the token during the window (DNS rebinding / cross-site requests).
4. **Authenticated endpoints:** `PATCH /api/v1/config` and `POST /api/v1/command` require the header `Authorization: Bearer <token>` (scheme case-insensitive). The token is compared in constant time.
   * Missing or wrong token: `401 Unauthorized` with `WWW-Authenticate: Bearer realm="atoms3-iaq"`.
   * **Lockout (per client address):** 5 wrong tokens from one client IP address within 60 s lock **that address** out for 60 s; other clients are unaffected (correct requests in between don't reset the count). During its lockout every authenticated request from that address (even with the right token) gets `429 Too Many Requests` with `Retry-After`. Up to 8 addresses are tracked; when the table is full the least recently active unlocked entry is replaced, so active lockouts are kept. A successful pairing clears all lockouts. Trailing whitespace after the token is ignored.
5. **Open endpoints:** `GET /`, `GET /api/v1/status`, `GET /api/v1/config` (neither response contains secrets).
6. **Revocation:** pairing again revokes the old token; the ≥ 10 s factory reset erases it. The new token is persisted to NVS by the main loop, retried until the write succeeds, so a reboot can never bring back a revoked token.
7. **Transport:** plain HTTP on the LAN (as with Hue's local API). The token protects against casual or accidental changes by other LAN devices, not against an attacker who can sniff LAN traffic.
8. **Robustness:** request bodies are limited to 1.5 KB (`413 Payload Too Large` above that). Any request rejected before its body is read (401, 403, 413, 429) is answered with `Connection: close` and the connection is closed, so a slow or oversized upload can't hold up the server. Pairing windows and lockouts expire by elapsed time and are cleared when they expire, so they can never reappear (e.g. on the ~49.7-day `millis()` wrap).

---

## 6. Memory & Resource Budget (`ESP32-S3FN8` with Matter Stack)

The M5Stack AtomS3 Lite (`ESP32-S3FN8`) provides **8 MB (8,388,608 bytes) of SPI Flash** and **512 KB (524,288 bytes) of internal SRAM** (no external PSRAM) [1][3][7]. Because the Matter stack (`libchip` + `mbedTLS` + IPv6/mDNS) requires a compiled application binary of `~1.9 MB` and `~160 KB` of RAM [26][29], the 8 MB flash on the AtomS3 Lite is a major advantage (allowing full Dual-OTA even with Matter!).

### 6.1 Flash Partition Table (8 MB SPI Flash — Matter Dual-OTA Layout)
Define `partitions_8MB_matter.csv` in the PlatformIO project root [29]:

| Partition Name | Type / SubType | Offset | Size | Purpose & Budget |
| :--- | :--- | :--- | :--- | :--- |
| `nvs` | `data / nvs` | `0x9000` | `64 KB` | Stores Matter fabric certificates, CASE session keys, Wi-Fi credentials, and runtime config [29]. |
| `otadata` | `data / ota` | `0x19000` | `8 KB` | OTA active partition selector. |
| `app0` | `app / ota_0` | `0x20000` | `3,584 KB` (`3.50 MB`) | Primary firmware slot (exceeds Espressif's 3.0 MB Matter minimum recommendation [26][29]; estimated binary size `~1.95 MB`). |
| `app1` | `app / ota_1` | `0x3A0000` | `3,584 KB` (`3.50 MB`) | Secondary OTA firmware slot for safe wireless firmware updates. |
| `fctry` | `data / nvs` | `0x720000` | `24 KB` | Matter factory discriminator, passcode, and DAC attestation data. |
| `littlefs` | `data / spiffs` | `0x726000` | `872 KB` | Stores `/config.json` backup and diagnostic logs. |

### 6.2 Internal SRAM Budget (512 KB Total SRAM, No PSRAM)

| Memory Region / Subsystem | Allocated RAM | Engineering Optimization Constraint |
| :--- | :--- | :--- |
| **ESP-IDF / FreeRTOS Kernel & Wi-Fi + IPv4/IPv6 lwIP** | `~135 KB` | Standard dual-stack IPv4/IPv6 required by Matter over Wi-Fi [26][29]. |
| **Matter Stack (`ESP-Matter` / `CHIP` + `mbedTLS`)** | `~145 KB` | **Critical Rule:** Enable `CONFIG_BT_NIMBLE_ENABLED` only during initial commissioning; release BLE controller memory (`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`) once commissioned to reclaim `~65 KB` of SRAM [29]! |
| **Local HTTP Client (`ag_client`) + REST Server** | `~20 KB` | Use ESP-IDF native `esp_http_server.h` and `esp_http_client.h` instead of heavy third-party async libraries. |
| **Static JSON Buffers (`ArduinoJson v7`)** | `~4 KB` | Fixed stack/static allocation; zero dynamic `String` heap concatenation. |
| **v3.0 Algorithm Circular Buffers & State Struct** | **`184 Bytes`** | Three 5-element `float` arrays (`60 B`) + threshold vectors (`96 B`) + state counters (`28 B`). |
| **RMT IR (`GPIO4`) & WS2812 (`GPIO35`) Buffers** | `~2 KB` | Hardware RMT symbol blocks (`64 × 4B` per channel). |
| **Remaining Free Internal SRAM Heap** | **`~205 KB` (`40%` Free)** | Watchdog logs a warning if `esp_get_free_heap_size()` drops below `120 KB`. |

---

## 7. Modular Firmware Directory Structure (For AI Coding Swarm)

```text
atoms3-iaq-matter-controller/
├── platformio.ini                # board = m5stack-atoms3, esp32 arduino core >= 3.1.0, partitions_8MB_matter.csv
├── partitions_8MB_matter.csv     # Dual 3.5MB OTA slots + 64KB NVS + Matter factory partition
├── data/
│   └── config.json               # Default configuration (150.0 m3, serial 9888e0eefcec, NEC hex codes)
├── include/
│   ├── sys_config.h              # Config structs, GPIO macros (G4 IR, G35 WS2812, G41 Button)
│   ├── clock_sync.h              # 4-Tier Clock Sync (DHCP Opt 42 -> Gateway/Pool SNTP -> Matter/HTTP Date)
│   ├── ag_client.h               # 4-Step AirGradient discovery waterfall & /measures/current parser
│   ├── iaq_algorithm.h           # v3.0 Dual-Path (PM2.5 + TVOC/NOx) Hysteresis & rho(V) volume scaling
│   ├── ir_nec_purifier.h         # ESP32-S3 RMT 38kHz NEC driver (0xFF00, 0xE817..0xE916) & wake prefix
│   ├── status_led.h              # Non-blocking WS2812 P1-P8 priority state machine & night silence
│   ├── api_auth.h                # REST bearer token, button pairing window, lockout (§5.3)
│   └── matter_ha_bridge.h        # Matter Endpoints 1-3 (Purifier, AirQuality, NightLock) + HTTP /api/v1/*
└── src/
    ├── main.cpp                  # Setup, hardware WDT, 60s control loop task, button IRQ handler
    ├── sys_config.cpp            # NVS + LittleFS load/save/validation
    ├── clock_sync.cpp            # esp_sntp_servermode_dhcp(1), gateway SNTP, HTTP Date fallback
    ├── ag_client.cpp             # Static -> Cached IP -> mDNS hostname -> mDNS service scan
    ├── iaq_algorithm.cpp         # Pure C++ implementation of companion v3.0 algorithm spec
    ├── ir_nec_purifier.cpp       # Discrete NEC pulse encoder & 600ms wake-from-off state handler
    ├── status_led.cpp            # 20 Hz FreeRTOS timer driving WS2812 LEDs on GPIO35
    └── matter_ha_bridge.cpp      # Matter callbacks (Apple/Google Home slider -> MANUAL mode) & REST server
```

---

## 8. Verification & Acceptance Test Matrix (For AI Coding Swarm)

| Test ID | Test Scenario | Input Stimulus | Expected Firmware Behavior & Pass Criteria |
| :--- | :--- | :--- | :--- |
| **`TC-01`** | **Matter Multi-Admin Commissioning (Apple + Google Home)** | Boot uncommissioned device; scan QR code in Apple Home, then share pairing code to Google Home [26][29]. | Commissions over BLE, joins Wi-Fi, frees BLE RAM, and exposes Purifier speed slider + Air Quality sensors simultaneously in both Apple Home and Google Home. |
| **`TC-02`** | **4-Tier Clock Sync & Offline Router Fallback** | Block external WAN internet on router; boot AtomS3 Lite on LAN. | Acquires NTP from local router via DHCP Option 42 / Gateway IP (`WiFi.gatewayIP()`) or HTTP `Date` header; synchronizes local time (`UTC+8`) within 30 seconds [27][28]. |
| **`TC-03`** | **Discrete NEC IR Wake & Speed Transition** | Controller transitions from `Speed 0 (Off)` to `Speed 4`. | RMT on `GPIO4` transmits `0xE817` (`Speed 1 / On`), waits `600 ms`, then transmits `0xFA05` (`Speed 4`) twice (`100 ms` gap), and flashes `GPIO35` White. |
| **`TC-04`** | **Dynamic Room Volume Scaling (`room_volume_m3`)** | Send `PATCH /api/v1/config {"room_volume_m3": 250.0}`. | `ρ(250)` updates to `0.775`, `T_hold` updates from `4` to `7` mins, and Day Speed 3–6 upper thresholds scale to `[15.5, 27.1, 42.6, 65.9]` μg/m³ in `< 1 ms`. |
| **`TC-05`** | **Sensor Dropout: Hold (Day) & Night Cap** | (a) Day, purifier at Speed 3: disconnect AirGradient ONE for 3 consecutive 60 s ticks. (b) Night, purifier at Speed 4: disconnect for 3 ticks. | (a) `ag_fail_count = 3`, purifier speed remains **unchanged**, `GPIO35` pulses **Amber (1 Hz)** (`50%` Day / `15%` Night), `alarm_sensor_unreachable = false`. (b) Ticks 1–2 hold Speed 4; on tick 3 the MCU transmits Speed 1 (`0xE817`) once (Night Cap). |
| **`TC-06`** | **Unreachable Alarm & Day Cap** | Day, purifier at Speed 6: keep AirGradient ONE unreachable for 15 consecutive 60 s ticks. | On tick 5: `alarm_sensor_unreachable = true`, Matter sensor Fault, `GPIO35` strobes **Red (3 Hz)** (`80%` Day / `40%` Night), and **no IR command** is sent (Speed 6 held). Ticks 6–14: no IR command. On tick 15: MCU transmits Speed 3 (`0xFD02`) once (Day Cap). No further commands while unreachable. |
| **`TC-07`** | **Automatic Recovery After Alarm** | Day, purifier at Speed 3: AirGradient ONE unreachable on ticks 1–6, reconnected on tick 7 reporting `pm02 = 28.0`. | Clears the alarm, updates the `SMA-5` buffers (pre-outage samples aged out), computes target Speed 3 from the commanded speed, re-transmits `0xFD02`, and restores the normal LED state (`0%` Off at night, solid color during day). After a Night Cap the algorithm resumes from Speed 1. |
| **`TC-08`** | **REST API Pairing & Authentication** | (a) Fresh device: `PATCH /api/v1/config` without a token. (b) Hold the button 4 s and release, then `POST /api/v1/pair`; repeat the POST. (c) `PATCH` with the issued token, then with the previous token. (d) Send 5 wrong tokens, then the right one. (e) Wait 60 s after opening the window without pairing. | (a) `401`. (b) LED solid Cyan while held past 3 s, Cyan heartbeat after release; first POST `200` with a 32-hex token, second POST `403`. (c) New token `200`, old token `401`. (d) 5 × `401`, then `429` with `Retry-After` until 60 s pass. (e) Heartbeat stops, `POST /api/v1/pair` returns `403`. The token never appears in GET responses or logs. |
| **`TC-09`** | **Frozen AirGradient Readings** | Night, purifier at Speed 4: the monitor returns HTTP `200` with valid JSON but its `boot` counter stops advancing. | Unchanged readings 1–3 are used normally; on the 3rd, `ag_data_stale = true`. Unchanged readings 4–6 count as failed polls (`ag_fail_count` 1–3, amber LED); on the 6th the MCU transmits Speed 1 (Night Cap). When the counter advances again, the reading is valid, `ag_fail_count = 0`, `ag_data_stale = false`, and the algorithm resumes. |

## Sources
[1] [M5Stack ATOM S3 Lite - ESPHome Devices](https://devices.esphome.io/devices/m5stack-atoms3-lite/)

[2] [AirGradient ONE (I-9PSL): deep dive · Goat Farm](https://www.goatfarm.team/p/airgradient-one)

[3] [Tiny ESP32-S3 IoT controller M5Stack ATOMS3 Lite - Electronics-Lab](https://www.electronics-lab.com/news/tiny-esp32-s3-iot-controller-m5stack-atoms3-lite/)

[4] [M5Atom S3 Lite, the "little one" of the M5Stack Atom family - Luis Llamas](https://www.luisllamas.es/en/m5stack-atom-s3-lite/)

[5] [IQAir GC MultiGas Review - Objective Air Quality Testing - Modern Castle](https://moderncastle.com/air-purifiers/iqair-gc-multigas-review/)

[7] [AtomS3-Lite - m5-docs](https://docs.m5stack.com/en/core/AtomS3-Lite)

[9] [AirGradient One - Test with Home Assistant - The smarthome journey](https://thesmarthomejourney.com/2026/03/05/airgradient-one-home-assistant/)

[11] [Where to set up MQTT? - AirGradient Forum](https://forum.airgradient.com/t/where-to-set-up-mqtt/2526)

[15] [Local API Questions/Versioning? : airgradient-csv-writer - Software integrations](https://forum.airgradient.com/t/local-api-questions-versioning-airgradient-csv-writer/3613)

[17] [GC Series | IQAir Asia Pacific](https://www.iqair.com/as/products/gc-series)

[23] [How to Configure MQTT Locally (Without AirGradient Dashboard)](https://www.airgradient.com/documentation/kb/configure-mqtt-via-local-api)

[24] [Infrared Remote Control and Saving Last Speed Setting - Infineon Technologies](https://www.infineon.com/assets/row/public/documents/24/42/infineon-an2023-03-infrared-remote-control-and-saving-last-speed-setting-applicationnotes-en.pdf)

[25] [Infrared Remote Control Protocols: Part 2 - irq5.io](https://irq5.io/2012/08/11/infrared-remote-control-protocols-part-2/)

[26] [ESP32 Matter: Control On/Off Light (Arduino) – Works with Alexa, Apple Home, Google, etc… - Random Nerd Tutorials](https://randomnerdtutorials.com/esp32-matter-on-off-light/)

[27] [esp-idf/examples/protocols/sntp/README.md at master - GitHub](https://github.com/espressif/esp-idf/blob/master/examples/protocols/sntp/README.md)

[28] [ESP-NETIF Programmers Manual - ESP32 - — ESP-IDF Programming Guide v6.1 documentation - Espressif Systems](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_netif_programming.html)

[29] [Matter - - — Arduino ESP32 latest documentation - Espressif Systems](https://docs.espressif.com/projects/arduino-esp32/en/latest/matter/matter.html)

[30] [ESP32 Matter: 69 Open-Source Smart-Home Devices, No Cloud Required - Studiopieters](https://www.studiopieters.nl/esp32-matter/)

[31] [AirGradient API - Open Air Foundation](https://openair.foundation/undp-toolkit/operating/airgradient-api)
