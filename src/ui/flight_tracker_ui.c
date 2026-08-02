/**
 * Flight Tracker UI — LVGL 9.5 for ESP32 CYD (2.8" 240×320, no PSRAM)
 *
 * Memory strategy:
 *  - Dual draw buffers: 240 × 10 × 2 bytes = 4800 bytes each  → 9600 bytes total
 *    (never exceed ~10 lines; CYD has ~320KB SRAM, keep buffers small)
 *  - All canvas drawing done with LVGL draw descriptors — no raw malloc bitmaps
 *  - Radar sweep uses a single LVGL canvas of 160×160 (51 200 bytes RGB565) placed
 *    inside a clip area; that is the only large allocation and fits comfortably
 *  - Fonts: built-in lv_font_montserrat_* (link only the sizes you use in lv_conf.h)
 *
 * lv_conf.h prerequisites (enable these):
 *   #define LV_FONT_MONTSERRAT_10  1
 *   #define LV_FONT_MONTSERRAT_12  1
 *   #define LV_FONT_MONTSERRAT_14  1
 *   #define LV_FONT_MONTSERRAT_28  1
 *   #define LV_USE_CANVAS          1
 *   #define LV_USE_LABEL           1
 *   #define LV_USE_LINE            1
 *   #define LV_USE_ARC             1
 *   #define LV_USE_TIMER           1   (lv_timer)
 *
 * Pin / driver wiring is board-specific; hook lv_display_set_flush_cb() to your
 * ILI9341 / ST7789 driver before calling flight_tracker_ui_init().
 */

#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Arduino.h>
#include <esp_log.h>
#include <esp32-hal-log.h>
#include "flight_tracker_ui.h"
#include "img_plane.h"

/* ───────────────────────────── palette ──────────────────────────────── */
#define CLR_BG lv_color_hex(0x060E18)        /* near-black navy      */
#define CLR_PANEL lv_color_hex(0x0A1628)     /* slightly lighter     */
#define CLR_BORDER lv_color_hex(0x1A3A5C)    /* dim blue border      */
#define CLR_GREEN lv_color_hex(0x00FF66)     /* radar / accent green */
#define CLR_GREEN_DIM lv_color_hex(0x007733) /* dimmer green         */
#define CLR_CYAN lv_color_hex(0x00CCFF)      /* blue-cyan highlights */
#define CLR_WHITE lv_color_hex(0xFFFFFF)
#define CLR_GRAY lv_color_hex(0x8899AA)
#define CLR_ALT_LOW lv_color_hex(0x1A7A3C)       /*  0–5k ft: dark green  */
#define CLR_ALT_MID lv_color_hex(0x00CCFF)       /*  5–15k ft: cyan       */
#define CLR_ALT_HIGH lv_color_hex(0xFFE600)      /* 15–25k ft: yellow     */
#define CLR_ALT_VHIGH lv_color_hex(0xFF8C00)     /* 25–35k ft: orange     */
#define CLR_ALT_TOP lv_color_hex(0xFF3333)       /* 35k+ ft: red          */

/* ───────────────────────── display constants ────────────────────────── */
#define DISP_W 240
#define DISP_H 320

/* Draw-buffer height (lines). Keep at ≤10 for no-PSRAM safety.           */
#define BUF_LINES 8
static lv_color_t buf1[DISP_W * BUF_LINES];
static lv_color_t buf2[DISP_W * BUF_LINES];

/* ───────────────────── radar canvas dimensions ──────────────────────── */
/* 160×160 px canvas = 51 200 bytes RGB565 — allocated once, reused      */
#define RADAR_SZ 160
#define RADAR_CX (RADAR_SZ / 2) /* 80 */
#define RADAR_CY (RADAR_SZ / 2) /* 80 */

#define g_planes_on_radar_count 30

/* Configurable radar radius (km) — default 50 km */
static float g_radar_radius_km = 50.0f;

/* Runway length is scaled ×3 from true scale for visibility */
#define RUNWAY_SCALE 3.0f

/* Fixed ring radii in canvas pixels (radar canvas is 160×160) */
static const int R1 = 24; /* 1/3 of max radius */
static const int R2 = 48; /* 2/3 of max radius */
static const int R3 = 72; /* Full max radius    */

// time
const char *current_time;
uint32_t last_update_timestamp_ms = 0; /* millis() when flights were last updated */

// radar origin in lat/lon (for future use: plotting multiple flights on radar with correct relative positions)
static float g_radar_origin_lat = 0;
static float g_radar_origin_lon = 0;

/* ─────────────────────── live flight data ───────────────────────────── */
typedef struct
{
    char icao_address[7];
    char flight[10];
    char origin[4];
    char destination[4];
    char aircraft_code[8];
    float latitude;
    float longitude;
    float distance_km;
    int bearing_deg;
    int altitude_ft;
    int vs_fpm; /* vertical speed ft/min */
    int heading_deg;
    int sweep_deg;
    int ground_speed;
    char airline[50];
} flight_data_t;

