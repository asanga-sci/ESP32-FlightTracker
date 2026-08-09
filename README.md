# ESP32 Flight Tracker

A real-time flight tracking station for **ESP32 boards with a 2.8" 240x320 ILI9341 TFT**. This project fetches live ADS-B data from the **airplanes.live** community feed and visualizes aircraft on a radar-style interface, with optional route/airline enrichment and airline logos.

Two hardware variants are supported:

| Variant | Board | Env | Config |
| :--- | :--- | :--- | :--- |
| **CBD** (Cheap Black Display) — **default env** | ESP32-S3 ES3C28P / Freenove FNK0104A-B | `esp32-s3-es3c28p` | `include/User_Setup_S3.h` |
| **CYD** (Cheap Yellow Display) | ESP32-2432S028R | `esp32-2432S028R` | `include/User_Setup.h` |

## 🚀 Features

*   **Real-time Tracking:** Fetches live flight data including ICAO address, flight number, aircraft type, altitude, speed, and heading.
*   **Open Data Sources:** Live ADS-B positions from **airplanes.live** and aircraft/airline metadata from **adsbdb.com** — no proprietary API keys required. An optional **AirLabs** fallback resolves routes when adsbdb reports an unknown callsign.
*   **Optimized UI:** Built with **LVGL 9.1**, featuring a radar display and detailed flight information panels optimized for the 2.8" TFT.
*   **Dual-Core Architecture:** 
    *   **Core 0:** Dedicated to UI rendering and resistive touch handling.
    *   **Core 1:** Manages WiFi, HTTP requests, JSON parsing, and image decoding.
*   **Web Configuration:** Integrated WiFi Manager and settings portal to configure your GPS coordinates, tracking radius, and optional API keys without reflashing.
*   **Airline Logos:** Dynamically fetches, decodes (LodePNG) and caches airline logos using the LogoStream API (optional) — with retry on transient failures.
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

The **CBD** variant (`esp32-s3-es3c28p`) is the default environment, so a bare `pio run` targets it:

```bash
pio run                          # compile the default env (CBD / esp32-s3-es3c28p)
pio run -e esp32-2432S028R       # compile the CYD variant
pio run -t upload                # build + flash the active variant
pio run -t uploadfs              # flash LittleFS image (airlines.txt, etc.)
```

Notes:
*   The **CYD** variant uses the `huge_app.csv` partition scheme; the **CBD** variant uses `default_16MB.csv` (specified in `platformio.ini`).
*   The CBD variant flashes over native USB CDC at 921600 baud.

### Uploading the Airline Database

1.  Place your `airlines.txt` in the `data` folder.
2.  Use `pio run -t uploadfs` to flash the `LittleFS` partition.

### Capturing a UI Screenshot

The firmware accepts `ss` over the USB/UART serial port. The included helper
requests a screenshot and saves it in the repository's `images` folder. It
supports PNG and BMP output:

```text
pip install pyserial
python scripts/screenshot.py COM7
python scripts/screenshot.py COM7 --format bmp
```

Replace `COM7` with the port assigned to the board. Close PlatformIO's serial
monitor before running the helper. A 240x320 screenshot may take several
seconds to transfer at the firmware's 115200 baud rate.

## ⚙️ Configuration

On first boot, the device will enter **Setup Mode**:

1.  Connect to the WiFi network: `FlightTracker-Setup`.
2.  Navigate to `192.168.4.1` in your browser.
3.  Configure:
    *   **WiFi SSID & Password.**
    *   **Coordinates:** Your Latitude and Longitude.
    *   **Radius:** Tracking range in Kilometers.
    *   **Enable airline logos** — required to use the LogoStream API key field (the key input is greyed out until the checkbox is ticked).
    *   **Enable AirLabs route fallback** — required to use the AirLabs API key field; adds route/airline data when adsbdb can't resolve a callsign.

Once connected, open `http://<device-ip>/settings` to change these later. Saving reboots the device.

## 🏗️ Technical Details

| Component | Core | Description |
| :--- | :--- | :--- |
| `UI_Task` | 0 | Handles the LVGL timer handler and screen updates. |
| `HTTP_Task`| 1 | Periodically fetches flight data and updates the aircraft model. |
| `Logo_Task`| 1 | Background fetching and LodePNG decoding for airline icons. |

## 📜 Data Sources & Credits

This project is only possible thanks to the community that keeps these data sources free and open:

**Flight data**
*   **[airplanes.live](https://airplanes.live)** — open ADS-B aggregator providing live aircraft positions, headings, altitudes and speeds (previously FlightRadar24; the retired FR24 feed is kept in `legacy/` for reference).
*   **[adsbdb.com](https://adsbdb.com)** — open database that resolves aircraft and airline/route metadata from callsigns.

**Optional enrichment & branding**
*   **[AirLabs](https://airlabs.co)** — optional route/airline fallback API used when adsbdb reports an *unknown callsign*. ⚠️ **Not supported on the CYD (`esp32-2432S028R`):** the additional HTTPS/TLS session requires more free RAM than the CYD has available, so enable it only on the CBD (`esp32-s3-es3c28p`, 8 MB PSRAM) variant.
*   **[LogoStream](https://logostream.dev)** — optional airline logo API; logos are decoded with **LodePNG** and cached in RAM with retry-on-failure.

**Libraries**
*   **LVGL** (UI framework), **TFT_eSPI** (ILI9341 driver), **ArduinoJson** (JSON parsing), **LodePNG** (PNG decoding), **Preferences** (NVS configuration storage).

Distance and bearing are calculated using the Haversine formula.

---
*Disclaimer: This project is for educational purposes. Ensure you comply with the terms of service of the data providers.*
