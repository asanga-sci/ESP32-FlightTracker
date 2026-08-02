# AGENTS.md — ESP32 Flight Tracker

## Project

PlatformIO / Arduino project for the **ESP32-2432S028R** ("Cheap Yellow Display"). Fetches live ADS-B data from FlightRadar24 and displays a radar-style UI on a 2.8" 240x320 ILI9341 TFT with resistive touch.

## Build & Flash

```bash
pio run                        # compile
pio run -t upload              # compile + flash
pio run -t uploadfs            # flash LittleFS filesystem image (airlines.txt, etc.)
pio device monitor             # serial monitor at 115200 baud (esp32_exception_decoder filter)
```

No test suite exists. The `test/` directory is empty.

## Architecture

**Dual-core FreeRTOS** — `setup()` creates pinned tasks then idles.

| Task | Core | Priority | Stack | Purpose |
|---|---|---|---|---|
| `UI_Task` | 0 | 4 | 16 KB | LVGL timer handler, screen updates, flight data rendering |
| `HTTP_Task` | 1 | 5 | 12 KB | Fetches flight data from FR24 API every 10 s |
| `Fetch_Logo_Task` | 1 | 2 | 8 KB | Fetches + decodes airline logo PNGs (optional, per config) |
| `resetBtn` | 0 | 1 | 2 KB | Watches BOOT button for 3 s hold → factory reset |

### Entry point

`src/main.cpp:setup()` — initializes TFT, LVGL, LittleFS, NVS config, then creates the tasks above.

### Key files

| File | Role |
|---|---|
| `src/main.cpp` | Application entry, task definitions, Haversine math, inter-task queues |
| `src/flight_info.cpp` | FR24 HTTP fetch, JSON parsing, PNG logo fetch + in-memory LRU cache |
| `src/web_config.cpp` | NVS config load/save, WiFi AP+STA, captive portal + settings page |
| `src/ui/flight_tracker_ui.c` | LVGL 9.x UI — radar screen, flight info panel, logo display (C file, extern "C") |
| `include/User_Setup.h` | TFT_eSPI pin/driver config for CYD hardware |
| `include/lv_conf.h` | LVGL build configuration (16-bit RGB565 color depth) |
| `boards/esp32-2432S028R.json` | Custom PlatformIO board definition with all GPIO/display/touch constants |
| `platformio.ini` | Build flags, library deps, partition scheme |
| `partitions.csv` | Custom partition table (`nvs` + `ota_0` 1.9 MB + `app` factory 2.1 MB) |

### Data flow

1. `HTTP_Task` calls `get_flights()` → fetches FR24 JSON → packs into `flights_wrapper_msg_t` → pushes to `g_flight_queue` (overwrite semantics).
2. `UI_Task` reads `g_flight_queue` → sorts by distance → updates LVGL widgets via `flight_tracker_update()`.
3. Logo requests go through `g_airline_logo_req_queue` → `Fetch_Logo_Task` fetches PNG, decodes with LodePNG, converts RGBA→BGRA, signals UI via `g_airline_logo_response_queue`.
4. All inter-task communication uses FreeRTOS queues (single-slot, overwrite-on-full).

### Configuration

- Stored in NVS (namespace `"fltcfg"`) via `Preferences` library.
- First boot: device starts as AP (`FlightTracker-Setup`, open, 192.168.4.1) with captive portal.
- Config fields: WiFi SSID/pass, lat/lon, radius (km), logoSupport (bool), logostreamApiKey.
- Access config from any task: `FlightConfig cfg = webConfigGet();` (mutex-protected snapshot).
- Factory reset: hold BOOT button 3 s, or POST to `/reset` in settings page.

### Filesystem

- **LittleFS** partition for runtime data.
- `data/airlines.txt` — airline ICAO→name lookup, format `ICAO|FullName` (one per line, 105 bytes/record). Uploaded via `pio run -t uploadfs`.

## Gotchas

- **LVGL is C.** `src/ui/flight_tracker_ui.c` uses `extern "C"` linkage. All LVGL API calls must respect this.
- **No LVGL thread-safety by default.** `flight_tracker_update()` and related UI functions must be called from the same task as `lv_timer_handler()`.
- **TFT driver is configured twice** — once in `include/User_Setup.h` (TFT_eSPI pins/driver) and once in `boards/esp32-2432S028R.json` (build flags). Changing one without the other will cause display failures.
- **Partition scheme matters.** The project uses `huge_app.csv` (referenced in `platformio.ini` as `board_partitions`). The custom `partitions.csv` in root has a different layout — clarify which is active.
- **No `#define BOARD_NAME` mismatch.** The build flag `-D BOARD_NAME="${this.board}"` resolves to `esp32-2432S028R` — do not hardcode this elsewhere.
- **Logo pipeline is RGBA→BGRA** — the display expects BGR byte order. Forgetting this swap produces correct-looking but wrong-color logos.
- **Memory is tight.** Heap checks are present in `http_fetch_task`. Vector reservations scale down when free heap is low (<30 flight_info worth). Do not add large stack allocations to tasks.
- **`xQueueOverwrite` only works on single-slot queues.** All queues are created with size 1.
- **NTP timezone is hardcoded** to `gmtOffset_sec = 25200` (UTC+7) in `main.cpp`. This is not configurable via the web portal.

## Libraries

| Library | Version | Purpose |
|---|---|---|
| TFT_eSPI | ^2.5.43 | ILI9341 display driver |
| ArduinoJson | ^7.2.2 | JSON parsing (v7 API) |
| LVGL | ^9.1.0 | UI framework |
| LodePNG | ^0.0.0 | PNG decoding for airline logos |
