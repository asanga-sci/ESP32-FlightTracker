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
constexpr const char *kApiUserAgent = "ESP32-FlightTracker (contact:gurusingha.92@gmail.com)";

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
    client.setUserAgent(kApiUserAgent);

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

/**
 * @brief Simple HTTPS POST helper (cert validation disabled — matches
 *        the GET helper above).
 *
 * adsb.lol's /api/0/routeset endpoint is fronted by a WAF that rejects
 * requests without a browser-like Referer/User-Agent (returns an empty
 * 201 instead of the JSON routeset), so we spoof both.
 */
bool https_post(const String &url, const String &body, String &response, String &error_message)
{
    WiFiClientSecure wifi_client;
    wifi_client.setInsecure();

    HTTPClient client;
    log_i("POST: %s", url.c_str());
    if (!client.begin(wifi_client, url))
    {
        error_message = "Failed to start HTTPS client";
        log_e("%s", error_message.c_str());
        return false;
    }

    client.setUserAgent(kApiUserAgent);
    client.addHeader("Content-Type", "application/json");
    client.addHeader("Referer", "https://adsb.lol/");
    const int http_code = client.POST(body);
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
 * @brief Enrich one flight with route + airline data from adsb.lol's
 *        /api/0/routeset endpoint (VRS standing data).
 * @return true if the route was resolved and is plausible.
 */
bool enrich_flight_from_adsblol(flight_info &flight)
{
    if (flight.callsign.isEmpty())
    {
        log_w("adsb.lol: no callsign to look up");
        return false;
    }

    // POST the closest flight's position so adsb.lol can compute whether
    // the route is plausible for the aircraft's current location.
    const String url = "https://api.adsb.lol/api/0/routeset";
    const String body = "{\"planes\":[{\"callsign\":\"" + flight.callsign +
                        "\",\"lat\":" + String(flight.latitude, 5) +
                        ",\"lng\":" + String(flight.longitude, 5) + "}]}";

    String response;
    String error_message;
    if (!https_post(url, body, response, error_message))
    {
        log_e("adsb.lol routeset: %s", error_message.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError parse_error = deserializeJson(doc, response);
    if (parse_error != DeserializationError::Ok)
    {
        log_e("adsb.lol routeset: parse error: %s", parse_error.c_str());
        return false;
    }

    const JsonArray routes = doc.as<JsonArray>();
    if (routes.isNull() || routes.size() == 0)
    {
        log_w("adsb.lol routeset: empty response for %s", flight.callsign.c_str());
        return false;
    }

    const JsonObject route = routes[0];

    const String airport_codes = route["airport_codes"] | "unknown";
    if (airport_codes.isEmpty() || airport_codes == "unknown")
    {
        log_w("adsb.lol routeset: no route data for %s", flight.callsign.c_str());
        return false;
    }

    // A non-plausible match is usually a callsign collision or misassociated
    // data — better to show "N/A" than a wrong route.
    const bool plausible = route["plausible"] | true;
    if (!plausible)
    {
        log_w("adsb.lol routeset: %s route %s is not plausible, skipping",
              flight.callsign.c_str(), airport_codes.c_str());
        return false;
    }

    const String airline_code = route["airline_code"] | "unknown";
    if (!airline_code.isEmpty() && airline_code != "unknown")
        flight.icao_airline = airline_code;

    // _airport_codes_iata lists origin-destination with IATA codes where the
    // airport has one, otherwise the ICAO code (e.g. "BKK-CEI").
    String codes = route["_airport_codes_iata"] | "";
    if (codes.isEmpty() || codes == "unknown")
        codes = airport_codes;

    const int dash = codes.indexOf('-');
    const int space = dash < 0 ? codes.indexOf(' ') : -1;
    if (dash > 0)
    {
        flight.origin_airport = codes.substring(0, dash);
        flight.destination_airport = codes.substring(dash + 1);
    }
    else if (space > 0)
    {
        flight.origin_airport = codes.substring(0, space);
        flight.destination_airport = codes.substring(space + 1);
    }

    // Fall back to airport name/municipality when a code is missing
    if (flight.origin_airport.isEmpty() || flight.destination_airport.isEmpty())
    {
        const JsonArray airports = route["_airports"];
        if (!airports.isNull())
        {
            for (const JsonObject ap : airports)
            {
                if (flight.origin_airport.isEmpty())
                {
                    flight.origin_airport = ap["iata"] | "";
                    if (flight.origin_airport.isEmpty())
                        flight.origin_airport = ap["icao"] | "";
                    if (flight.origin_airport.isEmpty())
                        flight.origin_airport = ap["location"] | "";
                }
                else if (flight.destination_airport.isEmpty())
                {
                    flight.destination_airport = ap["iata"] | "";
                    if (flight.destination_airport.isEmpty())
                        flight.destination_airport = ap["icao"] | "";
                    if (flight.destination_airport.isEmpty())
                        flight.destination_airport = ap["location"] | "";
                }
            }
        }
    }

    log_i("adsb.lol: %s: %s -> %s (airline %s)",
          flight.iata_callsign.c_str(),
          flight.origin_airport.c_str(),
          flight.destination_airport.c_str(),
          flight.icao_airline.c_str());
    return !flight.origin_airport.isEmpty() || !flight.destination_airport.isEmpty();
}

/* ═══════════════════════════════════════════════════════════════════════
   AIRLABS ROUTE CACHE (flight_icao → route data, reduces API calls).
   Heap-allocated on first use — BSS is already at the DRAM limit, so the
   ~2.4 KB table must NOT be a static array.
   ═══════════════════════════════════════════════════════════════════════*/
#define AIRLABS_ROUTE_CACHE_MAX 50

struct AirlabsRouteEntry
{
    char callsign[10];      /* flight_icao, e.g. "TVJ134" */
    char airline_icao[4];   /* e.g. "TVJ" */
    char airline_iata[4];   /* e.g. "VZ" */
    char flight_iata[7];    /* e.g. "VZ134" */
    char dep_iata[4];       /* e.g. "BKK" */
    char dep_icao[5];       /* e.g. "VTBS" */
    char arr_iata[4];       /* e.g. "CEI" */
    char arr_icao[5];       /* e.g. "VTCT" */
    uint32_t last_used;     /* LRU timestamp */
};

static AirlabsRouteEntry *s_airlabs_route_cache = nullptr;
static int s_airlabs_route_cache_count = 0;
static uint32_t s_airlabs_route_tick = 0;

static bool airlabs_route_cache_ensure()
{
    if (s_airlabs_route_cache != nullptr)
        return true;

    s_airlabs_route_cache = (AirlabsRouteEntry *)calloc(AIRLABS_ROUTE_CACHE_MAX, sizeof(AirlabsRouteEntry));
    if (s_airlabs_route_cache == nullptr)
    {
        log_e("airlabs route cache: calloc failed (%u bytes)",
              (unsigned)(AIRLABS_ROUTE_CACHE_MAX * sizeof(AirlabsRouteEntry)));
        return false;
    }
    log_i("airlabs route cache: allocated %d entries (Heap=%u)",
          AIRLABS_ROUTE_CACHE_MAX, ESP.getFreeHeap());
    return true;
}

static bool airlabs_route_cache_lookup(const char *callsign, AirlabsRouteEntry &out)
{
    if (s_airlabs_route_cache == nullptr)
        return false;

    for (int i = 0; i < s_airlabs_route_cache_count; i++)
    {
        if (strcmp(s_airlabs_route_cache[i].callsign, callsign) == 0)
        {
            s_airlabs_route_cache[i].last_used = s_airlabs_route_tick++;
            out = s_airlabs_route_cache[i];
            log_i("airlabs route cache HIT: %s", callsign);
            return true;
        }
    }
    return false;
}

static void airlabs_route_cache_add(const AirlabsRouteEntry &entry)
{
    if (s_airlabs_route_cache == nullptr)
        return;

    /* Refresh an existing entry, if present */
    for (int i = 0; i < s_airlabs_route_cache_count; i++)
    {
        if (strcmp(s_airlabs_route_cache[i].callsign, entry.callsign) == 0)
        {
            s_airlabs_route_cache[i] = entry;
            s_airlabs_route_cache[i].last_used = s_airlabs_route_tick++;
            return;
        }
    }

    AirlabsRouteEntry *slot = nullptr;
    if (s_airlabs_route_cache_count < AIRLABS_ROUTE_CACHE_MAX)
    {
        slot = &s_airlabs_route_cache[s_airlabs_route_cache_count++];
    }
    else
    {
        /* Evict the least-recently-used entry */
        int lru_idx = 0;
        uint32_t oldest = s_airlabs_route_cache[0].last_used;
        for (int i = 1; i < AIRLABS_ROUTE_CACHE_MAX; i++)
        {
            if (s_airlabs_route_cache[i].last_used < oldest)
            {
                oldest = s_airlabs_route_cache[i].last_used;
                lru_idx = i;
            }
        }
        slot = &s_airlabs_route_cache[lru_idx];
        log_i("airlabs route cache: evicted %s", slot->callsign);
    }

    *slot = entry;
    slot->last_used = s_airlabs_route_tick++;
    log_i("airlabs route cache ADD: %s (entries=%d/%d)", entry.callsign,
          s_airlabs_route_cache_count, AIRLABS_ROUTE_CACHE_MAX);
}

/**
 * @brief Apply a cached/fresh AirLabs route to a flight.
 */
static void airlabs_apply_route(flight_info &flight, const AirlabsRouteEntry &route)
{
    flight.icao_airline = route.airline_icao;

    if (route.flight_iata[0] != '\0')
        flight.iata_callsign = route.flight_iata;

    // IATA code is preferred for the compact route row; fall back to ICAO
    flight.origin_airport = route.dep_iata;
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route.dep_icao;

    flight.destination_airport = route.arr_iata;
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route.arr_icao;
}

/**
 * @brief Fallback enrichment using the AirLabs routes API.
 *        Used when adsb.lol reports an unknown/non-plausible route.
 * @return true if the route was resolved (from cache or network).
 */
bool enrich_flight_from_airlabs(flight_info &flight)
{
    if (flight.callsign.isEmpty())
    {
        log_w("airlabs: no callsign to look up");
        return false;
    }

    FlightConfig cfg = webConfigGet();
    if (!cfg.airlabsFallback)
    {
        log_i("airlabs: fallback disabled, skipping %s", flight.callsign.c_str());
        return false;
    }
    if (cfg.airlabsApiKey[0] == '\0')
    {
        log_i("airlabs: no API key configured, skipping fallback for %s",
              flight.callsign.c_str());
        return false;
    }

    /* ── Check route cache first ── */
    AirlabsRouteEntry cached;
    if (airlabs_route_cache_lookup(flight.callsign.c_str(), cached))
    {
        airlabs_apply_route(flight, cached);
        log_i("airlabs (cached): %s: %s -> %s (airline %s)",
              flight.iata_callsign.c_str(),
              flight.origin_airport.c_str(),
              flight.destination_airport.c_str(),
              flight.icao_airline.c_str());
        return true;
    }

    if (!airlabs_route_cache_ensure())
        return false;

    const String url = "https://airlabs.co/api/v9/routes?api_key=" + String(cfg.airlabsApiKey) +
                       "&flight_icao=" + flight.callsign +
                       "&_fields=airline_iata,airline_icao,flight_iata,flight_number,dep_iata,dep_icao,arr_iata,arr_icao";

    String response;
    String error_message;
    if (!https_get(url, response, error_message))
    {
        log_e("airlabs: %s", error_message.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError parse_error = deserializeJson(doc, response);
    if (parse_error != DeserializationError::Ok)
    {
        log_e("airlabs: parse error: %s", parse_error.c_str());
        return false;
    }

    const JsonArray routes = doc["response"];
    if (routes.isNull() || routes.size() == 0)
    {
        log_w("airlabs: no route for %s", flight.callsign.c_str());
        return false;
    }

    const JsonObject route = routes[0];

    AirlabsRouteEntry entry = {};
    strncpy(entry.callsign, flight.callsign.c_str(), sizeof(entry.callsign) - 1);
    strncpy(entry.airline_icao, route["airline_icao"] | "", sizeof(entry.airline_icao) - 1);
    strncpy(entry.airline_iata, route["airline_iata"] | "", sizeof(entry.airline_iata) - 1);
    strncpy(entry.flight_iata, route["flight_iata"] | "", sizeof(entry.flight_iata) - 1);
    strncpy(entry.dep_iata, route["dep_iata"] | "", sizeof(entry.dep_iata) - 1);
    strncpy(entry.dep_icao, route["dep_icao"] | "", sizeof(entry.dep_icao) - 1);
    strncpy(entry.arr_iata, route["arr_iata"] | "", sizeof(entry.arr_iata) - 1);
    strncpy(entry.arr_icao, route["arr_icao"] | "", sizeof(entry.arr_icao) - 1);

    airlabs_apply_route(flight, entry);
    airlabs_route_cache_add(entry);

    log_i("airlabs: %s: %s -> %s (airline %s)",
          flight.iata_callsign.c_str(),
          flight.origin_airport.c_str(),
          flight.destination_airport.c_str(),
          flight.icao_airline.c_str());
    return true;
}
} // namespace

bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude, bool air, bool ground, bool gliders, bool vehicles, std::vector<flight_info> &flights, String &error_message)
{
    (void)range_longitude;
    (void)air;
    (void)gliders;
    (void)vehicles;

    int radius_km = (int)(range_latitude + 0.5f);
    if (radius_km < 1)
    {
        radius_km = 1;
    }

    flights.clear();
    error_message = "";

    // ── 1. adsb.lol: all aircraft within a circle ───────────────────────
    // adsb.lol's v2/point radius is in nautical miles (max 250 NM).
    int radius_nm = (int)(radius_km / 1.852f + 0.5f);
    if (radius_nm < 1)
        radius_nm = 1;
    if (radius_nm > 250)
        radius_nm = 250;
    const String url = "https://api.adsb.lol/v2/point/" + String(latitude, 5) +
                       "/" + String(longitude, 5) + "/" + String(radius_nm);

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
    response = ""; /* free the raw payload before adsb.lol enrichment */
    if (parse_error != DeserializationError::Ok)
    {
        log_e("adsb.lol: parse error: %s", parse_error.c_str());
        error_message = parse_error.c_str();
        return false;
    }

    const JsonArray aircraft = doc["ac"];
    if (aircraft.isNull())
    {
        error_message = "adsb.lol: unexpected payload";
        log_e("%s", error_message.c_str());
        return false;
    }

    const time_t now_secs = time(nullptr);
    for (const JsonObject obj : aircraft)
    {
        flight_info flight = {};

        flight.icao_address = obj["hex"] | "";
        flight.latitude = obj["lat"] | 0.0f;
        flight.longitude = obj["lon"] | 0.0f;
        flight.heading = (int)(obj["track"] | 0.0f);
        flight.ground_speed = (int)(obj["gs"] | 0.0f);
        flight.squawk = obj["squawk"] | "";
        flight.aircraft_code = obj["t"] | "";
        flight.registration = obj["r"] | "";

        // alt_baro is barometric altitude in feet, or the string "ground"
        if (obj["alt_baro"] == "ground")
        {
            flight.on_ground = true;
        }
        else
        {
            flight.altitude = obj["alt_baro"] | 0;
        }

        flight.vertical_speed = (int)(obj["baro_rate"] | 0.0f);
        const int seen_secs = (int)(obj["seen"] | 0);
        flight.timestamp = now_secs - seen_secs;

        String callsign = obj["flight"] | "";
        callsign.trim();
        flight.callsign = callsign;
        flight.iata_callsign = callsign;

        // Actually on the ground: the transponder reports surface position
        // (alt_baro == "ground") AND the aircraft is moving slowly. Aircraft
        // on final approach can briefly decode to 0 ft / "ground" while still
        // airborne and fast — combining with ground speed keeps them visible.
        if (!ground && flight.on_ground && flight.ground_speed < 50)
            continue;

        flights.push_back(flight);
    }

    log_i("adsb.lol: %u aircraft in radius %d nm (Heap=%u)", flights.size(), radius_nm, ESP.getFreeHeap());

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

    // ── 3. adsb.lol: resolve route/airline data for the closest aircraft ─
    // If adsb.lol reports an unknown/non-plausible route, fall back to
    // AirLabs routes (when configured).
    if (!enrich_flight_from_adsblol(flights.front()))
    {
        enrich_flight_from_airlabs(flights.front());
    }

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
 * @brief Remove a PNG from the cache (e.g. after a decode failure).
 *        Prevents a corrupt/undecodable PNG from being re-served forever.
 */
void png_cache_remove(const char *airline)
{
    for (int i = 0; i < png_cache_count; i++)
    {
        if (strcmp(png_cache[i].airline, airline) == 0)
        {
            if (png_cache[i].png_data)
                free(png_cache[i].png_data);
            png_cache[i] = png_cache[png_cache_count - 1];
            png_cache_count--;
            log_i("PNG cache REMOVE: %s (entries=%d/%d)", airline, png_cache_count, PNG_CACHE_MAX_ENTRIES);
            return;
        }
    }
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
    client.setUserAgent(kApiUserAgent);

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
