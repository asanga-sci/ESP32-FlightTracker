#include "airport_info.h"

#include "web_config.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <esp32-hal-log.h>

namespace
{
constexpr const char *kApiUserAgent = "ESP32-FlightTracker (contact:gurusingha.92@gmail.com)";

bool extract_json_string(const char *json, const char *key, char *value, size_t value_capacity)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (pos == nullptr)
    {
        return false;
    }

    pos = strchr(pos + strlen(needle), ':');
    if (pos == nullptr)
    {
        return false;
    }
    ++pos;
    while (*pos != '\0' && isspace(static_cast<unsigned char>(*pos)))
    {
        ++pos;
    }
    if (*pos != '"')
    {
        return false;
    }
    ++pos;

    const char *end = pos;
    while (*end != '\0' && *end != '"')
    {
        if (*end == '\\')
        {
            end += 2;
        }
        else
        {
            ++end;
        }
    }
    if (*end != '"')
    {
        return false;
    }

    size_t length = static_cast<size_t>(end - pos);
    if (length >= value_capacity)
    {
        length = value_capacity - 1;
    }
    memcpy(value, pos, length);
    value[length] = '\0';
    return true;
}

bool extract_json_float(const char *json, const char *key, float &value)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (pos == nullptr)
    {
        return false;
    }

    pos = strchr(pos + strlen(needle), ':');
    if (pos == nullptr)
    {
        return false;
    }
    ++pos;
    while (*pos != '\0' && isspace(static_cast<unsigned char>(*pos)))
    {
        ++pos;
    }

    char *end = nullptr;
    value = strtof(pos, &end);
    return end != pos;
}

const char *skip_ws(const char *ptr)
{
    while (*ptr != '\0' && isspace(static_cast<unsigned char>(*ptr)))
    {
        ++ptr;
    }
    return ptr;
}

const char *find_matching_brace(const char *start)
{
    int depth = 0;
    const char *cursor = start;
    while (*cursor != '\0')
    {
        if (*cursor == '{')
        {
            ++depth;
        }
        else if (*cursor == '}')
        {
            --depth;
            if (depth == 0)
            {
                return cursor;
            }
        }
        ++cursor;
    }
    return nullptr;
}

void parse_runway_object(const char *object_text, runway_info &runway)
{
    runway.ident[0] = '\0';
    runway.heading_deg = 0.0f;
    runway.length_ft = 0.0f;
    runway.width_ft = 0.0f;

    extract_json_string(object_text, "ident", runway.ident, sizeof(runway.ident));
    if (runway.ident[0] == '\0')
    {
        extract_json_string(object_text, "id", runway.ident, sizeof(runway.ident));
    }
    if (runway.ident[0] == '\0')
    {
        extract_json_string(object_text, "name", runway.ident, sizeof(runway.ident));
    }
    extract_json_float(object_text, "heading_deg", runway.heading_deg);
    if (runway.heading_deg == 0.0f)
    {
        extract_json_float(object_text, "true_heading_deg", runway.heading_deg);
    }
    if (runway.heading_deg == 0.0f)
    {
        extract_json_float(object_text, "heading", runway.heading_deg);
    }
    extract_json_float(object_text, "length_ft", runway.length_ft);
    if (runway.length_ft == 0.0f)
    {
        extract_json_float(object_text, "length", runway.length_ft);
    }
    if (runway.length_ft == 0.0f)
    {
        extract_json_float(object_text, "length_m", runway.length_ft);
    }
    extract_json_float(object_text, "width_ft", runway.width_ft);
    if (runway.width_ft == 0.0f)
    {
        extract_json_float(object_text, "width", runway.width_ft);
    }
    if (runway.width_ft == 0.0f)
    {
        extract_json_float(object_text, "width_m", runway.width_ft);
    }
}

