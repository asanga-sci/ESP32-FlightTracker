#pragma once

#include <Arduino.h>
#include <list>
#include <vector>

// Conversions to metric
#define FT_TO_M 0.3048
#define KTS_TO_KMH 1.852f

// Open-data flight model.
// Positions/telemetry come from airplanes.live (community ADS-B aggregator);
// origin/destination municipalities + airline details come from adsbdb.com.
struct flight_info
{
    String icao_address;             // ICAO 24-bit address hex (e.g. "8832CB")
    float latitude;
    float longitude;
    int heading;                     // degrees true
    int altitude;                    // barometric altitude (feet), 0 when on ground
    int ground_speed;                // knots
    String squawk;
    String radar;                    // unused (airplanes.live has no radar feed field)
    String aircraft_code;            // ICAO type code (e.g. "B739")
    String registration;             // tail number (e.g. "HS-LVK")
    time_t timestamp;
    String origin_airport;           // municipality from adsbdb (e.g. "Bangkok")
    String destination_airport;      // municipality from adsbdb (e.g. "Khon Kaen")
    String flight;                   // flight number / callsign (e.g. "TLM646")
    bool on_ground;
    int vertical_speed;              // ft/min
    String call_sign;
    String icao_airline;             // airline ICAO code from adsbdb (e.g. "TLM")
    String airline_name;             // airline name from adsbdb (e.g. "Thai Lion Air")

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
// enriched with route/airline data from adsbdb.com.
extern bool get_flights(float latitude, float longitude, float range_latitude, float range_longitude, bool air, bool ground, bool gliders, bool vehicles, std::vector<flight_info> &flights, String &error_message);
extern size_t get_logo(const char *icao_airline, std::vector<uint8_t> &buffer, String &error_message);
