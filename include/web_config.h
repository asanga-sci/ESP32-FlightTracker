#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct APInfo {
    char ssid[64];
    char password[64];
    char ip[20];
};

// ─── Config structure ─────────────────────────────────────────────────────────

struct FlightConfig {
    // WiFi
    char ssid[64];
    char password[64];

    // Radar / OpenSky bounding box
    float lat;
    float lon;
    float radiusKm;

    // logo support    
    bool logoSupport;
    char logostreamApiKey[50];

    // AirLabs fallback for route/airline data (optional)
    bool airlabsFallback;
    char airlabsApiKey[50];

    // Poll interval
    uint16_t refreshSec;   // ≥ 10
};

// ─── Events published on the config event group ───────────────────────────────
// Other tasks can xEventGroupWaitBits() on these.

#define CFG_EVT_READY   (1 << 0)   // config loaded, WiFi connected
#define CFG_EVT_CHANGED (1 << 1)   // user saved new settings (device will reboot)

typedef void (*WebConfigAPReadyCb)(const char* ssid,
                                   const char* password,
                                   const char* ip);

// ─── Public API ───────────────────────────────────────────────────────────────

/**
 * Initialise the config subsystem and spawn the web-config FreeRTOS task.
 *
 * The task:
 *   - Loads config from NVS.
 *   - If no valid config → starts AP + captive-portal web server and waits.
 *   - Once configured → connects to WiFi, then serves /settings on port 80.
 *   - Sets CFG_EVT_READY on the returned event group once WiFi is up.
 *
 * Call once from app_main() / setup() before starting other tasks that need
 * the network.
 *
 * @param coreID  CPU core to pin the task to (0 or 1).  Use 1 to leave
 *                core 0 free for the Arduino/BT stack.
 * @return        FreeRTOS event group handle (never NULL).
 */
EventGroupHandle_t webConfigInit(BaseType_t coreID = 1, WebConfigAPReadyCb apReadyCb = nullptr);

/**
 * Thread-safe accessor — returns a snapshot of the current config.
 * Safe to call from any task after CFG_EVT_READY is set.
 */
FlightConfig webConfigGet();

/**
 * Erase NVS and reboot into AP setup mode.
 * Safe to call from any task or ISR (schedules reboot via timer).
 */
void webConfigReset();