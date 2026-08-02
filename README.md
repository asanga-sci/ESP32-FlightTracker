# ESP32 Flight Radar (CYD Edition)

A real-time flight tracking station designed for the **ESP32-2432S028R** (Cheap Yellow Display). This project fetches live ADS-B data from FlightRadar24 and visualizes aircraft on a radar-style interface.

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

*   Flight data provided via the FlightRadar24 public feed.
*   Airline logos provided by LogoStream.
*   Distance and bearing calculated using the Haversine formula.

---
*Disclaimer: This project is for educational purposes. Ensure you comply with the terms of service of the data providers.*
```