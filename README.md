# ESP32 Flight Radar (CYD Edition)

A real-time flight tracking station designed for the **ESP32-2432S028R** (Cheap Yellow Display). This project fetches live ADS-B data and visualizes aircraft on a radar-style interface.

> **Data source migration:** We are moving away from the proprietary FlightRadar24 feed toward fully open, community-driven sources — [airplanes.live](https://airplanes.live) for live flight data and [adsbdb.com](https://adsbdb.com) for aircraft and airline metadata.

## 🚀 Features

*   **Real-time Tracking:** Fetches live flight data including ICAO address, flight number, aircraft type, altitude, speed, and heading.
*   **Open Data Sources:** Live ADS-B positions from **airplanes.live** and aircraft/airline metadata from **adsbdb.com** — no proprietary API keys required.
*   **Optimized UI:** Built with **LVGL 9.1**, featuring a radar display and detailed flight information panels optimized for the 2.8" TFT.
*   **Dual-Core Architecture:** 
    *   **Core 0:** Dedicated to UI rendering and resistive touch handling.
    *   **Core 1:** Manages WiFi, HTTP requests, JSON parsing, and image decoding.
*   **Web Configuration:** Integrated WiFi Manager and settings portal to configure your GPS coordinates and tracking radius without reflashing.
*   **Airline Logos:** Dynamically fetches and caches airline logos using the LogoStream API (optional).
*   **Local Database:** Resolves airline ICAO codes to full names using a database stored in `LittleFS`.

## 🛠️ Hardware

This project is pre-configured for the **ESP32-2432S028R**:
*   **Display:** 2.8" 240x320 TFT (ILI9341).
*   **Touch:** Resistive touch (XPT2046).
*   **Storage:** Internal Flash used via `LittleFS` for configuration and airline data.

## 📦 Software Setup

This project is built using **PlatformIO**.

### Dependencies
*   **LVGL 9.1.0+**
*   **TFT_eSPI** (Pre-configured for CYD via `include/User_Setup.h`)
*   **ArduinoJson 7.x**
*   **LodePNG**

### Installation

1.  **Clone the repository.**
2.  **Upload Airline Database:** 
    *   Place your `airlines.txt` in the `data` folder.
    *   Use the PlatformIO "Upload Filesystem Image" task to flash the `LittleFS` partition.
3.  **Partition Scheme:** 
    *   The project requires the `huge_app.csv` scheme (specified in `platformio.ini`) to accommodate LVGL and the firmware.
4.  **Flash:**
    *   Build and upload the project to your ESP32.

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

*   Flight data provided by the open-source **airplanes.live** ADS-B aggregator.
*   Aircraft and airline metadata resolved via **adsbdb.com**.
*   Airline logos provided by LogoStream (optional).
*   Distance and bearing calculated using the Haversine formula.

---
*Disclaimer: This project is for educational purposes. Data from airplanes.live and adsbdb.com is open and freely licensed; ensure you comply with their respective usage policies.*