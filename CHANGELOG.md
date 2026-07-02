# Change Log
All notable changes to this project will be documented in this file.

## ToDo

- other Homeautomation (Discoverer)
## 2026-07-01
- 1.2.13
- UI fix (CPU Monitor page "Heap frei" always showed ~8.0 MB): `web/cpu_api.h` reported `heap_free` via `esp_get_free_heap_size()`, which returns the GLOBAL free heap including the ~8MB PSRAM - useless for judging internal-DRAM pressure (the actual stability-relevant number). Changed to `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`, matching the `heap_min` value already reported on the same page and the `memory.heap_free` in `/api/monitoring` (~60KB). The CPU Monitor "Heap frei" now shows real internal DRAM.

## 2026-07-01
- 1.2.12
- Heap headroom (PSRAM offloading): after the 1.2.11 transient fix `heap_min` recovered to ~27KB but a single ~20KB transient (heavy web-response builder) still briefly drove it to ~7KB - well below the 14000/15000 low-heap guards. To add a large permanent safety margin, `setup()` now calls `heap_caps_malloc_extmem_enable(1024)`, lowering the PSRAM allocation threshold from the compile-time default (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` = 4096) to 1024 bytes. Generic heap allocations >= 1024 B (web-response Strings, JSON documents, api_cache concatenations) now land in the 8MB PSRAM instead of the scarce internal DRAM, freeing internal RAM for lwIP/WiFi/DMA and the FieldMap/parser buffers. Capability-specific requests (`MALLOC_CAP_INTERNAL` / DMA, used by the network stack) are unaffected and stay internal, so stability is preserved. Expected result: internal `heap_min` stays well above the guard thresholds even during coincident web/MQTT/network activity.

## 2026-07-01
- 1.2.11
- Crash fix (root-cause, transient internal-DRAM spike): eliminated the recurring ~50KB transient that drove `heap_min` to a few hundred bytes and caused the OOM PANICs (seen at nrt:sys_loop AND nrt:web_loop - distributed, i.e. whichever unguarded allocation ran during the spike aborted). Root cause = three per-PWR-cycle DEEP COPIES of the 8-module `PwrBuffer` (each module holds a `FieldMap` of Strings): (1) `py_mqtt.cpp` copied the whole `PwrBuffer` into a local `pwrSnap` every publish cycle; (2) `py_uart.cpp` and (3) the HUB path in the sketch assigned `target->modules = mods` (copy) on every parse. FIXES: PWR is now published by reading the active double-buffer IN PLACE under `g_pwrMutex` (no `pwrSnap` copy); the two parser buffer-swaps use `std::move(mods)` instead of a deep copy. This removes hundreds of transient String allocations per cycle and keeps internal-DRAM headroom high, so coincident web/MQTT/network allocations no longer hit the floor.

## 2026-07-01
- 1.2.10
- Crash fix (PANIC nrt:sys_loop): with the 1.2.9 per-core breadcrumbs the recurring crash (~3:45h) was pinned to the Non-Critical **system loop** (`reset.last_stage_nrt = 230`), while `heap_min` reached ~3KB. Root cause: `WiFiManagerModule::loop()` and `EthManagerModule::loop()` call `ArduinoOTA.handle()` every cycle, an UNGUARDED lwIP/UDP `parsePacket()` (+ mDNS) that allocates from internal DRAM. When a transient elsewhere briefly drove internal heap near the floor, this unguarded network allocation was the one that tipped into OOM/abort - exactly the failure mode already guarded for the web server (`WEB_MIN_FREE_HEAP`). FIX: `ArduinoOTA.handle()` is now skipped in both loops when internal DRAM is below `OTA_MIN_FREE_HEAP` (14000); a skipped cycle is harmless (OTA retries; the web `/api/ota` upload path uses a different mechanism and is unaffected). Also split crash stage 230 into finer breadcrumbs 231 (wifi_loop) / 232 (eth_loop) / 233 (sysmgr_loop) to confirm the exact sub-call on any future sys-loop panic.
## 2026-07-01
- 1.2.9
- Crash diagnostics: per-core crash breadcrumbs. Root-cause analysis of the recurring PANIC every ~2.5-4.5h showed there is NO persistent heap leak (free internal DRAM recovers to ~55KB), but `heap_min` repeatedly hits ~260B - a rare transient allocation coincidence briefly consumes almost all internal DRAM and occasionally tips a concurrent allocation into OOM/abort. The reported crash stage was ALWAYS `130` (rt:wait_queue) because both FreeRTOS tasks shared one breadcrumb (`g_rtcLastStage`) and the Real-Time task's idle spin rewrites stage 130 every ~1ms - ~20x more often than the 20ms Non-Critical loop - masking the true fault location. Added a dedicated Non-Critical breadcrumb (`g_rtcLastStageNRT`, stages 200/210/220/230 = scheduler/MQTT/web/system) that is latched independently on crash and reported via `/api/monitoring` as `reset.last_stage_nrt` / `last_stage_nrt_name`. The next crash will reveal whether the fault occurs in the MQTT, web, or system pipeline so it can be fixed precisely.

## 2026-06-30
- 1.2.8
- Discovery heap-burst fix (renewed PANIC stage 130 / rt:wait_queue, heap_min ~8KB, core0 ~96%): once discovery actually worked again (1.2.7) the BAT phase published all 15 cells of a module - each with several fields - in one tight loop, a burst of dozens of JsonDocument+String allocations that spiked internal DRAM and crashed the device. BAT discovery now publishes ONE cell per loop iteration, spreading the allocations across iterations. Added a heap-floor guard in `handleDiscoveryStep`: if free internal DRAM is below `MQTT_MIN_FREE_HEAP` (15000), the step is deferred and retried next loop instead of allocating into the danger zone.

## 2026-06-29
- 1.2.7
- Discovery reliability fix: the discovery state machine received the per-iteration PWR snapshot, which is only populated when fresh PWR data arrived that exact loop. During the multi-iteration DISC_PWR/DISC_BAT phases the module list was usually empty, so module/cell discovery configs were often not (re)published and stale retained configs persisted. Discovery now uses the latest known PWR buffer, so re-running discovery actually overwrites the retained HA configs (e.g. after the BAL text-field fix).

## 2026-06-29
- 1.2.6
- Home Assistant discovery fix for text fields: balancer (BAL "N"/"Y") and state fields were declared with `state_class: measurement`, so HA rejected the non-numeric value and showed "Unknown". Text/unit-less fields now publish as plain sensors (no state_class/device_class/unit) in BAT cell discovery and the shared `addDiscoveryMeta` (STAT/INFO/PWR). After update the auto-discovery re-runs (~65s after boot) and overwrites the retained configs.

## 2026-06-29
- 1.2.5
- Console output cleanup: carriage returns are stripped and remaining control characters escaped so the last lines of pwr/bat/stat no longer overwrite/mix in the textarea; tabs render as spaces to keep columns aligned
- Console capture buffer is now mutex-protected; previously the realtimeTask could overwrite `consoleFrame` mid-read, corrupting the JSON response sent to the web console
- UART RX buffer enlarged to 4096B (`Serial2.setRxBufferSize`): the default 256B FIFO overflowed during long pwr/bat listings causing dropped/truncated module rows; full frames are now received intact

## 2026-06-28
- 1.2.4
- Battery-Console fix: console commands now return the full output reliably, no more missing lines or premature TIMEOUT; the complete raw response is captured into a dedicated buffer before the parser clears it (`consoleFrame`/`consoleSeq`), independent of frame validity so commands like `help`/`log` also display fully
- `/api/lastframe` returns the captured response as JSON with a sequence/command tag; the web console now polls until a fresh frame for the issued command arrives (up to 18s) instead of one short 2s wait
- Scheduled poll commands (pwr/bat/stat/info) keep priority; console commands run when the realtimeTask reaches them, so periodic monitoring is never starved

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