static flight_data_t g_flight = {
    .icao_address = "----",
    .flight = "----",
    .origin = "---",
    .destination = "---",
    .aircraft_code = "---",
    .airline = "---",
    .latitude = 0,
    .longitude = 0,
    .distance_km = 0,
    .bearing_deg = 0,
    .altitude_ft = 0,
    .vs_fpm = 0,
    .heading_deg = 0,
    .sweep_deg = 0,
    .ground_speed = 0,
};

static latlon_t g_planes_on_radar[g_planes_on_radar_count];
static airport_runway_overlay_t g_airport_runways[6];
static int g_airport_runway_count = 0;

/* ───────────────────── airplane icon on radar ─────────────────────── */
#define PLANE_IMG_W 28
#define PLANE_IMG_H 28
#define PLANE_SCALE (LV_SCALE_NONE / 2) /* render at 1/3 of native size */
/* plane_map[] is LVGL8 interleaved RGB565 + alpha; we convert it once to
   ARGB8888 at init into this static buffer (28*28*4 = 3136 B). */
static uint8_t s_plane_argb[PLANE_IMG_W * PLANE_IMG_H * 4];
static lv_image_dsc_t s_plane_img;

/* ─────────────────────── widget handles ─────────────────────────────── */
static lv_obj_t *g_canvas;
static lv_obj_t *g_lbl_flight;
static lv_obj_t *g_lbl_origin;
static lv_obj_t *g_lbl_destination;
static lv_obj_t *g_lbl_aircraft_code;
static lv_obj_t *g_lbl_airline;
static lv_obj_t *g_lbl_alt;
static lv_obj_t *g_lbl_speed;
static lv_obj_t *g_lbl_heading;
static lv_obj_t *g_lbl_vs;
static lv_obj_t *g_lbl_time;
static lv_obj_t *g_lbl_last_upd;
static lv_obj_t *plane_logo;
static lv_obj_t *airline_logo;

/* canvas pixel buffer — allocated from heap to save BSS space */
static uint8_t *g_canvas_buf = NULL;

/* ═══════════════════════════════════════════════════════════════════════
 *  DOUBLE-BUFFER LOGO MANAGEMENT (prevents corruption when flight changes)
 *  Two fixed buffers rotate: new logo written to "next", swap after render
 * ═══════════════════════════════════════════════════════════════════════*/
#define MAX_LOGO_WIDTH 35
#define MAX_LOGO_HEIGHT 35
#define MAX_LOGO_SIZE (MAX_LOGO_WIDTH * MAX_LOGO_HEIGHT * 4) /* ARGB8888 */

/* Two persistent buffers and descriptors (never freed, never reallocated) */
static uint8_t logo_buffer_pool[2][MAX_LOGO_SIZE];
static lv_image_dsc_t logo_dsc_pool[2];

/* Indices for double-buffering: current=being displayed, next=being filled */
static int logo_current_idx = 0; /* Index displayed on screen */
static int logo_next_idx = 1;    /* Index for next logo decode */

/* State tracking */
static bool logo_swap_pending = false;

