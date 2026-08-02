#include "flight_info_fr24.h"

#include<vector>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <web_config.h>
#include <esp32-hal-log.h>

String generateMockRsponse(int numFlights);

bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude, bool air, bool ground, bool gliders, bool vehicles, std::vector<flight_info> &flights, String &error_message)
{
    const String flight_data_base_url = "http://data-cloud.flightradar24.com/zones/fcgi/feed.js";
    const String bounds = String(latitude + range_latitude / 2.0) + "," + String(latitude - range_latitude / 2.0) + "," + String(longitude - range_longitude / 2.0) + "," + String(longitude + range_longitude / 2.0);
    const String flight_data_url = flight_data_base_url + "?" + "bounds=" + bounds + "&faa=1&satellite=1&mlat=1&flarm=1&adsb=1&gnd=" + String(ground) + "&air=" + String(air) + "&vehicles=" + String(vehicles) + "&estimated=1&maxage=14400&gliders=" + String(gliders) + "&stats=0";

    flights.clear();
    error_message = "";

    HTTPClient client;
    client.resetCookieJar();
    log_i("Request states=%s", flight_data_url.c_str());
    if (!client.begin(flight_data_url))
    {
        error_message = "Failed to start client. DNS/TCP error?";
        log_e("%s", error_message.c_str());
        return false;
    }

    const auto httpResultCode = client.GET();
    if (httpResultCode != HTTP_CODE_OK)
    {
        client.end();
        log_e("HTTP error code: %d", httpResultCode);
        return false;
    }

    auto response = client.getString();
    // auto response = generateMockRsponse(30);
    log_i("Body=%s", response.c_str());

    // Parse JSON states object 32k
    JsonDocument doc_flight_data;
    const auto error = deserializeJson(doc_flight_data, response);
    if (error != DeserializationError::Ok)
    {
        client.end();
        log_e("Deserialize. Error=%s", error.c_str());
        error_message = error.c_str();
        return false;
    }

    log_i(
        "memory stat after desirialization: size=%u Heap=%u Largest=%u",
        response.length(),
        ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );

    client.end();
    int flight_count = 0;
    bool saw_flight_arrays = false;

    auto flight_data_root = doc_flight_data.as<JsonObject>();
    for (JsonPair kvp : flight_data_root)
    {
        if (!kvp.value().is<JsonArray>())
            continue;

        saw_flight_arrays = true;
        log_i("KVP=%s", kvp.key().c_str());
        auto items = kvp.value().as<JsonArray>();
        flight_count++;

        // NOTE: Must copy strings from JSON immediately — they become invalid
        // after the JsonDocument goes out of scope!
        struct flight_info flight;
        flight.icao_address = String(items[0].as<const char *>() ?: "");
        flight.latitude = items[1].as<const float>();
        flight.longitude = items[2].as<const float>();
        flight.heading = items[3].as<const int>();
        flight.altitude = items[4].as<const int>();
        flight.ground_speed = items[5].as<const int>();
        flight.squawk = String(items[6].as<const char *>() ?: "");
        flight.radar = String(items[7].as<const char *>() ?: "");
        flight.aircraft_code = String(items[8].as<const char *>() ?: "");
        flight.registration = String(items[9].as<const char *>() ?: "");
        flight.timestamp = items[10].as<time_t>();
        flight.iata_origin_airport = String(items[11].as<const char *>() ?: "");
        flight.iata_destination_airport = String(items[12].as<const char *>() ?: "");
        flight.flight = String(items[13].as<const char *>() ?: "");
        flight.on_ground = items[14].as<const bool>();
        flight.vertical_speed = items[15].as<const int>();
        flight.call_sign = String(items[16].as<const char *>() ?: "");
        flight.icao_airline = String(items[18].as<const char *>() ?: "");

        flights.push_back(flight);
    }
    if (!saw_flight_arrays)
    {
        log_w("FR24 returned metadata-only payload: %s", response.c_str());
    }
    log_i("flight count:%d", flight_count);
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

String generateMockRsponse(int numFlights)
{

    String json;
    float radar_origin_lat = 13.7078; // set radar origin to home for demo
    float radar_origin_lon = 100.6017;

    json.reserve(numFlights * 180);

    json += "{";
    json += "\"full_count\":";
    json += String(numFlights);
    json += ",";
    json += "\"version\":4,";

    for (int i = 0; i < numFlights; i++)
    {
        char key[16];
        snprintf(key, sizeof(key), "%08X", i);

                float lat =
            radar_origin_lat +
            ((float)random(-500, 500) / 1000.0f);

        float lon =
            radar_origin_lon +
            ((float)random(-500, 500) / 1000.0f);

        const char *aircraftTypes[] =
        {
            "A320",
            "A321",
            "B738",
            "B77W",
            "A359",
            "B789"
        };

        const char *airlines[] =
        {
            "THA",
            "AXM",
            "CPA",
            "SIA",
            "UAE",
            "QTR"
        };

        const char *airline =
            airlines[random(0, 6)];

        char flight[16];
        sprintf(
            flight,
            "%s%d",
            airline,
            random(10, 9999));

        json += "\"";
        json += key;
        json += "\":[";

        json += "\"06A13B\",";
        json += String(lat, 5);
        json += ",";
        json += String(lon, 5);
        json += ",";
        json += String(random(0, 360));
        json += ",";
        json += String(random(2000, 41000));
        json += ",";
        json += String(random(150, 550));
        json += ",";
        json += "\"7000\",";
        json += "\"T-BKK\",";
        json += "\"A320\",";
        json += "\"HS1234\",";
        json += String(time(nullptr));
        json += ",";
        json += "\"BKK\",";
        json += "\"SIN\",";
        json += "\"";
        json += flight;
        json += "\",";
        json += "false,";
        json += String(random(-3000, 3000));
        json += ",";
        json += "\"";
        json += flight;
        json += "\",";
        json += "null,";
        json += "\"";
        json += airline;
        json += "\"";

        json += "]";

        if(i < numFlights - 1)
            json += ",";
    }

    json += "}";
    return json;
}
