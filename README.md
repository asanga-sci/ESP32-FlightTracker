# ESP32 Flight Tracker

A real-time flight tracking station for **ESP32 boards with a 2.8" 240x320 ILI9341 TFT**. This project fetches live ADS-B data from FlightRadar24 and visualizes aircraft on a radar-style interface.

Two hardware variants are supported:

| Variant | Board | Env | Config |
| :--- | :--- | :--- | :--- |
| **CYD** (Cheap Yellow Display) | ESP32-2432S028R | `esp32-2432S028R` | `include/User_Setup.h` |
| **CBD** (Cheap Black Display) | ESP32-S3 ES3C28P / Freenove FNK0104A-B | `esp32-s3-es3c28p` | `include/User_Setup_S3.h` |

## 🚀 Features

*   **Real-time Tracking:** Fetches live flight data including ICAO address, flight number, aircraft type, altitude, speed, and heading.
*   **Optimized UI:** Built with **LVGL 9.1**, featuring a radar display and detailed flight information panels optimized for the 2.8" TFT.
*   **Dual-Core Architecture:** 
    *   **Core 0:** Dedicated to UI rendering and resistive touch handling.
    *   **Core 1:** Manages WiFi, HTTP requests, JSON parsing, and image decoding.
*   **Web Configuration:** Integrated WiFi Manager and settings portal to configure your GPS coordinates and tracking radius without reflashing.
*   **Airline Logos:** Dynamically fetches and caches airline logos using the LogoStream API.
*   **Local Database:** Resolves airline ICAO codes to full names using a database stored in `LittleFS`.

## 🛠️ Hardware

*   **CYD:** ESP32-2432S028R (ESP32-WROOM-32) with 2.8" ILI9341 TFT, resistive touch (XPT2046), 4 MB flash.
*   **CBD:** ESP32-S3 ES3C28P (ESP32-S3-WROOM-1) with 2.8" ILI9341 TFT, resistive touch (XPT2046), **16 MB flash + 8 MB OPI PSRAM**, native USB CDC (no UART bridge chip).
*   **Storage:** Internal Flash used via `LittleFS` for configuration and airline data.

## 📦 Software Setup

This project is built using **PlatformIO**.

### Dependencies
*   **LVGL 9.1.0+**
*   **TFT_eSPI** (Pre-configured per board via `include/User_Setup.h` / `include/User_Setup_S3.h`)
*   **ArduinoJson 7.x**
*   **LodePNG** (vendored under `lib/lodepng`)

### Build & Flash

```bash
pio run -e esp32-2432S028R          # compile the CYD variant
pio run -e esp32-s3-es3c28p         # compile the CBD variant
pio run -t upload                   # build + flash the active variant
pio run -t uploadfs                 # flash LittleFS image (airlines.txt, etc.)
```

Notes:
*   The **CYD** variant uses the `huge_app.csv` partition scheme; the **CBD** variant uses `default_16MB.csv` (specified in `platformio.ini`).
*   The CBD variant flashes over native USB CDC at 921600 baud.

### Uploading the Airline Database

1.  Place your `airlines.txt` in the `data` folder.
2.  Use `pio run -t uploadfs` to flash the `LittleFS` partition.

## ⚙️ Configuration

On first boot, the device will enter **Setup Mode**:

1.  Connect to the WiFi network: `FlightRadar-Setup`.
2.  Navigate to `192.168.4.1` in your browser.
3.  Configure:
    *   **WiFi SSID & Password.**
    *   **Coordinates:** Your Latitude and Longitude.
    *   **Radius:** Tracking range in Kilometers.
    *   **API Key:** Your LogoStream key for airline branding.

## 🏗️ Technical Details

| Component | Core | Description |
| :--- | :--- | :--- |
| `UI_Task` | 0 | Handles the LVGL timer handler and screen updates. |
| `HTTP_Task`| 1 | Periodically fetches flight data and updates the aircraft model. |
| `Logo_Task`| 1 | Background fetching and LodePNG decoding for airline icons. |

## 📜 Data Sources & Credits

*   Flight data provided via the FlightRadar24 public feed.
*   Airline logos provided by LogoStream.
*   Distance and bearing calculated using the Haversine formula.

---
*Disclaimer: This project is for educational purposes. Ensure you comply with the terms of service of the data providers.*
