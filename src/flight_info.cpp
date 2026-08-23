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

bool https_post_json(const String &url, const String &json_body, String &response, String &error_message)
{
    WiFiClientSecure wifi_client;
    wifi_client.setInsecure();

    HTTPClient client;
    log_i("POST: %s :: %s", url.c_str(), json_body.c_str());
    if (!client.begin(wifi_client, url))
    {
        error_message = "Failed to start HTTPS POST client";
        log_e("%s", error_message.c_str());
        return false;
    }

    client.addHeader("Content-Type", "application/json");
    const int http_code = client.POST((uint8_t *)json_body.c_str(), json_body.length());
    if (http_code != HTTP_CODE_OK)
    {
        client.end();
        log_e("HTTPS POST error code: %d for %s", http_code, url.c_str());
        error_message = "HTTP POST failed (" + String(http_code) + ")";
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
    if (flight.callsign.isEmpty())
    {
        log_w("adsbdb: no callsign to look up");
        return false;
    }

    const String url = "https://api.adsbdb.com/v0/callsign/" + flight.callsign;

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
        log_w("adsbdb: no route data for %s", flight.callsign.c_str());
        return false;
    }

    flight.icao_airline = route["airline"]["icao"] | "";
    flight.airline_name = route["airline"]["name"] | "";

    // Replace the raw ICAO callsign with the IATA callsign from adsbdb
    const String iata_callsign = route["callsign_iata"] | "";
    if (!iata_callsign.isEmpty())
        flight.iata_callsign = iata_callsign;

    // IATA code is preferred for the compact route row; fall back to
    // ICAO code, then municipality, then airport name
    flight.origin_airport = route["origin"]["iata_code"] | "";
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route["origin"]["icao_code"] | "";
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route["origin"]["municipality"] | "";
    if (flight.origin_airport.isEmpty())
        flight.origin_airport = route["origin"]["name"] | "";

    flight.destination_airport = route["destination"]["iata_code"] | "";
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route["destination"]["icao_code"] | "";
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route["destination"]["municipality"] | "";
    if (flight.destination_airport.isEmpty())
        flight.destination_airport = route["destination"]["name"] | "";

    log_i("adsbdb: %s: %s -> %s (airline %s)",
          flight.iata_callsign.c_str(),
          flight.origin_airport.c_str(),
          flight.destination_airport.c_str(),
          flight.airline_name.c_str());
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
   ADSB.LOL ROUTE CACHE (flight_icao → route data, reduces API calls).
   Heap-allocated on first use — BSS is already at the DRAM limit, so the
   ~2.4 KB table must NOT be a static array.
   ═══════════════════════════════════════════════════════════════════════*/
#define ADSB_LOL_ROUTE_CACHE_MAX 50

struct AdsbLolRouteEntry
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

static AdsbLolRouteEntry *s_adsb_lol_route_cache = nullptr;
static int s_adsb_lol_route_cache_count = 0;
static uint32_t s_adsb_lol_route_tick = 0;

static bool adsb_lol_route_cache_ensure()
{
    if (s_adsb_lol_route_cache != nullptr)
        return true;

    s_adsb_lol_route_cache = (AdsbLolRouteEntry *)calloc(ADSB_LOL_ROUTE_CACHE_MAX, sizeof(AdsbLolRouteEntry));
    if (s_adsb_lol_route_cache == nullptr)
    {
        log_e("adsb.lol route cache: calloc failed (%u bytes)",
              (unsigned)(ADSB_LOL_ROUTE_CACHE_MAX * sizeof(AdsbLolRouteEntry)));
        return false;
    }
    log_i("adsb.lol route cache: allocated %d entries (Heap=%u)",
          ADSB_LOL_ROUTE_CACHE_MAX, ESP.getFreeHeap());
    return true;
}

static bool adsb_lol_route_cache_lookup(const char *callsign, AdsbLolRouteEntry &out)
{
    if (s_adsb_lol_route_cache == nullptr)
        return false;

    for (int i = 0; i < s_adsb_lol_route_cache_count; i++)
    {
        if (strcmp(s_adsb_lol_route_cache[i].callsign, callsign) == 0)
        {
            s_adsb_lol_route_cache[i].last_used = s_adsb_lol_route_tick++;
            out = s_adsb_lol_route_cache[i];
            log_i("adsb.lol route cache HIT: %s", callsign);
            return true;
        }
    }
    return false;
}

static void adsb_lol_route_cache_add(const AdsbLolRouteEntry &entry)
{
    if (s_adsb_lol_route_cache == nullptr)
        return;

    /* Refresh an existing entry, if present */
    for (int i = 0; i < s_adsb_lol_route_cache_count; i++)
    {
        if (strcmp(s_adsb_lol_route_cache[i].callsign, entry.callsign) == 0)
        {
            s_adsb_lol_route_cache[i] = entry;
            s_adsb_lol_route_cache[i].last_used = s_adsb_lol_route_tick++;
            return;
        }
    }

    AdsbLolRouteEntry *slot = nullptr;
    if (s_adsb_lol_route_cache_count < ADSB_LOL_ROUTE_CACHE_MAX)
    {
        slot = &s_adsb_lol_route_cache[s_adsb_lol_route_cache_count++];
    }
    else
    {
        /* Evict the least-recently-used entry */
        int lru_idx = 0;
        uint32_t oldest = s_adsb_lol_route_cache[0].last_used;
        for (int i = 1; i < ADSB_LOL_ROUTE_CACHE_MAX; i++)
        {
            if (s_adsb_lol_route_cache[i].last_used < oldest)
            {
                oldest = s_adsb_lol_route_cache[i].last_used;
                lru_idx = i;
            }
        }
        slot = &s_adsb_lol_route_cache[lru_idx];
        log_i("adsb.lol route cache: evicted %s", slot->callsign);
    }

    *slot = entry;
    slot->last_used = s_adsb_lol_route_tick++;
    log_i("adsb.lol route cache ADD: %s (entries=%d/%d)", entry.callsign,
          s_adsb_lol_route_cache_count, ADSB_LOL_ROUTE_CACHE_MAX);
}

/**
 * @brief Apply a cached/fresh ADSB.lol route to a flight.
 */
static void adsb_lol_apply_route(flight_info &flight, const AdsbLolRouteEntry &route)
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

static bool adsb_lol_extract_route(const JsonVariant &node, AdsbLolRouteEntry &entry)
{
    if (node.isNull())
        return false;

    JsonVariant current = node;
    if (current.is<JsonArray>())
    {
        for (JsonVariant item : current.as<JsonArray>())
        {
            if (adsb_lol_extract_route(item, entry))
                return true;
        }
        return false;
    }

    if (!current.is<JsonObject>())
        return false;

    const JsonObject obj = current.as<JsonObject>();
    const String airline_icao = obj["airline_icao"] | "";
    const String airline_iata = obj["airline_iata"] | "";
    const String flight_iata = obj["flight_iata"] | "";
    const String dep_iata = obj["dep_iata"] | "";
    const String dep_icao = obj["dep_icao"] | "";
    const String arr_iata = obj["arr_iata"] | "";
    const String arr_icao = obj["arr_icao"] | "";

    if (airline_icao.isEmpty() && airline_iata.isEmpty() && flight_iata.isEmpty() &&
        dep_iata.isEmpty() && dep_icao.isEmpty() && arr_iata.isEmpty() && arr_icao.isEmpty())
    {
        for (JsonPair kv : obj)
        {
            if (adsb_lol_extract_route(kv.value(), entry))
                return true;
        }
        return false;
    }

    memset(&entry, 0, sizeof(entry));
    strncpy(entry.airline_icao, airline_icao.c_str(), sizeof(entry.airline_icao) - 1);
    strncpy(entry.airline_iata, airline_iata.c_str(), sizeof(entry.airline_iata) - 1);
    strncpy(entry.flight_iata, flight_iata.c_str(), sizeof(entry.flight_iata) - 1);
    strncpy(entry.dep_iata, dep_iata.c_str(), sizeof(entry.dep_iata) - 1);
    strncpy(entry.dep_icao, dep_icao.c_str(), sizeof(entry.dep_icao) - 1);
    strncpy(entry.arr_iata, arr_iata.c_str(), sizeof(entry.arr_iata) - 1);
    strncpy(entry.arr_icao, arr_icao.c_str(), sizeof(entry.arr_icao) - 1);
    return true;
}

/**
 * @brief Fallback enrichment using the ADSB.lol routeset API.
 *        Used when adsbdb reports "unknown callsign" (HTTP 404 / null route).
 * @return true if the route was resolved (from cache or network).
 */
bool enrich_flight_from_adsb_lol(flight_info &flight)
{
    if (flight.callsign.isEmpty())
    {
        log_w("adsb.lol: no callsign to look up");
        return false;
    }

    /* ── Check route cache first ── */
    AdsbLolRouteEntry cached;
    if (adsb_lol_route_cache_lookup(flight.callsign.c_str(), cached))
    {
        adsb_lol_apply_route(flight, cached);
        log_i("adsb.lol (cached): %s: %s -> %s (airline %s)",
              flight.iata_callsign.c_str(),
              flight.origin_airport.c_str(),
              flight.destination_airport.c_str(),
              flight.icao_airline.c_str());
        return true;
    }

    if (!adsb_lol_route_cache_ensure())
        return false;

    const String body = String("{\"planes\":[{\"callsign\":\"") + flight.callsign +
                        String("\",\"lat\":") + String(flight.latitude, 5) +
                        String(",\"lng\":") + String(flight.longitude, 5) +
                        String("}]} ");

    String response;
    String error_message;
    if (!https_post_json("https://api.adsb.lol/api/0/routeset", body, response, error_message))
    {
        log_e("adsb.lol: %s", error_message.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError parse_error = deserializeJson(doc, response);
    if (parse_error != DeserializationError::Ok)
    {
        log_e("adsb.lol: parse error: %s", parse_error.c_str());
        return false;
    }

    AdsbLolRouteEntry entry = {};
    strncpy(entry.callsign, flight.callsign.c_str(), sizeof(entry.callsign) - 1);

    JsonVariant route_candidate = doc["route"];
    if (route_candidate.isNull())
        route_candidate = doc["routes"];
    if (route_candidate.isNull())
        route_candidate = doc["response"];
    if (route_candidate.isNull())
        route_candidate = doc;

    if (!adsb_lol_extract_route(route_candidate, entry))
    {
        log_w("adsb.lol: no route for %s in payload", flight.callsign.c_str());
        return false;
    }

    adsb_lol_apply_route(flight, entry);
    adsb_lol_route_cache_add(entry);

    log_i("adsb.lol: %s: %s -> %s (airline %s)",
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
    // If adsbdb reports an unknown callsign, fall back to adsb.lol routeset.
    if (!enrich_flight_from_adsbdb(flights.front()))
    {
        enrich_flight_from_adsb_lol(flights.front());
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
