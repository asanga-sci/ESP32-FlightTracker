#pragma once

#include <Arduino.h>
#include <list>
#include <vector>

// Conversions to metric
#define FT_TO_M 0.3048
#define KTS_TO_KMH 1.852f

// Open-data flight model.
// Positions/telemetry come from adsb.lol (community ADS-B aggregator);
// origin/destination airports + airline code come from adsb.lol's
// /api/0/routeset endpoint (VRS standing data).
struct flight_info
{
    String icao_address;             // ICAO 24-bit address hex (e.g. "8832CB")
    float latitude;
    float longitude;
    int heading;                     // degrees true
    int altitude;                    // barometric altitude (feet), 0 when on ground
    int ground_speed;                // knots
    String squawk;
    String radar;                    // unused (adsb.lol has no radar feed field)
    String aircraft_code;            // ICAO type code (e.g. "B739")
    String registration;             // tail number (e.g. "HS-LVK")
    time_t timestamp;
    String origin_airport;           // IATA code from adsb.lol routeset (e.g. "DMK"); falls back to ICAO/municipality
    String destination_airport;      // IATA code from adsb.lol routeset (e.g. "KKC"); falls back to ICAO/municipality
    bool on_ground;
    int vertical_speed;              // ft/min
    String callsign;                 // raw callsign from adsb.lol (e.g. "TLM646")
    String iata_callsign;            // IATA callsign — adsb.lol routeset has none, so falls back to raw callsign
    String icao_airline;             // airline ICAO code from adsb.lol routeset (e.g. "TLM")
    String airline_name;             // airline name — not provided by adsb.lol routeset (resolved via airlines.txt)

    String toString() const;

    int altitude_metric() const { return altitude * FT_TO_M; }
    int ground_speed_metric() const { return ground_speed * KTS_TO_KMH; }
    int vertical_speed_metric() const { return vertical_speed * FT_TO_M; }

    bool squawk_hijack() const { return squawk == "7500"; }
    bool squawk_radio_failure() const { return squawk == "7600"; }
    bool squawk_emergency() const { return squawk == "7700"; }
};

// Fetches aircraft in a circle around (latitude, longitude).
// range_latitude is the search radius in kilometres (range_longitude ignored).
// Planes are sorted by distance (closest first); the closest aircraft is
// enriched with route/airline data from adsb.lol's /api/0/routeset endpoint.
extern bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude, bool air, bool ground, bool gliders, bool vehicles, std::vector<flight_info> &flights, String &error_message);
extern size_t get_logo(const char *icao_airline, std::vector<uint8_t> &buffer, String &error_message);
extern void png_cache_remove(const char *icao_airline);