static float ui_calc_dist_km(float lat1, float lon1, float lat2, float lon2)
{
    float lat1_r = lat1 * (float)M_PI / 180.0f;
    float lon1_r = lon1 * (float)M_PI / 180.0f;
    float lat2_r = lat2 * (float)M_PI / 180.0f;
    float lon2_r = lon2 * (float)M_PI / 180.0f;
    float dlat = lat2_r - lat1_r;
    float dlon = lon2_r - lon1_r;
    float a = sinf(dlat / 2.0f) * sinf(dlat / 2.0f) + cosf(lat1_r) * cosf(lat2_r) * sinf(dlon / 2.0f) * sinf(dlon / 2.0f);
    return 6371.0f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static int ui_calc_bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    float lat1_r = lat1 * (float)M_PI / 180.0f;
    float lon1_r = lon1 * (float)M_PI / 180.0f;
    float lat2_r = lat2 * (float)M_PI / 180.0f;
    float lon2_r = lon2 * (float)M_PI / 180.0f;
    float dlon = lon2_r - lon1_r;
    float y = sinf(dlon) * cosf(lat2_r);
    float x = cosf(lat1_r) * sinf(lat2_r) - sinf(lat1_r) * cosf(lat2_r) * cosf(dlon);
    return (int)fmodf(atan2f(y, x) * 180.0f / (float)M_PI + 360.0f, 360.0f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Helper: draw filled circle on canvas (software)
 * ═══════════════════════════════════════════════════════════════════════*/
static void canvas_draw_circle(lv_obj_t *canvas, int cx, int cy, int r,
                               lv_color_t color, bool fill)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = color;
    arc.width = fill ? r : 1;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = r;

    lv_draw_arc(&layer, &arc);
    lv_canvas_finish_layer(canvas, &layer);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Helper: decode the embedded airplane icon (RGB565 + alpha) into an
 *  ARGB8888 LVGL image. Call once at UI init.
 * ═══════════════════════════════════════════════════════════════════════*/
static void plane_image_init(void)
{
    for (int i = 0; i < PLANE_IMG_W * PLANE_IMG_H; i++)
    {
        uint16_t rgb565 = (uint16_t)((plane_map[i * 3 + 1] << 8) | plane_map[i * 3]);
        uint8_t r5 = (rgb565 >> 11) & 0x1F;
        uint8_t g6 = (rgb565 >> 5) & 0x3F;
        uint8_t b5 = rgb565 & 0x1F;

        /* ARGB8888 stored in memory as B, G, R, A (little-endian) */
        s_plane_argb[i * 4 + 0] = (uint8_t)((b5 * 255) / 31);
        s_plane_argb[i * 4 + 1] = (uint8_t)((g6 * 255) / 63);
        s_plane_argb[i * 4 + 2] = (uint8_t)((r5 * 255) / 31);
        s_plane_argb[i * 4 + 3] = plane_map[i * 3 + 2];
    }

    s_plane_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_plane_img.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_plane_img.header.w = PLANE_IMG_W;
    s_plane_img.header.h = PLANE_IMG_H;
    s_plane_img.header.stride = PLANE_IMG_W * 4;
    s_plane_img.data_size = sizeof(s_plane_argb);
    s_plane_img.data = s_plane_argb;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Helper: draw the airplane icon rotated to the compass heading.
 *  Base icon points North (up); LVGL positive rotation is clockwise.
 * ═══════════════════════════════════════════════════════════════════════*/
static void canvas_draw_plane(lv_obj_t *canvas, int cx, int cy,
                              int heading_deg, lv_color_t color)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &s_plane_img;
    dsc.rotation = heading_deg * 10; /* 0.1 deg units */
    dsc.scale_x = PLANE_SCALE;
    dsc.scale_y = PLANE_SCALE;
    dsc.pivot.x = PLANE_IMG_W / 2;
    dsc.pivot.y = PLANE_IMG_H / 2;
    dsc.recolor = color;
    dsc.recolor_opa = LV_OPA_COVER;
    dsc.antialias = 1;

    lv_area_t coords;
    coords.x1 = cx - PLANE_IMG_W / 2;
    coords.y1 = cy - PLANE_IMG_H / 2;
    coords.x2 = coords.x1 + PLANE_IMG_W - 1;
    coords.y2 = coords.y1 + PLANE_IMG_H - 1;

    lv_draw_image(&layer, &dsc, &coords);
    lv_canvas_finish_layer(canvas, &layer);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Helper: draw a line on canvas
 * ═══════════════════════════════════════════════════════════════════════*/
static void canvas_draw_line_seg(lv_obj_t *canvas,
                                 int x0, int y0, int x1, int y1,
                                 lv_color_t color, int width)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.p1.x = x0;
    dsc.p1.y = y0;
    dsc.p2.x = x1;
    dsc.p2.y = y1;

    lv_draw_line(&layer, &dsc);
    lv_canvas_finish_layer(canvas, &layer);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Helper: draw text on canvas
 * ═══════════════════════════════════════════════════════════════════════*/
static void canvas_draw_text(lv_obj_t *canvas, int x, int y,
                             const char *text, const lv_font_t *font,
                             lv_color_t color)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.text = text;
    dsc.opa = LV_OPA_COVER;
    dsc.align = LV_TEXT_ALIGN_LEFT;

    lv_area_t coords = { x, y, x + 200, y + 40 };
    lv_draw_label(&layer, &dsc, &coords);
    lv_canvas_finish_layer(canvas, &layer);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Radar canvas: full redraw each sweep tick
 *  Called from timer; cheap because canvas is 160×160, not full screen.
 * ═══════════════════════════════════════════════════════════════════════*/
static lv_color_t altitude_to_color(int altitude_ft)
{
    if (altitude_ft < 5000)
        return CLR_ALT_LOW;
    if (altitude_ft < 15000)
        return CLR_ALT_MID;
    if (altitude_ft < 25000)
        return CLR_ALT_HIGH;
    if (altitude_ft < 35000)
        return CLR_ALT_VHIGH;
    return CLR_ALT_TOP;
}

static bool tracked_plane_blink_bright(void)
{
    /* slow blink: alternate dim ↔ bright every 500 ms */
    return ((millis() / 500) & 1) == 0;
}

static void radar_redraw(lv_obj_t *canvas, int sweep_deg)
{
    /* 1. Fill background */
    lv_canvas_fill_bg(canvas, CLR_BG, LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /* ── sweep wedge (gradient-like: draw multiple arcs with decreasing opa) */
    /* Approximate a swept sector with a filled arc going backward 60 deg    */
    for (int fade = 0; fade < 30; fade += 5)
    {
        lv_draw_arc_dsc_t sw;
        lv_draw_arc_dsc_init(&sw);
        sw.color = CLR_GREEN;
        sw.width = R3;
        sw.center.x = RADAR_CX;
        sw.center.y = RADAR_CY;
        sw.radius = R3;
        /* LVGL arc: 0° = 3-o'clock, grows CW                               */
        /* heading 0°=North: offset by -90 then add sweep                   */
        int base = sweep_deg - 90;
        sw.start_angle = (uint16_t)((base - fade + 360) % 360);
        sw.end_angle = (uint16_t)((base - fade + 5 + 360) % 360);
        sw.opa = (lv_opa_t)(LV_OPA_50 - fade * 4);
        lv_draw_arc(&layer, &sw);
    }

    lv_canvas_finish_layer(canvas, &layer);

    /* ── ring circles ── */
    canvas_draw_circle(canvas, RADAR_CX, RADAR_CY, R1, CLR_GREEN_DIM, false);
    canvas_draw_circle(canvas, RADAR_CX, RADAR_CY, R2, CLR_GREEN_DIM, false);
    canvas_draw_circle(canvas, RADAR_CX, RADAR_CY, R3, CLR_GREEN, false);

    /* ── cross-hair center dot ── */
    canvas_draw_circle(canvas, RADAR_CX, RADAR_CY, 1, CLR_GREEN, true);

    for (int i = 0; i < g_planes_on_radar_count; i++)
    {
        latlon_t p = g_planes_on_radar[i];
        if (p.icao_address[0] == '\0')
            continue; // skip empty slots (check ICAO address as identifier)

        // Use precalculated distance and bearing from latlon_t struct
        int bearing = p.bearing_deg;
        float distance = p.distance_km;

        /* draw flight in radar — cap distance to radar_radius_km for on-screen display */
        if (distance < 0.1f)
            distance = 0.1f; /* Avoid clustering at center */
        if (distance > g_radar_radius_km)
            continue; /* Skip planes beyond radar range */

        float norm = distance / g_radar_radius_km; /* 0..1 within outer ring       */
        float rad = (float)(bearing - 90) * (float)M_PI / 180.0f;
        int ax = (int)(RADAR_CX + norm * R3 * cosf(rad));
        int ay = (int)(RADAR_CY + norm * R3 * sinf(rad));

        lv_color_t plane_color = altitude_to_color(p.altitude_ft);
        if (strcmp(g_flight.icao_address, p.icao_address) == 0)
        {
            plane_color = tracked_plane_blink_bright() ? lv_color_lighten(plane_color, LV_OPA_50)
                                                       : lv_color_darken(plane_color, LV_OPA_50);
        }
        canvas_draw_plane(canvas, ax, ay, p.heading_deg, plane_color);
    }

    for (int i = 0; i < g_airport_runway_count; i++)
    {
        const airport_runway_overlay_t *airport = &g_airport_runways[i];
        float distance = ui_calc_dist_km(g_radar_origin_lat, g_radar_origin_lon, airport->latitude, airport->longitude);
        if (distance > g_radar_radius_km)
        {
            continue;
        }

        float norm = distance / g_radar_radius_km;
        int bearing = ui_calc_bearing_deg(g_radar_origin_lat, g_radar_origin_lon, airport->latitude, airport->longitude);
        float rad = (float)(bearing - 90) * (float)M_PI / 180.0f;
        int ax = (int)(RADAR_CX + norm * R3 * cosf(rad));
        int ay = (int)(RADAR_CY + norm * R3 * sinf(rad));

        if (airport->runway_count <= 0)
        {
            canvas_draw_circle(canvas, ax, ay, 1, CLR_GRAY, true);
            continue;
        }

        /* Map canvas scale: R3 px == g_radar_radius_km */
        float px_per_km = (float)R3 / g_radar_radius_km;
        for (int j = 0; j < airport->runway_count; j++)
        {
            /* Full runway length (ft → km → px), scaled ×3 for visibility */
            float runway_len_px = airport->runways[j].length_ft * 0.0003048f * px_per_km * RUNWAY_SCALE;
            if (runway_len_px < 4.0f)
            {
                runway_len_px = 4.0f; /* minimum so short runways stay visible */
            }

            /* Compass heading → canvas angle; draw centered on the airport so
               both runway ends are shown with the correct orientation. */
            float runway_rad = (float)(airport->runways[j].heading_deg - 90) * (float)M_PI / 180.0f;
            float dx = (runway_len_px / 2.0f) * cosf(runway_rad);
            float dy = (runway_len_px / 2.0f) * sinf(runway_rad);
            int x0 = (int)(ax - dx);
            int y0 = (int)(ay - dy);
            int x1 = (int)(ax + dx);
            int y1 = (int)(ay + dy);
            canvas_draw_line_seg(canvas, x0, y0, x1, y1, CLR_GRAY, 1);
        }

        /* Airport code next to the runway */
        canvas_draw_text(canvas, ax + 3, ay - 14, airport->icao, &lv_font_montserrat_10, CLR_GRAY);
    }
}

static void display_airline_logo(const lv_img_dsc_t *logo_dsc)
{
    if (logo_dsc == NULL)
    {
        lv_obj_remove_flag(plane_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(airline_logo, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(plane_logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(airline_logo, LV_OBJ_FLAG_HIDDEN);

    lv_image_set_src(airline_logo, logo_dsc);
    lv_obj_set_size(airline_logo, logo_dsc->header.w, logo_dsc->header.h);
    lv_obj_update_layout(airline_logo);
    lv_obj_invalidate(airline_logo);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DOUBLE-BUFFER LOGO FUNCTIONS
 *  get_next_logo_buffer() — returns pointer to buffer for next decode
 *  commit_logo_buffer() — swaps buffer after decode complete
 * ═══════════════════════════════════════════════════════════════════════*/

/**
 * @brief Get pointer to the "next" buffer for decoding a new logo.
 *        This buffer is NOT currently displayed (safe to write to).
 * @return Pointer to pixel data buffer, max MAX_LOGO_SIZE bytes
 */
uint8_t *get_next_logo_buffer(void)
{
    return logo_buffer_pool[logo_next_idx];
}

/**
 * @brief Get the descriptor for the "next" buffer (to fill header info).
 * @return Pointer to lv_image_dsc_t for next buffer
 */
lv_image_dsc_t *get_next_logo_descriptor(void)
{
    return &logo_dsc_pool[logo_next_idx];
}

/**
 * @brief Finalize and swap logo buffers after decode is complete.
 *        Displays the newly decoded logo immediately.
 * @param width   Logo width in pixels
 * @param height  Logo height in pixels
 *
 * Safe to call from any task (cross-core). Logo descriptor data is persistent,
 * so LVGL will never reference freed memory.
 */
void commit_logo_buffer(uint32_t width, uint32_t height)
{

    // indiacate issue with logo (e.g. decode failure or size too large) by sending empty logo (w=0 or h=0)
    if (width == 0 || height == 0)
    {
        hide_logo();
        return;
    }
    /* Fill descriptor for next buffer */
    lv_image_dsc_t *next_dsc = &logo_dsc_pool[logo_next_idx];
    next_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    next_dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    next_dsc->header.w = width;
    next_dsc->header.h = height;
    next_dsc->data_size = width * height * 4;
    next_dsc->data = logo_buffer_pool[logo_next_idx]; /* Point to buffer data */

    /* Swap: "next" becomes "current", "current" becomes "next" */
    logo_current_idx = logo_next_idx;
    logo_next_idx = (logo_current_idx + 1) % 2;

    /* Update display with new logo */
    display_airline_logo(&logo_dsc_pool[logo_current_idx]);

    esp_log_write(ESP_LOG_INFO, "flight_tracker_ui",
                  "Logo swapped: now %ux%u, current_idx=%d", width, height, logo_current_idx);
}

/**
 * @brief Hide logo (show plane icon instead)
 */
void hide_logo(void)
{
    display_airline_logo(NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Utility: create a styled label
 * ═══════════════════════════════════════════════════════════════════════*/
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, lv_color_t color,
                            lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

// Returns pointer to direction string (8-point compass)
const char *heading_to_direction(float heading)
{
    // Normalize heading to 0-360 range
    while (heading < 0)
        heading += 360;
    while (heading >= 360)
        heading -= 360;

    // Add 22.5 degrees offset so ranges are centered on directions
    heading = fmod(heading + 22.5, 360);

    if (heading < 45)
        return "N";
    if (heading < 90)
        return "NE";
    if (heading < 135)
        return "E";
    if (heading < 180)
        return "SE";
    if (heading < 225)
        return "S";
    if (heading < 270)
        return "SW";
    if (heading < 315)
        return "W";
    return "NW";
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Timer callback — updates sweep + live labels (1 Hz for labels, 50ms
 *  for sweep to get ~20fps radar animation without overloading ESP32)
 * ═══════════════════════════════════════════════════════════════════════*/
static uint32_t g_tick_label = 0;

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* advance sweep */
    g_flight.sweep_deg = (g_flight.sweep_deg + 6) % 360;
    radar_redraw(g_canvas, g_flight.sweep_deg);

    /* update labels at ~1 Hz (every 20 × 50ms ticks) */
    g_tick_label++;
    if (g_tick_label % 20 == 0)
    {
        char buf[32];

        lv_label_set_text(g_lbl_flight, g_flight.flight);
        lv_label_set_text(g_lbl_aircraft_code, g_flight.aircraft_code);
        lv_label_set_text(g_lbl_origin, g_flight.origin);
        lv_label_set_text(g_lbl_destination, g_flight.destination);
        lv_label_set_text(g_lbl_airline, g_flight.airline);

        snprintf(buf, sizeof(buf), "%d ft", g_flight.altitude_ft);
        lv_label_set_text(g_lbl_alt, buf);

        snprintf(buf, sizeof(buf), "%d kts", g_flight.ground_speed);
        lv_label_set_text(g_lbl_speed, buf);

        snprintf(buf, sizeof(buf), "%d\xc2\xb0 %s", g_flight.heading_deg, heading_to_direction(g_flight.heading_deg));
        lv_label_set_text(g_lbl_heading, buf);

        snprintf(buf, sizeof(buf), "%d ft/min", g_flight.vs_fpm);
        lv_label_set_text(g_lbl_vs, buf);

        /* Calculate elapsed time from actual timestamp (accurate to flight fetch frequency) */
        uint32_t now_ms = millis();
        uint32_t elapsed_ms = now_ms - last_update_timestamp_ms;
        uint32_t elapsed_secs = (elapsed_ms + 500) / 1000; /* Round to nearest second */
        snprintf(buf, sizeof(buf), "last update: %lus ago", (unsigned long)elapsed_secs);
        lv_label_set_text(g_lbl_last_upd, buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Thin horizontal divider helper
 * ═══════════════════════════════════════════════════════════════════════*/
static void add_divider(lv_obj_t *parent, int y)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, DISP_W - 8, 1);
    lv_obj_set_style_bg_color(div, CLR_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, y);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Stat cell: icon-label + value, placed in a 120×40 cell
 * ═══════════════════════════════════════════════════════════════════════*/
static lv_obj_t *make_stat_cell_with_icon(lv_obj_t *parent,
                                          const char *icon,
                                          const char *cap,
                                          const char *init_val,
                                          lv_color_t val_color,
                                          lv_align_t align, int x, int y)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, 115, 30);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 2, 0);
    lv_obj_align(cell, align, x, y);

    /* Icon */
    lv_obj_t *ic = lv_label_create(cell);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &material_icons, 0);
    lv_obj_set_style_text_color(ic, CLR_CYAN, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);

    /* caption */
    make_label(cell, cap, &lv_font_montserrat_10, CLR_GRAY,
               LV_ALIGN_TOP_LEFT, 30, -2);

    /* value */
    lv_obj_t *val = make_label(cell, init_val, &lv_font_montserrat_14, val_color,
                               LV_ALIGN_BOTTOM_LEFT, 30, 0);
    return val;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PUBLIC: initialise display driver + build full UI
 *
 *  Call sequence from your app_main / setup():
 *    lv_init();
 *    // ... register your ILI9341 flush callback here ...
 *    flight_tracker_ui_init();
 *    // then in loop: lv_timer_handler(); + 5ms delay
 * ═══════════════════════════════════════════════════════════════════════*/
void flight_tracker_ui_init(void)
{ /* ── allocate canvas buffer from heap ────────────────────────── */
    plane_image_init();

    size_t canvas_buf_size = LV_CANVAS_BUF_SIZE(RADAR_SZ, RADAR_SZ, 16, 1);
    g_canvas_buf = (uint8_t *)malloc(canvas_buf_size);
    if (!g_canvas_buf)
    {
        return; /* allocation failed, abort UI init */
    }
    lv_display_t *disp = lv_display_get_default();
    lv_display_set_buffers(disp, buf1, buf2,
                           sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ── root screen ──────────────────────────────────────────────── */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ══════════════════════ HEADER ROW (y: 0–30) ══════════════════ */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, DISP_W, 24);
    lv_obj_set_style_bg_color(hdr, CLR_PANEL, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    make_label(hdr, "FLIGHT TRACKER", &lv_font_montserrat_10, CLR_GRAY,
               LV_ALIGN_LEFT_MID, 6, 0);

    /* live clock — updated via timer */
    g_lbl_time = make_label(hdr, "12:45:32", &lv_font_montserrat_10, CLR_GRAY,
                            LV_ALIGN_RIGHT_MID, -22, 0);

    /* wifi icon (unicode approx) */
    make_label(hdr, LV_SYMBOL_WIFI, &lv_font_montserrat_12, CLR_GREEN,
               LV_ALIGN_RIGHT_MID, -4, 0);

    add_divider(scr, 24);

    /* ══════════════════════ flight ROW (y: 31–80) ═══════════════ */

    // plane Icon when logo is not avilable
    plane_logo = make_label(lv_screen_active(), "\ue6ca", &material_icons, CLR_GREEN,
                            LV_ALIGN_TOP_LEFT, 6, 36);
    // logo holder
    airline_logo = lv_image_create(lv_screen_active());
    lv_obj_align(airline_logo, LV_ALIGN_TOP_LEFT, 2, 30);
    lv_obj_add_flag(airline_logo, LV_OBJ_FLAG_HIDDEN);

    g_lbl_flight = make_label(scr, g_flight.flight, &lv_font_montserrat_28, CLR_WHITE,
                              LV_ALIGN_TOP_LEFT, 40, 30);

    // aircraft code
    g_lbl_aircraft_code = make_label(scr, g_flight.aircraft_code, &lv_font_montserrat_12, CLR_WHITE,
                                     LV_ALIGN_TOP_LEFT, 40, 58);

    // origin
    g_lbl_origin = make_label(scr, g_flight.origin, &lv_font_montserrat_12, CLR_WHITE,
                              LV_ALIGN_TOP_LEFT, 40, 70);

    // arrow
    make_label(scr, "\ue941", &material_icons, CLR_WHITE,
               LV_ALIGN_TOP_LEFT, 70, 66);

    // destination
    g_lbl_destination = make_label(scr, g_flight.destination, &lv_font_montserrat_12, CLR_WHITE,
                                   LV_ALIGN_TOP_LEFT, 100, 70);

    make_label(scr, "AIRLINE", &lv_font_montserrat_10, CLR_GRAY,
               LV_ALIGN_TOP_RIGHT, -4, 34);

    g_lbl_airline = make_label(scr, g_flight.airline, &lv_font_montserrat_14, CLR_GREEN,
                               LV_ALIGN_TOP_RIGHT, -4, 50);
    lv_obj_set_content_width(g_lbl_airline, 100);
    lv_label_set_long_mode(g_lbl_airline, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_lbl_airline, LV_TEXT_ALIGN_RIGHT, 0);

    add_divider(scr, 88);

    /* ══════════════════════ RADAR AREA (y: 89–250) ════════════════ */
    /* Radar container — clips children, centers canvas */
    lv_obj_t *radar_cont = lv_obj_create(scr);
    lv_obj_set_size(radar_cont, DISP_W, 162);
    lv_obj_set_style_bg_color(radar_cont, CLR_BG, 0);
    lv_obj_set_style_bg_opa(radar_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(radar_cont, 0, 0);
    lv_obj_set_style_pad_all(radar_cont, 0, 0);
    lv_obj_align(radar_cont, LV_ALIGN_TOP_MID, 0, 89);
    lv_obj_clear_flag(radar_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Canvas — centred in radar_cont */
    g_canvas = lv_canvas_create(radar_cont);
    lv_canvas_set_buffer(g_canvas, g_canvas_buf, RADAR_SZ, RADAR_SZ,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(g_canvas, LV_ALIGN_CENTER, 0, 0);

    /* Cardinal direction labels (outside canvas, on radar_cont) */
    make_label(radar_cont, "N", &lv_font_montserrat_12, CLR_GREEN,
               LV_ALIGN_TOP_MID, 0, 2);
    make_label(radar_cont, "S", &lv_font_montserrat_12, CLR_GREEN,
               LV_ALIGN_BOTTOM_MID, 0, -2);
    make_label(radar_cont, "W", &lv_font_montserrat_12, CLR_GREEN,
               LV_ALIGN_LEFT_MID, 40, 0);
    make_label(radar_cont, "E", &lv_font_montserrat_12, CLR_GREEN,
               LV_ALIGN_RIGHT_MID, -40, 0);

    /* outer ring distance label — placed to the right of the ring */
    char label_r3[16];
    snprintf(label_r3, sizeof(label_r3), "%.0f km", g_radar_radius_km);

    make_label(radar_cont, label_r3, &lv_font_montserrat_10, CLR_GREEN,
               LV_ALIGN_RIGHT_MID, -4, 0);

    /* Inner ring distance labels (commented out — only outer label shown) */
    // char label_r1[16], label_r2[16];
    // snprintf(label_r1, sizeof(label_r1), "%.0f km", g_radar_radius_km * 1.0f / 3.0f);
    // snprintf(label_r2, sizeof(label_r2), "%.0f km", g_radar_radius_km * 2.0f / 3.0f);
    // make_label(radar_cont, label_r1, &lv_font_montserrat_10, CLR_GREEN_DIM,
    //            LV_ALIGN_CENTER, 2, -R1 - 2);
    // make_label(radar_cont, label_r2, &lv_font_montserrat_10, CLR_GREEN_DIM,
    //            LV_ALIGN_CENTER, 2, -R2 - 2);

    /* Initial radar render */
    radar_redraw(g_canvas, 0);

    add_divider(scr, 251);

    /* ══════════════════════ STATS GRID (y: 252–310) ═══════════════ */
    /* 2×2 grid: altitude | speed / heading | vertical-rate           */

    /* vertical divider */
    lv_obj_t *vdiv = lv_obj_create(scr);
    lv_obj_set_size(vdiv, 1, 58);
    lv_obj_set_style_bg_color(vdiv, CLR_BORDER, 0);
    lv_obj_set_style_bg_opa(vdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vdiv, 0, 0);
    lv_obj_set_style_pad_all(vdiv, 0, 0);
    lv_obj_align(vdiv, LV_ALIGN_TOP_MID, 0, 252);

    /* horizontal divider within stats */
    add_divider(scr, 281);

    /* ALTITUDE */
    g_lbl_alt = make_stat_cell_with_icon(scr, "\uf873", /* altitude icon */
                                         "ALTITUDE", "-- ft",
                                         CLR_GREEN, LV_ALIGN_TOP_LEFT, 4, 253);
    /* vs indicator appended to alt label — keep simple */
    // make_label(scr, "\xe2\x86\x91 200 ft/min", &lv_font_montserrat_10, CLR_GREEN,
    //            LV_ALIGN_TOP_LEFT, 60, 272);

    /* SPEED */
    g_lbl_speed = make_stat_cell_with_icon(scr, "\ue9e4", "SPEED", "-- km/h",
                                           CLR_WHITE, LV_ALIGN_TOP_RIGHT, -4, 253);

    /* HEADING */
    g_lbl_heading = make_stat_cell_with_icon(scr, "\ue87a", "HEADING", "--\xc2\xb0 SW",
                                             CLR_WHITE, LV_ALIGN_TOP_LEFT, 4, 282);

    /* VERTICAL RATE */
    g_lbl_vs = make_stat_cell_with_icon(scr, "\ueee3", "VERTICAL RATE", "-- ft/min",
                                        CLR_GREEN, LV_ALIGN_TOP_RIGHT, -4, 282);

    /* ══════════════════════ FOOTER (y: 311–319) ═══════════════════ */
    add_divider(scr, 310);

    lv_obj_t *ftr = lv_obj_create(scr);
    lv_obj_set_size(ftr, DISP_W, 10);
    lv_obj_set_style_bg_opa(ftr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ftr, 0, 0);
    lv_obj_set_style_pad_all(ftr, 0, 0);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);

    make_label(ftr, LV_SYMBOL_GPS " flightradar24", &lv_font_montserrat_10, CLR_GREEN,
               LV_ALIGN_LEFT_MID, 4, 0);

    g_lbl_last_upd = make_label(ftr, "last update: 0s ago",
                                &lv_font_montserrat_10, CLR_GRAY,
                                LV_ALIGN_RIGHT_MID, -4, 0);

    // display logo
    display_airline_logo(NULL);

    /* ══════════════════════ ANIMATION TIMER ═══════════════════════ */
    /* 50 ms → ~20 fps sweep; label update every 20 ticks (~1 s)     */
    lv_timer_create(ui_timer_cb, 50, NULL);
}

void setRadarOrigin(float latitude, float longitude)
{
    g_radar_origin_lat = latitude;
    g_radar_origin_lon = longitude;
}

void setRadarRadius(float radius_km)
{
    if (radius_km < 10.0f)
        radius_km = 10.0f; /* Minimum 10 km */
    if (radius_km > 250.0f)
        radius_km = 250.0f; /* Maximum 250 km */
    g_radar_radius_km = radius_km;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  API: inject real ADS-B data from your decoder (call from main loop)
 * ═══════════════════════════════════════════════════════════════════════*/
void flight_tracker_update(const char *icao_address, const char *flight, const char *aircraft_code, const char *origin, const char *destination, float latitude, float longitude, float distance_km, int bearing_deg, int altitude_ft,
                           int vertical_speed, int ground_speed, int heading_deg, const char *airline)
{
    // CRITICAL: Copy strings into buffers to avoid dangling pointers
    // The input pointers may point to temporary data that will be freed
    strncpy(g_flight.icao_address, icao_address ? icao_address : "----", sizeof(g_flight.icao_address) - 1);
    g_flight.icao_address[sizeof(g_flight.icao_address) - 1] = '\0';

    strncpy(g_flight.flight, flight ? flight : "----", sizeof(g_flight.flight) - 1);
    g_flight.flight[sizeof(g_flight.flight) - 1] = '\0';

    strncpy(g_flight.aircraft_code, aircraft_code ? aircraft_code : "---", sizeof(g_flight.aircraft_code) - 1);
    g_flight.aircraft_code[sizeof(g_flight.aircraft_code) - 1] = '\0';

    strncpy(g_flight.origin, origin ? origin : "---", sizeof(g_flight.origin) - 1);
    g_flight.origin[sizeof(g_flight.origin) - 1] = '\0';

    strncpy(g_flight.destination, destination ? destination : "---", sizeof(g_flight.destination) - 1);
    g_flight.destination[sizeof(g_flight.destination) - 1] = '\0';

    strncpy(g_flight.airline, airline ? airline : "---", sizeof(g_flight.airline) - 1);
    g_flight.airline[sizeof(g_flight.airline) - 1] = '\0';

    g_flight.latitude = latitude;
    g_flight.longitude = longitude;
    g_flight.distance_km = distance_km;
    g_flight.bearing_deg = bearing_deg;
    g_flight.altitude_ft = altitude_ft;
    g_flight.vs_fpm = vertical_speed;
    g_flight.ground_speed = ground_speed;
    g_flight.heading_deg = heading_deg;
    last_update_timestamp_ms = millis(); /* Capture actual time of update */
}

void update_planes_on_radar(latlon_t *planes, int count)
{
    memset(g_planes_on_radar, 0, sizeof(g_planes_on_radar));
    for (int i = 0; i < count && i < g_planes_on_radar_count; i++)
    {
        g_planes_on_radar[i] = planes[i];
    }
}

void update_airport_runways(const airport_runway_overlay_t *airports, int count)
{
    memset(g_airport_runways, 0, sizeof(g_airport_runways));
    g_airport_runway_count = 0;
    for (int i = 0; i < count && i < 6; i++)
    {
        g_airport_runways[i] = airports[i];
        g_airport_runway_count++;
    }
}

void update_time(const char *time_str)
{
    current_time = time_str;
    lv_label_set_text(g_lbl_time, current_time);
}

void update_logo(const lv_image_dsc_t *logo_dsc)
{
    display_airline_logo(logo_dsc);
}