bool fetch_airport_runways(const String &icao, airport_runway_info &airport, String &error_message)
{
    const String airportdb_url = String("https://airportdb.io/api/v1/airport/") + icao + "?apiToken=6cd0031bbfed0a6799d752b248e91e399f3f01c29f5fefe4eb95c156b8de185bb9ea6bda84fb89441915b0b88c6ac2b1";
    log_i("Fetching runways for %s", icao.c_str());

    WiFiClientSecure wifi_client;
    wifi_client.setInsecure(); // Disable certificate validation for this example. In production, you should validate the server's certificate.
    HTTPClient client;
    log_i(
        "memory stat before airportdb call: Heap=%u Largest=%u",
        ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );
    if (!client.begin(wifi_client, airportdb_url))
    {
        error_message = "Failed to start airportdb client";
        log_e("%s", error_message.c_str());
        return false;
    }
    client.setUserAgent(kApiUserAgent);

    int httpCode = client.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        client.end();
        log_e("AirportDB HTTP error code: %d", httpCode);
        error_message = "AirportDB request failed";
        return false;
    }

    String payload = client.getString();
    client.end();

    const char *json = payload.c_str();
    extract_json_string(json, "icao", airport.icao, sizeof(airport.icao));
    if (airport.icao[0] == '\0')
    {
        strncpy(airport.icao, icao.c_str(), sizeof(airport.icao) - 1);
        airport.icao[sizeof(airport.icao) - 1] = '\0';
    }
    extract_json_string(json, "name", airport.name, sizeof(airport.name));
    if (airport.name[0] == '\0')
    {
        extract_json_string(json, "airport_name", airport.name, sizeof(airport.name));
    }
    if (airport.name[0] == '\0')
    {
        extract_json_string(json, "display_name", airport.name, sizeof(airport.name));
    }
    extract_json_float(json, "latitude_deg", airport.latitude);
    if (airport.latitude == 0.0f)
    {
        extract_json_float(json, "latitude", airport.latitude);
    }
    if (airport.latitude == 0.0f)
    {
        extract_json_float(json, "lat", airport.latitude);
    }
    extract_json_float(json, "longitude_deg", airport.longitude);
    if (airport.longitude == 0.0f)
    {
        extract_json_float(json, "longitude", airport.longitude);
    }
    if (airport.longitude == 0.0f)
    {
        extract_json_float(json, "lon", airport.longitude);
    }
    if (airport.longitude == 0.0f)
    {
        extract_json_float(json, "lng", airport.longitude);
    }

    const char *runways_key = strstr(json, "\"runways\"");
    if (runways_key != nullptr)
    {
        const char *array_start = strchr(runways_key, '[');
        if (array_start != nullptr)
        {
            const char *cursor = array_start + 1;
            while (*cursor != '\0')
            {
                cursor = skip_ws(cursor);
                if (*cursor == ']')
                {
                    break;
                }
                if (*cursor == '{')
                {
                    const char *object_end = find_matching_brace(cursor);
                    if (object_end != nullptr)
                    {
                        runway_info runway;
                        parse_runway_object(cursor, runway);
                        airport.runways.push_back(runway);
                        cursor = object_end + 1;
                        continue;
                    }
                }
                ++cursor;
            }
        }
    }

    return true;
}
} // namespace

