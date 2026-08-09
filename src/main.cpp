#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>
#include <TFT_eSPI.h> // hardware driver
#include <lvgl.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include "ui/flight_tracker_ui.h"
#include "flight_info.h"
#include "airport_info.h"
#include <lodepng.h>
#include "web_config.h"

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

// airline lookup defines
#define RECORD_SIZE 105
#define ICAO_LEN 3
#define NAME_LEN 100

// Hold BOOT (GPIO0) for 3 s to factory-reset.
static constexpr int RESET_BTN     = 0;
static constexpr uint32_t RESET_HOLD_MS = 3000;

static File airlines;

static EventGroupHandle_t s_cfgEvents = nullptr;

const char *ntpServer = "nl.pool.ntp.org";
const long gmtOffset_sec = 25200; // Adjust for your timezone
const int daylightOffset_sec = 0;

volatile bool screenshot_requested = false;

/**
 * Message from core 1 (HTTP) to core 0 (UI)
 * Lightweight structure - ONLY essential data, no pointers
 */
typedef struct
{
    char icao_address[7];       // ICAO 24-bit address as hex string (e.g. "4CA853")
    char flight[10];
    char aircraft_code[8];
    char origin[32];            // municipality from adsbdb (e.g. "Bangkok")
    char destination[32];       // municipality from adsbdb (e.g. "Khon Kaen")
    char airline[4];
    char airline_name[50]; // Resolved airline name from adsbdb
    float latitude;
    float longitude;
    float distance_km;
    int bearing_deg;
    int altitude_ft;
    int vertical_speed;
    int ground_speed;
    int heading_deg;
    bool on_ground;                  // alt_baro == "ground" from airplanes.live
} flight_msg_t;

typedef struct
{
    flight_msg_t flights[50];
    int count;
} flights_wrapper_msg_t;

typedef struct
{
    char airline[4];
} airline_logo_req_t;

typedef struct 
{
    char airline[4];
    uint8_t *pixel_data;
    unsigned int w;
    unsigned int h;
} decoded_airline_logo_t;

typedef struct {
    airport_runway_overlay_t airports[6];
    int count;
} airport_runway_payload_t;

static QueueHandle_t s_apQueue = nullptr;
static QueueHandle_t g_flight_queue = NULL;
static QueueHandle_t g_airline_logo_req_queue = NULL;
static QueueHandle_t g_airline_logo_response_queue = NULL;
static airport_runway_payload_t g_airport_runway_payload = {0};

static TaskHandle_t fetch_logo_task_handle;

TFT_eSPI tft = TFT_eSPI();

// --- LVGL 9.5 Display Buffer ---
static uint16_t disp_buf[SCREEN_WIDTH * 5]; // 10 rows buffer

// ── 2. Flush callback — LVGL calls this when a region is ready to send ─
void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)px_map, w * h, true);
    tft.endWrite();

    lv_display_flush_ready(disp); // ← MUST call this or LVGL hangs
}

static uint32_t my_tick_get_cb(void)
{
    return millis();
}

static void take_screenshot()
{
    // The screenshot shares UART0 with ESP_LOG. At 115200 baud, transmitting
    // 153600 bytes takes several seconds, so any log line emitted during the
    // transfer would corrupt the binary payload and shift the end marker.
    Serial.flush();
    esp_log_level_set("*", ESP_LOG_NONE);

    Serial.println("BEGIN_SCREENSHOT");

    // LVGL needs a few alignment bytes in addition to the visible RGB565
    // payload. A buffer sized only width * height * 2 can make snapshot
    // reshape fail before any metadata is sent.
    size_t snapshot_size = LV_DRAW_BUF_SIZE(240, 320, LV_COLOR_FORMAT_RGB565);

    uint8_t *snapshot_buf =
        (uint8_t *)heap_caps_malloc(
            snapshot_size,
            MALLOC_CAP_SPIRAM);

    if (!snapshot_buf)
    {
        Serial.println("PSRAM allocation failed");
        Serial.flush();
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }

    lv_draw_buf_t draw_buf;

    const lv_result_t init_result = lv_draw_buf_init(
        &draw_buf,
        240,
        320,
        LV_COLOR_FORMAT_RGB565,
        LV_STRIDE_AUTO,
        snapshot_buf,
        snapshot_size);

    const lv_result_t snapshot_result =
        init_result == LV_RESULT_OK
            ? lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf)
            : LV_RESULT_INVALID;

    if (init_result != LV_RESULT_OK || snapshot_result != LV_RESULT_OK || !draw_buf.data)
    {
        Serial.println("SNAPSHOT_FAILED");
        Serial.flush();
        heap_caps_free(snapshot_buf);
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }

    uint32_t width = draw_buf.header.w;
    uint32_t height = draw_buf.header.h;
    uint32_t data_size = draw_buf.data_size;

    Serial.printf("SCREENSHOT_INFO %u %u %u\n",
                  width, height, data_size);

    // Send raw RGB565 data
    Serial.write((uint8_t *)draw_buf.data, data_size);
    Serial.flush();

    Serial.println("END_SCREENSHOT");
    Serial.flush();

    lv_draw_buf_destroy(&draw_buf);

    esp_log_level_set("*", ESP_LOG_INFO);
    Serial.printf("[SS] screenshot sent: %u bytes\n", data_size);
}

