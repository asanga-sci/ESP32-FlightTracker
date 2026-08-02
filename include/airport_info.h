#pragma once

#include <Arduino.h>
#include <vector>

struct runway_info
{
    char ident[16];
    float heading_deg = 0.0f;
    float length_ft = 0.0f;
    float width_ft = 0.0f;
};

struct airport_runway_info
{
    char icao[16];
    char name[48];
    float latitude = 0.0f;
    float longitude = 0.0f;
    std::vector<runway_info> runways;
};

bool get_nearby_airports_and_runways(float latitude, float longitude, float distance_km,
                                    std::vector<airport_runway_info> &airports,
                                    String &error_message);