bool get_nearby_airports_and_runways(float latitude, float longitude, float distance_km,
                                    std::vector<airport_runway_info> &airports,
                                    String &error_message)
{
    airports.clear();
    error_message = "";

    FlightConfig cfg = webConfigGet();

    String nearby_url = String("http://aviation-api.logostream.dev/v1/nearby?lat=") + String(latitude, 6) +
                        "&lng=" + String(longitude, 6) +
                        "&distance=" + String(distance_km, 0);

    log_i("Fetching nearby airports from %s", nearby_url.c_str());

    HTTPClient client;
    if (!client.begin(nearby_url))
    {
        error_message = "Failed to start nearby-airport client";
        log_e("%s", error_message.c_str());
        return false;
    }
    client.setUserAgent(kApiUserAgent);

    if (cfg.logostreamApiKey[0] != '\0')
    {
        client.addHeader("x-api-key", cfg.logostreamApiKey);
    }

    int httpCode = client.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        client.end();
        log_e("Nearby airport HTTP error code: %d", httpCode);
        error_message = "Nearby airport request failed";
        return false;
    }

    String payload = client.getString();
    client.end();

    JsonDocument doc;
    DeserializationError parse_error = deserializeJson(doc, payload);
    if (parse_error)
    {
        error_message = "Nearby airport endpoint returned an invalid payload";
        log_e("%s: %s", error_message.c_str(), parse_error.c_str());
        return false;
    }

    if (!doc["airports"].is<JsonArray>())
    {
        error_message = "Nearby airport endpoint returned an unexpected payload";
        log_e("%s", error_message.c_str());
        return false;
    }

    JsonArray airport_items = doc["airports"].as<JsonArray>();
    for (JsonVariant airport_variant : airport_items)
    {
        if (!airport_variant.is<JsonObject>())
        {
            continue;
        }

        JsonObject airport_json = airport_variant.as<JsonObject>();
        airport_runway_info airport;
        airport.icao[0] = '\0';
        airport.name[0] = '\0';

        const char *icao_value = nullptr;
        if (!airport_json["icao_code"].isNull())
        {
            icao_value = airport_json["icao_code"].as<const char *>();
        }
        if (icao_value == nullptr || icao_value[0] == '\0')
        {
            icao_value = airport_json["iata_code"].as<const char *>();
        }
        if (icao_value == nullptr || icao_value[0] == '\0')
        {
            icao_value = airport_json["airport_code"].as<const char *>();
        }
        if (icao_value != nullptr && icao_value[0] != '\0')
        {
            strncpy(airport.icao, icao_value, sizeof(airport.icao) - 1);
            airport.icao[sizeof(airport.icao) - 1] = '\0';

            const char *name_value = nullptr;
            if (!airport_json["name"].isNull())
            {
                name_value = airport_json["name"].as<const char *>();
            }
            if (name_value == nullptr || name_value[0] == '\0')
            {
                name_value = airport_json["airport_name"].as<const char *>();
            }
            if (name_value == nullptr || name_value[0] == '\0')
            {
                name_value = airport_json["display_name"].as<const char *>();
            }
            if (name_value != nullptr && name_value[0] != '\0')
            {
                strncpy(airport.name, name_value, sizeof(airport.name) - 1);
                airport.name[sizeof(airport.name) - 1] = '\0';
            }

            if (!airport_json["lat"].isNull())
            {
                airport.latitude = airport_json["lat"].as<float>();
            }
            if (airport.latitude == 0.0f && !airport_json["latitude"].isNull())
            {
                airport.latitude = airport_json["latitude"].as<float>();
            }
            if (airport.latitude == 0.0f && !airport_json["latitude_deg"].isNull())
            {
                airport.latitude = airport_json["latitude_deg"].as<float>();
            }

            if (!airport_json["lng"].isNull())
            {
                airport.longitude = airport_json["lng"].as<float>();
            }
            if (airport.longitude == 0.0f && !airport_json["lon"].isNull())
            {
                airport.longitude = airport_json["lon"].as<float>();
            }
            if (airport.longitude == 0.0f && !airport_json["longitude"].isNull())
            {
                airport.longitude = airport_json["longitude"].as<float>();
            }
            if (airport.longitude == 0.0f && !airport_json["longitude_deg"].isNull())
            {
                airport.longitude = airport_json["longitude_deg"].as<float>();
            }

            String runway_error;
            if (!fetch_airport_runways(airport.icao, airport, runway_error))
            {
                log_w("Skipping airport %s because runway lookup failed: %s", airport.icao, runway_error.c_str());
            }
            else
            {
                airports.push_back(airport);
                if (airports.size() >= 6)
                {
                    break;
                }
            }
        }
    }

    if (airports.empty())
    {
        error_message = "No nearby airports with runway data were found";
        log_w("%s", error_message.c_str());
        return false;
    }

    return true;
}
