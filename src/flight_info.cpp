#include "flight_info.h"

#include "web_config.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <esp32-hal-log.h>
#include <math.h>
#include <vector>

namespace
{
/**
 * @brief Simple HTTPS GET helper (cert validation disabled — matches
 *        the existing airport_info.cpp pattern).
 */
bool https_get(const String &url, String &response, String &error_message)
{
    WiFiClientSecure wifi_client;
    wifi_client.setInsecure();

    HTTPClient client;
    log_i("Request: %s", url.c_str());
    if (!client.begin(wifi_client, url))
    {
        error_message = "Failed to start HTTPS client";
        log_e("%s", error_message.c_str());
        return false;
    }

    const int http_code = client.GET();
    if (http_code != HTTP_CODE_OK)
    {
        client.end();
        log_e("HTTPS error code: %d for %s", http_code, url.c_str());
        error_message = "HTTP request failed (" + String(http_code) + ")";
        return false;
    }

    response = client.getString();
    client.end();
    return true;
}

float dist_km(float lat1, float lon1, float lat2, float lon2)
{
    const float lat1_r = lat1 * (float)M_PI / 180.0f;
    const float lat2_r = lat2 * (float)M_PI / 180.0f;
    const float dlat = (lat2 - lat1) * (float)M_PI / 180.0f;
    const float dlon = (lon2 - lon1) * (float)M_PI / 180.0f;
    const float a = sinf(dlat / 2.0f) * sinf(dlat / 2.0f) +
                    cosf(lat1_r) * cosf(lat2_r) * sinf(dlon / 2.0f) * sinf(dlon / 2.0f);
    return 6371.0f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

/**
 * @brief Enrich one flight with route + airline data from adsbdb.com.
 * @return true if the route was resolved.
 */
bool enrich_flight_from_adsbdb(flight_info &flight)
{
    if (flight.call_sign.isEmpty())
    {
        log_w("adsbdb: no callsign to look up");
        return false;
    }

    const String url = "https://api.adsbdb.com/v0/callsign/" + flight.call_sign;

    String response;
    String error_message;
    if (!https_get(url, response, error_message))
    {
        log_e("adsbdb: %s", error_message.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError parse_error = deserializeJson(doc, response);
    if (parse_error != DeserializationError::Ok)
    {
        log_e("adsbdb: parse error: %s", parse_error.c_str());
        return false;
    }

    const JsonObject route = doc["response"]["flightroute"];
    if (route.isNull())
    {
        log_w("adsbdb: no route data for %s", flight.call_sign.c_str());
        return false;
    }

    flight.icao_airline = route["airline"]["icao"] | "";
    flight.airline_name = route["airline"]["name"] | "";

    // Municipality is the readable city name — fall back to airport name/code
    flight.origin_airport = route["origin"]["municipality"] | "";
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route["origin"]["name"] | "";
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route["origin"]["iata_code"] | "";

    flight.destination_airport = route["destination"]["municipality"] | "";
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route["destination"]["name"] | "";
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route["destination"]["iata_code"] | "";

    log_i("adsbdb: %s: %s -> %s (airline %s)",
          flight.call_sign.c_str(),
          flight.origin_airport.c_str(),
          flight.destination_airport.c_str(),
          flight.airline_name.c_str());
    return true;
}
} // namespace

bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude, bool air, bool ground, bool gliders, bool vehicles, std::vector<flight_info> &flights, String &error_message)
{
    (void)range_longitude;
    (void)air;
    (void)ground;
    (void)gliders;
    (void)vehicles;

    int radius_km = (int)(range_latitude + 0.5f);
    if (radius_km < 1)
    {
        radius_km = 1;
    }

    flights.clear();
    error_message = "";

    // ── 1. airplanes.live: all aircraft within a circle ────────────────
    const String url = "https://api.airplanes.live/v2/point/" + String(latitude, 5) +
                       "/" + String(longitude, 5) + "/" + String(radius_km);

    /* TLS handshake + payload download needs ~40 KB of contiguous heap.
       Bail out instead of OOM-aborting the task. */
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free_heap < 40 * 1024 || largest < 24 * 1024)
    {
        error_message = "Insufficient heap for TLS fetch";
        log_e("get_flights: %s (free=%u, largest=%u)", error_message.c_str(),
              (unsigned)free_heap, (unsigned)largest);
        return false;
    }

    String response;
    if (!https_get(url, response, error_message))
    {
        return false;
    }

    JsonDocument doc;
    const DeserializationError parse_error = deserializeJson(doc, response);
    response = ""; /* free the raw payload before adsbdb enrichment */
    if (parse_error != DeserializationError::Ok)
    {
        log_e("airplanes.live: parse error: %s", parse_error.c_str());
        error_message = parse_error.c_str();
        return false;
    }

    const JsonArray aircraft = doc["ac"];
    if (aircraft.isNull())
    {
        error_message = "airplanes.live: unexpected payload";
        log_e("%s", error_message.c_str());
        return false;
    }

    const time_t now_secs = time(nullptr);
    for (const JsonObject obj : aircraft)
    {
        flight_info flight;

        flight.icao_address = obj["hex"] | "";
        flight.latitude = obj["lat"] | 0.0f;
        flight.longitude = obj["lon"] | 0.0f;
        flight.heading = (int)(obj["track"] | 0.0f);
        flight.ground_speed = (int)(obj["gs"] | 0.0f);
        flight.squawk = obj["squawk"] | "";
        flight.aircraft_code = obj["t"] | "";
        flight.registration = obj["r"] | "";

        // alt_baro is barometric altitude in feet, or the string "ground"
        const JsonVariant alt_baro = obj["alt_baro"];
        if (alt_baro.is<const char *>())
        {
            flight.on_ground = true;
        }
        else
        {
            flight.altitude = alt_baro.as<int>();
        }

        flight.vertical_speed = (int)(obj["baro_rate"] | 0.0f);
        const int seen_secs = (int)(obj["seen"] | 0);
        flight.timestamp = now_secs - seen_secs;

        String callsign = obj["flight"] | "";
        callsign.trim();
        flight.flight = callsign;
        flight.call_sign = callsign;

        flights.push_back(flight);
    }

    log_i("airplanes.live: %u aircraft in radius %d km (Heap=%u)", flights.size(), radius_km, ESP.getFreeHeap());

    if (flights.empty())
    {
        error_message = "No aircraft found in range";
        log_w("%s", error_message.c_str());
        return false;
    }

    // ── 2. Sort by distance to the centre (closest first) ──────────────
    std::sort(flights.begin(), flights.end(), [latitude, longitude](const flight_info &a, const flight_info &b)
              {
                  const float da = dist_km(latitude, longitude, a.latitude, a.longitude);
                  const float db = dist_km(latitude, longitude, b.latitude, b.longitude);
                  return da < db;
              });

    // ── 3. adsbdb: resolve route/airline data for the closest aircraft ─
    enrich_flight_from_adsbdb(flights.front());

    log_i(
        "get_flights done: size=%u Heap=%u Largest=%u",
        flights.size(),
        ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
   PNG CACHE (stores compressed PNG data from API, reduces network calls)
   ═══════════════════════════════════════════════════════════════════════*/
#define PNG_CACHE_MAX_ENTRIES 10
#define PNG_CACHE_MAX_SIZE 3000  /* Max PNG size in bytes (~1.5-2.5KB typical) */

struct PngCacheEntry {
    char airline[4];
    uint8_t *png_data;     /* Compressed PNG bytes */
    uint32_t png_size;     /* Size of PNG data */
    uint32_t last_used;    /* LRU timestamp */
};

static PngCacheEntry png_cache[PNG_CACHE_MAX_ENTRIES] = {};
static int png_cache_count = 0;
static uint32_t png_cache_tick = 0;

/**
 * @brief Check if PNG is in cache
 * @return Pointer to PNG data in cache, or nullptr if not found
 */
static PngCacheEntry* png_cache_lookup(const char *airline) {
    for (int i = 0; i < png_cache_count; i++) {
        if (strcmp(png_cache[i].airline, airline) == 0) {
            png_cache[i].last_used = png_cache_tick++;
            log_i("PNG cache HIT: %s (%u bytes)", airline, png_cache[i].png_size);
            return &png_cache[i];
        }
    }
    return nullptr;
}

/**
 * @brief Add PNG to cache
 */
static bool png_cache_add(const char *airline, const uint8_t *png_data, uint32_t png_size) {
    if (png_size > PNG_CACHE_MAX_SIZE) {
        log_e("PNG cache: PNG too large (%u > %u max)", png_size, PNG_CACHE_MAX_SIZE);
        return false;
    }

    PngCacheEntry *entry = nullptr;

    /* Check if airline already in cache */
    for (int i = 0; i < png_cache_count; i++) {
        if (strcmp(png_cache[i].airline, airline) == 0) {
            entry = &png_cache[i];
            if (entry->png_data) free(entry->png_data);
            break;
        }
    }

    /* Add new entry if space available */
    if (!entry && png_cache_count < PNG_CACHE_MAX_ENTRIES) {
        entry = &png_cache[png_cache_count++];
    }

    /* Evict LRU if cache full */
    if (!entry) {
        int lru_idx = 0;
        uint32_t oldest = png_cache[0].last_used;
        for (int i = 1; i < PNG_CACHE_MAX_ENTRIES; i++) {
            if (png_cache[i].last_used < oldest) {
                oldest = png_cache[i].last_used;
                lru_idx = i;
            }
        }
        entry = &png_cache[lru_idx];
        if (entry->png_data) free(entry->png_data);
        log_i("PNG cache: evicted %s", entry->airline);
    }

    /* Allocate and store PNG */
    uint8_t *new_png = (uint8_t*)malloc(png_size);
    if (!new_png) {
        log_e("PNG cache: malloc failed (%u bytes)", png_size);
        return false;
    }

    memcpy(new_png, png_data, png_size);
    strncpy(entry->airline, airline, 3);
    entry->airline[3] = '\0';
    entry->png_data = new_png;
    entry->png_size = png_size;
    entry->last_used = png_cache_tick++;

    log_i("PNG cache ADD: %s (%u bytes), entries=%d/%d", airline, png_size, png_cache_count, PNG_CACHE_MAX_ENTRIES);
    return true;
}

/**
 * @brief get_logo: Cached API fetch for PNG data
 * Checks PNG cache first, fetches from API only on miss
 * @return PNG size (0 if error), buffer filled with PNG data
 */
size_t get_logo(const char *icao_airline, std::vector<uint8_t> &buffer, String &error_message)
{
    FlightConfig cfg = webConfigGet();
    if (!cfg.logoSupport) {
        log_i("Logo support disabled in config");
        return 0;
    }

    /* ── CHECK PNG CACHE FIRST ── */
    PngCacheEntry *cached_png = png_cache_lookup(icao_airline);
    if (cached_png) {
        buffer.clear();
        buffer.insert(buffer.begin(), cached_png->png_data, cached_png->png_data + cached_png->png_size);
        return cached_png->png_size;
    }

    /* ── CACHE MISS: Fetch PNG from API ── */
    const String logo_url = "http://airlines-api.logostream.dev/airlines/icao/" + String(icao_airline) + "?key=" + String(cfg.logostreamApiKey) + "&variant=icon-transparent&format=png&size=35";
    log_i("PNG cache MISS: fetching %s from API (Heap=%u)", icao_airline, ESP.getFreeHeap());

    HTTPClient client;
    client.resetCookieJar();
    if (!client.begin(logo_url.c_str()))
    {
        error_message = "Failed to start client. DNS/TCP error?";
        log_e("%s", error_message.c_str());
        return 0;
    }

    const auto httpResultCode = client.GET();
    if (httpResultCode != HTTP_CODE_OK)
    {
        client.end();
        log_e("HTTP error code: %d", httpResultCode);
        return 0;
    }

    int len = client.getSize();
    WiFiClient *stream = client.getStreamPtr();
    buffer.clear();

    if (len > 0) {
        buffer.resize(len);
        size_t bytes_read = 0;
        while (bytes_read < (size_t)len && stream->connected()) {
            if (stream->available()) {
                bytes_read += stream->readBytes(buffer.data() + bytes_read, len - bytes_read);
            }
        }
        buffer.resize(bytes_read);
    } else {
        uint8_t chunk[512];
        while (stream->connected() || stream->available()) {
            int n = stream->readBytes(chunk, sizeof(chunk));
            if (n > 0) {
                buffer.insert(buffer.end(), chunk, chunk + n);
            }
        }
    }

    client.end();
    size_t png_size = buffer.size();

    /* Store in cache for next time */
    if (png_size > 0) {
        png_cache_add(icao_airline, buffer.data(), png_size);
    }

    return png_size;
}