void display_setup_screen(const char* ssid, const char* password, const char* ip)
{
    lv_obj_t *setup_overlay = lv_obj_create(lv_screen_active());

    lv_obj_remove_style_all(setup_overlay);

    lv_obj_set_size(setup_overlay, 240, 320);

    lv_obj_set_style_bg_color(setup_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(setup_overlay, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_label_create(setup_overlay);
    lv_label_set_text(logo, "\ue6ca");
    lv_obj_set_style_text_font(logo, &material_icons, 0);
    lv_obj_set_style_text_color(logo, lv_color_hex(0x00FF66), 0);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 20);

    // Title
    lv_obj_t *title = lv_label_create(setup_overlay);
    lv_label_set_text(title, "FlightTracker Setup");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    // Instructions
    lv_obj_t *instr = lv_label_create(setup_overlay);
    lv_label_set_text(instr, "Connect to WiFi:\nFlightTracker-Setup\n\nThen open:\n192.168.4.1");
    lv_label_set_long_mode(instr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(instr, 220);
    lv_obj_align(instr, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_move_foreground(setup_overlay);
}
 
// ─── WebConfig callback (runs on webConfig task — NO lv_* here) ──────────────
void onAPReady(const char* ssid, const char* password, const char* ip) {
    if (!s_apQueue) return;
 
    APInfo info{};
    strncpy(info.ssid,     ssid,     sizeof(info.ssid)     - 1);
    strncpy(info.password, password, sizeof(info.password) - 1);
    strncpy(info.ip,       ip,       sizeof(info.ip)       - 1);
 
    xQueueOverwrite(s_apQueue, &info);
}

// Radar origin (home location)
static float radar_origin_lat = 0;
static float radar_origin_lon = 0;

// Calculate distance between two lat/lon points (Haversine formula) - returns km
float calc_dist_km(float lat1, float lon1, float lat2, float lon2)
{
    float lat1_r = lat1 * M_PI / 180, lon1_r = lon1 * M_PI / 180;
    float lat2_r = lat2 * M_PI / 180, lon2_r = lon2 * M_PI / 180;
    float dlat = lat2_r - lat1_r, dlon = lon2_r - lon1_r;
    float a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1_r) * cos(lat2_r) * sin(dlon / 2) * sin(dlon / 2);
    return 6371 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Calculate bearing from point 1 to point 2 - returns degrees (0-359)
int calc_bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    float lat1_r = lat1 * M_PI / 180, lon1_r = lon1 * M_PI / 180;
    float lat2_r = lat2 * M_PI / 180, lon2_r = lon2 * M_PI / 180;
    float dlon = lon2_r - lon1_r;
    float y = sin(dlon) * cos(lat2_r);
    float x = cos(lat1_r) * sin(lat2_r) - sin(lat1_r) * cos(lat2_r) * cos(dlon);
    return (int)fmod(atan2(y, x) * 180 / M_PI + 360, 360);
}

// Comparator to sort flights by distance (ascending - closest first)
bool compare_flights_by_distance(const flight_msg_t &a, const flight_msg_t &b)
{
    float dist_a = calc_dist_km(radar_origin_lat, radar_origin_lon, a.latitude, a.longitude);
    float dist_b = calc_dist_km(radar_origin_lat, radar_origin_lon, b.latitude, b.longitude);
    return dist_a < dist_b;
}

bool lookupAirline(fs::File &f, const char *icao, char *nameOut)
{
    // Reset file pointer to beginning
    f.seek(0);

    char line[256];
    while (f.available())
    {
        // Read line until newline or EOF
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len <= 0)
            break;

        line[len] = '\0';

        // Remove carriage return if present
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        // Parse "ICAO|Name" format
        char *pipe = strchr(line, '|');
        if (!pipe)
            continue;

        // Extract ICAO (first 3 chars before pipe)
        if (pipe - line != ICAO_LEN)
            continue;

        if (strncmp(icao, line, ICAO_LEN) == 0)
        {
            // Found it! Copy name after pipe
            strncpy(nameOut, pipe + 1, NAME_LEN);
            nameOut[NAME_LEN] = '\0';
            return true;
        }
    }
    return false; // not found
}

/* ─────────────────────── CORE 1: HTTP Fetcher Task ──────────────────────────── */

/**
 * Run on core 1 - fetches flight data from ADS-B endpoint
 * Runs in its own task to avoid blocking UI
 */
void http_fetch_task(void *param)
{
    (void)param;
    FlightConfig cfg = webConfigGet();

    log_i("[Flight Fetch Task] Waiting for network…");
    xEventGroupWaitBits(s_cfgEvents, CFG_EVT_READY, pdFALSE, pdTRUE, portMAX_DELAY);

    log_i("[Flight Fetch Task]: Starting on core %d", xPortGetCoreID());
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(NULL);
    log_i("[Flight Fetch Task]: Initial stack high water mark: %u bytes", highWaterMark * sizeof(StackType_t));

    int base_fetch_interval_ms = 10000; /* Start with a 10s base poll interval */
    int fetch_interval_ms = base_fetch_interval_ms;
    uint32_t last_fetch = millis();
    uint32_t last_http_hb = 0;
    while (true)
    {
        uint32_t now = millis();
        if (now - last_http_hb >= 2000)
        {
            last_http_hb = now;
            log_i("[HTTP] alive @%u ms, free heap=%u, HTTP stack HWM=%u",
                  (unsigned)now, (unsigned)ESP.getFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (now - last_fetch >= fetch_interval_ms)
        {
            last_fetch = now;
            log_i("[Flight Fetch Task]: Fetching flight data...");
            static flights_wrapper_msg_t wrapper_msg;
            String error_message;
            std::vector<flight_info> local_flights;

            /* No explicit reserve: get_flights grows via push_back, and a large
               upfront alloc was the OOM point. Heap guards live in get_flights(). */
            log_i("[Flight Fetch Task]: sizeof(flight_info)=%u, free=%u, largest=%u",
                  (unsigned)sizeof(flight_info), (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

            // Fetch into temporary list (no mutex needed here)
            // range_latitude now carries the search radius in km
            get_flights(cfg.lat, cfg.lon, cfg.radiusKm, cfg.radiusKm, true, false, true, true, local_flights, error_message);

            const bool have_flight_data = !local_flights.empty();
            int target_interval_ms = base_fetch_interval_ms;
            if (!error_message.isEmpty())
            {
                target_interval_ms = 15000;
                log_e("[Flight Fetch Task]: Error fetching flights: %s", error_message.c_str());
            }
            else if (!have_flight_data)
            {
                target_interval_ms = 15000;
                log_w("[Flight Fetch Task]: Empty flight payload, slowing polling to 30s");
            }
            else
            {
                flight_msg_t flight_msg = {0};
                wrapper_msg.count = 0;
                // Send to UI queue (non-blocking)
                for (const flight_info &f : local_flights)
                {
                    if (wrapper_msg.count >= (int)(sizeof(wrapper_msg.flights) / sizeof(wrapper_msg.flights[0])))
                    {
                        log_w("Flight queue full, dropping %u remaining aircraft", local_flights.size() - wrapper_msg.count);
                        break;
                    }

                    /* Calculate distance and bearing */
                    double dist = calc_dist_km(cfg.lat, cfg.lon, f.latitude, f.longitude);
                    int bearing = calc_bearing_deg(cfg.lat, cfg.lon, f.latitude, f.longitude);

                    strncpy(flight_msg.icao_address, f.icao_address.c_str(), sizeof(flight_msg.icao_address) - 1);
                    flight_msg.icao_address[sizeof(flight_msg.icao_address) - 1] = '\0';

                    /* Build message - copy strings to avoid pointers to freed memory */
                    strncpy(flight_msg.flight, f.iata_callsign.c_str(), sizeof(flight_msg.flight) - 1);
                    flight_msg.flight[sizeof(flight_msg.flight) - 1] = '\0';

                    strncpy(flight_msg.aircraft_code, f.aircraft_code.c_str(), sizeof(flight_msg.aircraft_code) - 1);
                    flight_msg.aircraft_code[sizeof(flight_msg.aircraft_code) - 1] = '\0';

                    strncpy(flight_msg.origin, f.origin_airport.c_str(), sizeof(flight_msg.origin) - 1);
                    flight_msg.origin[sizeof(flight_msg.origin) - 1] = '\0';

                    strncpy(flight_msg.destination, f.destination_airport.c_str(), sizeof(flight_msg.destination) - 1);
                    flight_msg.destination[sizeof(flight_msg.destination) - 1] = '\0';

                    strncpy(flight_msg.airline, f.icao_airline.c_str(), sizeof(flight_msg.airline) - 1);
                    flight_msg.airline[sizeof(flight_msg.airline) - 1] = '\0';

                    strncpy(flight_msg.airline_name, f.airline_name.c_str(), sizeof(flight_msg.airline_name) - 1);
                    flight_msg.airline_name[sizeof(flight_msg.airline_name) - 1] = '\0';

                    flight_msg.latitude = f.latitude;
                    flight_msg.longitude = f.longitude;
                    flight_msg.distance_km = dist;
                    flight_msg.bearing_deg = bearing;
                    flight_msg.altitude_ft = f.altitude;
                    flight_msg.vertical_speed = f.vertical_speed;
                    flight_msg.ground_speed = f.ground_speed;
                    flight_msg.heading_deg = f.heading;
                    flight_msg.on_ground = f.on_ground;

                    wrapper_msg.flights[wrapper_msg.count++] = flight_msg;
                }
                local_flights.clear();
                /* Send to UI queue (non-blocking) - overwrites if queue full */
                xQueueOverwrite(g_flight_queue, &wrapper_msg);
            }

            /* Add a small random jitter to avoid synchronized polling bursts */
            int jitter_ms = (millis() % 3000) - 1500;
            fetch_interval_ms = target_interval_ms + jitter_ms;
            if (fetch_interval_ms < 8000) {
                fetch_interval_ms = 8000;
            }
            if (fetch_interval_ms > 32000) {
                fetch_interval_ms = 32000;
            }
            log_i("[Flight Fetch Task]: Next poll in %d ms", fetch_interval_ms);
            log_i("[Flight Fetch Task]: Fetch flight data task stack high water mark: %u words", uxTaskGetStackHighWaterMark(NULL));
            // one-shot check for logo task
            log_i("[Flight Fetch Task]: Logo task stack HWM: %u words", uxTaskGetStackHighWaterMark(fetch_logo_task_handle));      
        }
        vTaskDelay(100 / portTICK_PERIOD_MS); // Sleep briefly to yield CPU
    }
    vTaskDelete(NULL);
}

/* ─────────────────────── CORE 0: UI Task ──────────────────────────────── */

void ui_update_task(void *param)
{
    (void)param;
    log_i("UI Task: Starting on core %d", xPortGetCoreID());

    log_i("UI Task: waiting for runway fetch to finish before initializing UI");

    bool setup_screen_shown = false;
    FlightConfig cfg = webConfigGet();
    log_i("location from config: lat=%.4f, lon=%.4f, radius=%f", cfg.lat, cfg.lon, cfg.radiusKm);
    
    /* Initialize display - order matters here to properly set radar ring labels*/
    setRadarOrigin(cfg.lat, cfg.lon);
    setRadarRadius(cfg.radiusKm); // default 50 km radar radius
    flight_tracker_ui_init();

    uint32_t last_update = 0;
    uint32_t last_ui_hb = 0;
    String previous_tracked_flight;
    char airline_name[NAME_LEN + 1] = {0};

    while (1)
    {
        bool network_ok = xEventGroupWaitBits(s_cfgEvents, CFG_EVT_READY, pdFALSE, pdTRUE, 0) & CFG_EVT_READY;
        if(!network_ok && !setup_screen_shown){
            APInfo ap_info;
            if (xQueuePeek(s_apQueue, &ap_info, 0) == pdTRUE)
            {
                log_i("UI Task: Received AP info, displaying setup screen...");
                display_setup_screen(ap_info.ssid, ap_info.password, ap_info.ip);
            }
            setup_screen_shown = true;
        }
        if (g_airport_runway_payload.count > 0)
        {
            update_airport_runways(g_airport_runway_payload.airports, g_airport_runway_payload.count);
        }

        /* Check for new flight data from HTTP task */
        static flights_wrapper_msg_t wrapper_msg;
        std::vector<latlon_t> planes_in_radar;
        if (xQueueReceive(g_flight_queue, &wrapper_msg, 0) == pdTRUE)
        {
            if (wrapper_msg.count > 0)
            {
                std::list<flight_msg_t> flight_list(wrapper_msg.flights, wrapper_msg.flights + wrapper_msg.count);
                // Sort flights by distance (closest first)
                flight_list.sort([](const flight_msg_t &a, const flight_msg_t &b)
                                 { return a.distance_km < b.distance_km; });
                const flight_msg_t &msg = flight_list.front(); // Closest flight

                log_i("UI Task: Received flight data, updating UI...");

                // resolve airline name — prefer adsbdb, fall back to LittleFS lookup.
                // Re-evaluate every fetch so a stale name doesn't stick when adsbdb
                // returns "unknown callsign" for the tracked flight.
                const bool adsb_has_airline = (msg.airline_name[0] != '\0');
                if (strcmp(previous_tracked_flight.c_str(), msg.flight) != 0)
                {
                    if (adsb_has_airline)
                    {
                        strncpy(airline_name, msg.airline_name, NAME_LEN);
                        airline_name[NAME_LEN] = '\0';
                        log_i("Airline name from adsbdb: %s", airline_name);
                    }
                    else if (lookupAirline(airlines, msg.airline, airline_name))
                    {
                        log_i("Resolved airline name (lookup): %s", airline_name);
                    }
                    else
                    {
                        strncpy(airline_name, "N/A", NAME_LEN);
                        airline_name[NAME_LEN] = '\0';
                        log_i("No airline data for %s, showing N/A", msg.flight);
                    }
                    previous_tracked_flight = msg.flight; // Update tracked flight to avoid redundant lookups
                    if (webConfigGet().logoSupport)
                    {
                        log_i("prepare logo req for airline : %s", msg.airline);
                        airline_logo_req_t req;
                        strncpy(req.airline, msg.airline, sizeof(msg.airline) - 1);
                        req.airline[sizeof(msg.airline) - 1] = '\0';
                        log_i("debug: airline %s",req.airline);
                        if(xQueueOverwrite(g_airline_logo_req_queue, &req) != pdTRUE){
                            log_e("Logo queue send FAILED - queue full?");
                        }
                    }
                }
                else if (adsb_has_airline && strcmp(airline_name, msg.airline_name) != 0)
                {
                    // adsbdb resumed resolving this callsign — restore the real name
                    strncpy(airline_name, msg.airline_name, NAME_LEN);
                    airline_name[NAME_LEN] = '\0';
                    log_i("Airline name from adsbdb (recovered): %s", airline_name);
                }
                else if (!adsb_has_airline && msg.airline[0] != '\0')
                {
                    // adsbdb no longer resolves this callsign ("unknown callsign").
                    // The AirLabs fallback may still have supplied the airline ICAO
                    // code — retry the LittleFS lookup before giving up.
                    if (strcmp(airline_name, "N/A") != 0 && lookupAirline(airlines, msg.airline, airline_name))
                    {
                        log_i("Resolved airline name (lookup fallback): %s", airline_name);
                    }
                    else
                    {
                        strncpy(airline_name, "N/A", NAME_LEN);
                        airline_name[NAME_LEN] = '\0';
                        log_i("adsbdb: no airline data for %s, showing N/A", msg.flight);
                    }
                }

                /* Update main flight display */
                flight_tracker_update(
                    msg.icao_address[0] ? msg.icao_address : "N/A",
                    msg.flight[0] ? msg.flight : "N/A",
                    msg.aircraft_code[0] ? msg.aircraft_code : "N/A",
                    msg.origin[0] ? msg.origin : "N/A",
                    msg.destination[0] ? msg.destination : "N/A",
                    msg.latitude,
                    msg.longitude,
                    msg.distance_km,
                    msg.bearing_deg,
                    msg.altitude_ft,
                    msg.on_ground,
                    msg.vertical_speed,
                    msg.ground_speed,
                    msg.heading_deg,
                    airline_name ? airline_name : "N/A");
            }
            else
            {
                flight_tracker_update(
                    "----",
                    "----",
                    "----",
                    "----",
                    "----",
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,    
                    0.0,
                    false,
                    0.0,
                    0.0,
                    "----");
                hide_logo();
            }
            for (size_t i = 0; i < wrapper_msg.count; i++)
            {
                const flight_msg_t &msg = wrapper_msg.flights[i];
                latlon_t plane = {msg.latitude, msg.longitude, msg.heading_deg, msg.distance_km, msg.bearing_deg, msg.altitude_ft, ""};
                strncpy(plane.icao_address, msg.icao_address, sizeof(plane.icao_address) - 1);
                plane.icao_address[sizeof(plane.icao_address) - 1] = '\0';
                planes_in_radar.push_back(plane);
            }
            update_planes_on_radar(planes_in_radar.data(), planes_in_radar.size());
            log_i("update UI task stack high water mark: %u words", uxTaskGetStackHighWaterMark(NULL));
        }

        decoded_airline_logo_t logo_respnse;
        if (xQueueReceive(g_airline_logo_response_queue, &logo_respnse, 0) == pdTRUE)
        {
            log_i("apply new logo for: %s", logo_respnse.airline);
            /* With double-buffering, commit_logo_buffer() swaps and displays immediately.
               No malloc needed — buffers are pre-allocated and reused. */
            commit_logo_buffer(logo_respnse.w, logo_respnse.h);
        }

        /* Update UI (50 ms = 20 fps) */
        uint32_t t0_lv = millis();
        lv_timer_handler();
        uint32_t dt_lv = millis() - t0_lv;
        if (dt_lv > 200)
            log_w("lv_timer_handler took %u ms", (unsigned)dt_lv);

        /* Update time every second */
        if (millis() - last_update >= 1000)
        {
            last_update = millis();

            time_t now = time(nullptr);
            struct tm *timeinfo = localtime(&now);
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", timeinfo);
            update_time(time_str);
        }

        uint32_t now_ui = millis();
        if (now_ui - last_ui_hb >= 2000)
        {
            last_ui_hb = now_ui;
            log_i("[UI] alive @%u ms, free heap=%u, largest=%u, UI stack HWM=%u",
                  (unsigned)now_ui, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if ((now_ui / 4000) % 2 == 0)
            {
                if (heap_caps_check_integrity_all(true))
                    log_i("[UI] heap integrity OK");
                else
                    log_e("[UI] HEAP CORRUPTED!");
            }
        }

        if (screenshot_requested)
        {
            screenshot_requested = false;

            take_screenshot();
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void airport_runway_fetch_task()
{
    log_i("Airport Runway Task: waiting for config readiness");
    xEventGroupWaitBits(s_cfgEvents, CFG_EVT_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000));

    FlightConfig cfg = webConfigGet();
    log_i("Airport Runway Task: fetching nearby airports around %.4f, %.4f with radius %.1f km",
          cfg.lat, cfg.lon, cfg.radiusKm);

    std::vector<airport_runway_info> nearby_airports;
    String error_message;
    if (!get_nearby_airports_and_runways(cfg.lat, cfg.lon, cfg.radiusKm, nearby_airports, error_message))
    {
        log_e("Airport Runway Task: %s", error_message.c_str());
        return;
    }

    memset(&g_airport_runway_payload, 0, sizeof(g_airport_runway_payload));
    for (const airport_runway_info &airport : nearby_airports)
    {
        airport_runway_overlay_t overlay = {0};
        strncpy(overlay.icao, airport.icao, sizeof(overlay.icao) - 1);
        overlay.icao[sizeof(overlay.icao) - 1] = '\0';
        strncpy(overlay.name, airport.name, sizeof(overlay.name) - 1);
        overlay.name[sizeof(overlay.name) - 1] = '\0';
        overlay.latitude = airport.latitude;
        overlay.longitude = airport.longitude;
        overlay.runway_count = (int)std::min<size_t>(airport.runways.size(), sizeof(overlay.runways) / sizeof(overlay.runways[0]));

        for (int i = 0; i < overlay.runway_count; ++i)
        {
            const runway_info &runway = airport.runways[i];
            strncpy(overlay.runways[i].ident, runway.ident, sizeof(overlay.runways[i].ident) - 1);
            overlay.runways[i].ident[sizeof(overlay.runways[i].ident) - 1] = '\0';
            overlay.runways[i].heading_deg = runway.heading_deg;
            overlay.runways[i].length_ft = runway.length_ft;
            overlay.runways[i].width_ft = runway.width_ft;
        }

        g_airport_runway_payload.airports[g_airport_runway_payload.count++] = overlay;
        if (g_airport_runway_payload.count >= 6)
        {
            break;
        }
    }

    log_i("Airport Runway Task: complete, %d airports stored", g_airport_runway_payload.count);
}

#define LOGO_FETCH_RETRIES   3
#define LOGO_RETRY_DELAY_MS  2000

void fetch_logo_task(void *param)
{
    (void)param;

    log_i("Fetch Logo Task: Starting on core %d", xPortGetCoreID());

    std::vector<uint8_t> png_buffer;
    png_buffer.reserve(4096);

    while (1)
    {
        airline_logo_req_t req;
        if (xQueueReceive(g_airline_logo_req_queue, &req, 1) == pdTRUE)
        {
            log_i("Logo request for: %s", req.airline);

            /* ── FETCH + DECODE, retrying transient failures ──
               A single failed fetch used to leave the fallback plane icon
               forever (the request only fires once per tracked flight). */
            bool ok = false;
            bool dropped_for_newer = false;
            for (int attempt = 1; attempt <= LOGO_FETCH_RETRIES && !ok; attempt++)
            {
                /* A newer request arrived while we were retrying — let the
                   main loop serve it instead of stalling on a stale airline. */
                if (attempt > 1)
                {
                    airline_logo_req_t pending;
                    if (xQueuePeek(g_airline_logo_req_queue, &pending, 0) == pdTRUE)
                    {
                        log_i("Logo task: newer request queued (%s), dropping stale %s",
                              pending.airline, req.airline);
                        dropped_for_newer = true;
                        break;
                    }
                }

                String error_msg;
                png_buffer.clear();
                size_t png_size = get_logo(req.airline, png_buffer, error_msg);

                if (png_size == 0)
                {
                    log_w("Logo fetch failed (attempt %d/%d): %s",
                          attempt, LOGO_FETCH_RETRIES, error_msg.c_str());
                    if (attempt < LOGO_FETCH_RETRIES)
                        vTaskDelay(pdMS_TO_TICKS(LOGO_RETRY_DELAY_MS));
                    continue;
                }
                log_i("PNG received: %s (%u bytes)", req.airline, png_size);

                /* ── DECODE PNG LOCALLY ── */
                uint8_t *next_logo_buf = get_next_logo_buffer();
                uint8_t *decoded_pixels = NULL;
                unsigned int w, h;

                const unsigned int res = lodepng_decode32(&decoded_pixels, &w, &h, png_buffer.data(), png_size);
                if (res > 0)
                {
                    log_e("lodePNG error (attempt %d/%d): %s",
                          attempt, LOGO_FETCH_RETRIES, lodepng_error_text(res));
                    if (decoded_pixels) free(decoded_pixels);
                    png_cache_remove(req.airline); /* don't cache corrupt/undecodable PNGs */
                    if (attempt < LOGO_FETCH_RETRIES)
                        vTaskDelay(pdMS_TO_TICKS(LOGO_RETRY_DELAY_MS));
                    continue;
                }

                /* Check bounds */
                const uint32_t required_bytes = w * h * 4;
                if (required_bytes > 5000)
                {
                    log_e("Logo too large: %ux%u = %u bytes", w, h, required_bytes);
                    free(decoded_pixels);
                    png_cache_remove(req.airline);
                    if (attempt < LOGO_FETCH_RETRIES)
                        vTaskDelay(pdMS_TO_TICKS(LOGO_RETRY_DELAY_MS));
                    continue;
                }

                /* Convert color format: RGBA → BGRA */
                for (uint32_t i = 0; i < (uint32_t)w * h; i++)
                {
                    uint8_t r = decoded_pixels[i * 4 + 0];
                    uint8_t g = decoded_pixels[i * 4 + 1];
                    uint8_t b = decoded_pixels[i * 4 + 2];
                    uint8_t a = decoded_pixels[i * 4 + 3];

                    next_logo_buf[i * 4 + 0] = b;
                    next_logo_buf[i * 4 + 1] = g;
                    next_logo_buf[i * 4 + 2] = r;
                    next_logo_buf[i * 4 + 3] = a;
                }

                free(decoded_pixels);

                /* Queue signal to UI task (double-buffer swap + display) */
                decoded_airline_logo_t msg = {
                    .pixel_data = (uint8_t*)0xDEADBEEF,  /* Sentinel */
                    .w = w,
                    .h = h,
                };
                strncpy(msg.airline, req.airline, sizeof(msg.airline) - 1);
                msg.airline[sizeof(msg.airline) - 1] = '\0';

                xQueueOverwrite(g_airline_logo_response_queue, &msg);
                log_i("Logo decoded & queued: %s (%ux%u)", req.airline, w, h);
                ok = true;
            }

            if (!ok && !dropped_for_newer)
            {
                log_e("Logo request failed for %s after %d attempts — showing fallback",
                      req.airline, LOGO_FETCH_RETRIES);
                decoded_airline_logo_t clear_msg = { .w = 0, .h = 0 };
                strncpy(clear_msg.airline, req.airline, sizeof(clear_msg.airline) - 1);
                xQueueOverwrite(g_airline_logo_response_queue, &clear_msg);
            }

            log_i("Fetch Logo task HWM: %u words", uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ─── Reset-button task ────────────────────────────────────────────────────────
static void resetBtnTask(void*) {
    pinMode(RESET_BTN, INPUT_PULLUP);
    uint32_t heldSince = 0;
 
    while (true) {
        if (digitalRead(RESET_BTN) == LOW) {
            if (heldSince == 0) heldSince = millis();
            if (millis() - heldSince >= RESET_HOLD_MS) {
                log_w("[Main] Reset button held — factory reset");
                webConfigReset();   // does not return
            }
        } else {
            heldSince = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

//-----------------------ss task-----------------------------
static void screenshot_serial_task(void *parameter)
{
    char buffer[16];

    while (true)
    {
        if (Serial.available())
        {
            size_t len = Serial.readBytesUntil(
                '\n',
                buffer,
                sizeof(buffer) - 1
            );

            buffer[len] = '\0';

            // Remove CR
            if (len > 0 && buffer[len - 1] == '\r')
            {
                buffer[len - 1] = '\0';
            }

            if (strcmp(buffer, "ss") == 0)
            {
                Serial.println("SS_REQUESTED");

                screenshot_requested = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void setup()
{
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_INFO);

    // ── A. Init TFT hardware ───────────────────────────────────────────
    tft.begin();        // SPI init, reset, wake display IC
    tft.setRotation(2); // portrait, adjust for your mounting

    // ── B. Init LVGL engine ───────────────────────────────────────────
    lv_init();
    lv_tick_set_cb(my_tick_get_cb); // ← MUST set tick callback for timing

    // ── C. Create a display object and wire everything together ───────
    lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, disp_buf, NULL, sizeof(disp_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_apQueue = xQueueCreate(1, sizeof(APInfo));
    g_flight_queue = xQueueCreate(1, sizeof(flights_wrapper_msg_t));
    g_airline_logo_req_queue = xQueueCreate(1, sizeof(airline_logo_req_t));
    g_airline_logo_response_queue = xQueueCreate(1, sizeof(decoded_airline_logo_t));

    s_cfgEvents = webConfigInit(1, onAPReady);
    FlightConfig cfg = webConfigGet();

    // ── E. Init filesystem and load airline data ───────────────────────
    if (!LittleFS.begin())
    {
        Serial.println("Failed to mount LittleFS");
        return;
    }
    airlines = LittleFS.open("/airlines.txt", "r");

    // Init and get the time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    /* ═══════════════════════════════════════════════════════════════ */
    /* CREATE DUAL-CORE TASKS */
    /* ═══════════════════════════════════════════════════════════════ */

    /* Fetch airport/runway data synchronously into the global before
       tasks start, so the UI task can draw runways on the radar. */
    log_i("Fetching airport runway data...");
    airport_runway_fetch_task();

    /* Core 1: HTTP fetching */
    log_i("Creating HTTP task on core 1");
    xTaskCreatePinnedToCore(
        http_fetch_task, /* Task function */
        "HTTP_Task",     /* Task name */
        16384,           /* Stack size (bytes) - TLS handshake ~8KB + frames; the 9KB flights_wrapper_msg_t is static, so 32KB was overkill */
        NULL,            /* Parameters */
        5,               /* Priority */
        NULL,            /* Task handle */
        1                /* Core 1 */
    );

    /* Core 0: UI rendering (LVGL) */
    log_i("Creating UI task on core 0");
    xTaskCreatePinnedToCore(
        ui_update_task, /* Task function */
        "UI_Task",      /* Task name */
        16384,          /* Stack size (bytes) */
        NULL,           /* Parameters */
        4,              /* Priority (higher = more priority) */
        NULL,           /* Task handle */
        0               /* Core 0 */
    );

    if (cfg.logoSupport)
    {
        log_i("Creating Fetch Logo task on core 1");
        xTaskCreatePinnedToCore(
            fetch_logo_task,         /* Task function */
            "Fetch_Logo_Task",       /* Task name */
            8096,                    /* Stack size (bytes) - HTTP needs more */
            NULL,                    /* Parameters */
            2,                       /* Priority */
            &fetch_logo_task_handle, /* Task handle */
            1                        /* Core 1 */
        );
    }

    // Reset-button watcher — tiny stack, lowest priority, either core is fine.
    xTaskCreatePinnedToCore(
        resetBtnTask,
        "resetBtn",
        2048, 
        nullptr, 
        1, 
        nullptr, 
        0);

    xTaskCreatePinnedToCore(
        screenshot_serial_task,
        "SS Serial",
        2048,
        nullptr,
        1,
        nullptr,
        0);

    log_i("Setup: Dual-core tasks created!");
}

void loop()
{
    /* In FreeRTOS, the loop is handled by task scheduler */
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}