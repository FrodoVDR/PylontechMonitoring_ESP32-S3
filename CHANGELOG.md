# Change Log
All notable changes to this project will be documented in this file.

## ToDo

- other Homeautomation (Discoverer)

## 2026-06-28
- 1.2.3
- NVS config persistence fix: per-field settings (MQTT/Send) for STAT/BAT/INFO/PWR are no longer lost on firmware update; without the per-field MQTT flags STAT stopped publishing to MQTT entirely
- `saveJsonChunked` now writes new chunks first and only removes stale chunks after a fully successful write; on a write failure existing data is left intact instead of being wiped
- Empty in-memory field maps no longer overwrite stored NVS config (`saveStatFields`/`saveBatFields`/`saveInfoFields`/`savePwrFields` skip saving when empty)
- Crash fix: sporadic PANIC in the web loop (`nrt:web_loop`) under low internal heap; web request handling is now deferred when free internal heap is critically low (mirrors the existing API cache heap-floor guard) instead of crashing in the network stack; added rate-limited low-heap warning for diagnostics
- Note: STAT field selections already lost from NVS must be re-entered and saved once after updating

## 2026-06-27
- Health (`/api/health`) integrity: status snapshot is only committed when every detected module is present and its cell data is verified plausible; transient PWR undercounts (e.g. 6/8) no longer drop modules in the UI
- Health evaluation now built in a PSRAM-backed working copy to relieve the heap
- Health warnings: status fields are matched against known alarm states only, so normal operating states no longer trigger unfounded warnings; warn/error history is extended on verified snapshots only
- Health thresholds: default warning raised to 38 mV and default error to 40 mV (`cellDiffWarn`/`cellDiffError`)

## 2026-06-24
- 1.2.2
- MQTT stack: SOC added to `pylontech/stack` payload
- Stack SOC calculation corrected to average Coulomb across all modules (rounded, based on BatteryCount)
- MQTT stack: computed `StackPower` added (`StackCurrSum * StackVoltAvg`) with unit W
- MQTT stack: `StackPowerIn` and `StackPowerOut` added (split from `StackPower` for charge/discharge)
- BAT parser: SOC values are normalized to numeric text (e.g. `98%` -> `98`)
- PWR parser: SOC/Coulomb values stored without `%` suffix in all fields (Stack + Hub mode)
- MQTT payload typing: numeric values are now published as JSON numbers (without quotes) for PWR/BAT/STAT/INFO
- MQTT PWR: `Power`/`Battery` index field is forced numeric when parseable
- MQTT BAT cells: added numeric `Number` field with module index (1..N)
- Scheduler hardening: runtime battery mode lock + auto-recovery on unexpected mode flips (prevents HUB-stop stall)
- BAT pipeline stabilization: synchronized single-writer/snapshot-reader buffer handling (mutex) to avoid race-related PANICs
- Optional debug visibility for stack publish values (SOC + BatteryCount)
- Syslog settings persistence: UDP target/enabled state now survives reboot and logs persisted/loaded state for diagnostics
- UART frame handling hardened for STACK commands: complete `@...$$` frames are accepted even when the trailing `pylon>` prompt is delayed
- PWR stability: transient parser undercounts no longer reduce detected module count; retry handling added for incomplete PWR reads
- BAT scheduler now uses stable detected module count so transient PWR drops do not remove modules from BAT polling
- BAT parser now treats fewer than 15 cells per module as an incomplete query and rejects the frame instead of publishing partial data
- BAT retry handling improved: incomplete BAT frames trigger retries with UART console recovery before retrying
- PWR parser tolerates missing/noisy optional temperature/SOC fields without dropping otherwise valid module rows
- INFO page stabilization for navigation flow (INFO -> Health -> INFO)
- INFO cache hardening with last-good handling and empty-raw protection
- Scheduler queue hardening (mutex, deduplication, queue limit)
- MQTT INFO publishing fixes for text-like fields
- Documentation update (German + English)

## 2026-05-14 
- 1.2.1
- config values for helth
- Alpha for Pylontech Hub

## 2026-05-14 
- 1.2.0
- OTA from Website 

## 2026-04-30 
- 1.1.0
- Display (ST7735)
- helth page 

## 2026-04-15 
- 1.0.0
- Stable Website

## 2026-04-03 
- Stable communikation to battery
- use 2 core 

## 2026-02-21
- include command and Parser for bat x (Celldata) 
- include command and parser for stat x (Statistic data) 

## 2026-01-31
- change code from [@hidaba](https://github.com/hidaba) (D1 mini) to ESP32-S
- build Modules for alls parts of the Software
- init Hotspot + Fallback Hotspot 
- Wlan Autoscan
- passwords in Secret 
- flexible Parser one for all Firmware
- klickable Data for MOTT (Discoverer, Publisher)