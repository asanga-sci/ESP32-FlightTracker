/**
 * flight_tracker_ui.h
 * LVGL 9.5 Flight Tracker UI for ESP32 CYD 2.8" 240×320
 */
 
#pragma once
 
#ifdef __cplusplus
extern "C" {
#endif

typedef struct{
    float latitude;
    float longitude;
    int heading_deg;
    float distance_km;
    int bearing_deg;
    int altitude_ft;
    char icao_address[7];  // Store actual string, not pointer - prevents dangling pointers
} latlon_t;

typedef struct {
    char icao[8];
    char name[32];
    float latitude;
    float longitude;
    int runway_count;
    struct {
        char ident[8];
        float heading_deg;
        float length_ft;
        float width_ft;
    } runways[8];
} airport_runway_overlay_t;
 
/**
 * @brief  Build the entire flight-tracker screen.
 *
 * Prerequisites:
 *   - lv_init() called
 *   - lv_display_t created and flush callback registered
 *   - lv_display_set_buffers() NOT called yet — this function does it
 *     using its own static dual-buffers (240×10 lines, no PSRAM needed)
 *
 * Call once from app_main / setup() after display driver init.
 */
void flight_tracker_ui_init(void);
void setRadarOrigin(float latitude, float longitude);
void setRadarRadius(float radius_km);
 
/**
 * @brief  Inject fresh ADS-B telemetry into the UI.
 *
 * Thread-safe IF called from the same task as lv_timer_handler().
 * For FreeRTOS cross-task updates wrap in lv_lock / lv_unlock.
 */
void flight_tracker_update(const char *icao_address, const char *flight, const char *aircraft_code, const char *origin, const char *destination, float latitude, float longitude, float distance_km, int bearing_deg, int altitude_ft,
                           bool on_ground, int vertical_speed, int ground_speed, int heading_deg, const char *airline);
void update_planes_on_radar(latlon_t *planes, int count);
void update_airport_runways(const airport_runway_overlay_t *airports, int count);
void update_time(const char *time_str);
void update_logo(const lv_image_dsc_t *logo_dsc);

/* ═══════════════════════════════════════════════════════════════════════
 *  DOUBLE-BUFFER LOGO API (thread-safe, prevents memory corruption)
 * ═══════════════════════════════════════════════════════════════════════*/

/**
 * @brief Get pointer to buffer for decoding next logo.
 *        This buffer is not currently displayed (safe to write to).
 * @return Pointer to pixel data buffer (MAX_LOGO_SIZE = 256×256×4 bytes)
 */
uint8_t* get_next_logo_buffer(void);

/**
 * @brief Get descriptor struct for next logo.
 *        Fill width/height, then call commit_logo_buffer() to display.
 * @return Pointer to lv_image_dsc_t for next buffer
 */
lv_image_dsc_t* get_next_logo_descriptor(void);

/**
 * @brief Finalize logo decode and display it immediately.
 *        Swaps double-buffer: new logo becomes active, old buffer recycled.
 * 
 * @param width   Logo width (pixels)
 * @param height  Logo height (pixels)
 * 
 * Call after:
 *   1. Decode PNG to get_next_logo_buffer()
 *   2. Convert pixel format (RGBA → BGRA)
 *   3. Call this function with final dimensions
 */
void commit_logo_buffer(uint32_t width, uint32_t height);

/**
 * @brief Hide logo, show plane icon instead
 */
void hide_logo(void);
 
#ifdef __cplusplus
}
#endif