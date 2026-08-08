#include "web_config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

// ─── NVS keys ─────────────────────────────────────────────────────────────────
static constexpr const char* NVS_NS      = "fltcfg";
static constexpr const char* KEY_VALID   = "valid";
static constexpr const char* KEY_SSID    = "ssid";
static constexpr const char* KEY_PASS    = "pass";
static constexpr const char* KEY_LAT     = "lat";
static constexpr const char* KEY_LON     = "lon";
static constexpr const char* KEY_RADIUS  = "radius";
static constexpr const char* LOGO_SUPPORT  = "logoSupport";
static constexpr const char* LOGOSTREAM_KEY  = "logostreamKey";
static constexpr const char* AIRLABS_FALLBACK = "airlabsFallback";
static constexpr const char* AIRLABS_KEY  = "airlabsKey";

// ─── AP defaults ──────────────────────────────────────────────────────────────
static constexpr const char* AP_SSID = "FlightTracker-Setup";
static constexpr const char* AP_PASS = "";           // open — add a password if desired
static const IPAddress AP_IP(192, 168, 4, 1);
static constexpr uint8_t DNS_PORT = 53;

// ─── Module-level state ───────────────────────────────────────────────────────
static TaskHandle_t s_configTaskHandle = nullptr;
static FlightConfig      s_cfg;
static SemaphoreHandle_t s_cfgMutex   = nullptr;
static EventGroupHandle_t s_evtGroup  = nullptr;
static WebServer         s_server(80);
static DNSServer         s_dns;
static Preferences       s_prefs;
static WebConfigAPReadyCb s_apReadyCb  = nullptr;

// ─── HTML (PROGMEM) ───────────────────────────────────────────────────────────

static const char HTML_HEAD[] PROGMEM = R"raw(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flight Tracker Config</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:#0f172a;color:#e2e8f0;min-height:100vh;
     display:flex;align-items:flex-start;justify-content:center;padding:2rem 1rem}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;
      padding:2rem;width:100%;max-width:480px;margin:auto}
h1{font-size:1.25rem;font-weight:600;color:#f8fafc;margin-bottom:.25rem}
.sub{font-size:.8rem;color:#64748b;margin-bottom:1.5rem}
.sec{font-size:.7rem;font-weight:600;letter-spacing:.08em;color:#475569;
     text-transform:uppercase;margin:1.4rem 0 .7rem;
     border-top:1px solid #334155;padding-top:1rem}
label{display:block;font-size:.8rem;color:#94a3b8;margin-bottom:.25rem}
input{width:100%;padding:.55rem .75rem;border-radius:8px;border:1px solid #334155;
      background:#0f172a;color:#f1f5f9;font-size:.9rem;outline:none;
      transition:border-color .15s}
input:focus{border-color:#3b82f6}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}
.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.75rem}
.f{margin-bottom:.85rem}
button{width:100%;padding:.7rem;border-radius:8px;border:none;cursor:pointer;
       font-size:.95rem;font-weight:600;margin-top:.5rem;transition:opacity .15s}
.bs{background:#3b82f6;color:#fff}.bs:hover{opacity:.85}
.br{background:#ef4444;color:#fff;margin-top:.75rem}.br:hover{opacity:.85}
.note{font-size:.75rem;color:#475569;margin-top:1.5rem;text-align:center}
.icon{font-size:1.5rem;margin-bottom:.4rem}
</style></head><body><div class="card">
)raw";

static const char HTML_FOOT[] PROGMEM = R"raw(</div></body></html>)raw";

static const char PAGE_SETUP[] PROGMEM = R"raw(
<div class="icon">✈️</div>
<h1>Flight Tracker Setup</h1>
<p class="sub">Join your network and configure radar coverage.</p>
<form method="POST" action="/save">
<p class="sec">WiFi</p>

<div class="f">
  <label>SSID</label>
  <input type="text"
         name="ssid"
         placeholder="Network name"
         maxlength="63"
         required>
</div>

<div class="f">
  <label>Password</label>
  <input type="password"
         name="pass"
         placeholder="WiFi password"
         maxlength="63">
</div>

<p class="sec">Radar Coverage</p>

<div class="row3">
  <div class="f">
    <label>Latitude</label>
    <input type="text"
           name="lat"
           placeholder="13.7563"
           required>
  </div>

  <div class="f">
    <label>Longitude</label>
    <input type="text"
           name="lon"
           placeholder="100.5018"
           required>
  </div>

  <div class="f">
    <label>Radius (km)</label>
    <input type="number"
           name="radius"
           value="150"
           min="10"
           max="500">
  </div>
</div>

<p class="sec">Airline Logos</p>

<div class="f">
  <label>
    <input type="checkbox"
           name="logoSupport"
           onchange="toggleKey(this,'logostreamKey')">
    Enable airline logos
  </label>
</div>

<div class="f">
  <label>LogoStream API Key</label>
  <input type="text"
         name="logostreamApiKey"
         id="logostreamKey"
         placeholder="Optional API key"
         maxlength="127"
         disabled>
</div>

<p class="sec">Data Sources</p>

<div class="f">
  <label>
    <input type="checkbox"
           name="airlabsFallback"
           onchange="toggleKey(this,'airlabsKey')">
    Enable AirLabs route fallback
  </label>
</div>

<div class="f">
  <label>AirLabs API Key</label>
  <input type="text"
         name="airlabsApiKey"
         id="airlabsKey"
         placeholder="Optional — route fallback"
         maxlength="50"
         disabled>
</div>

<button type="submit" class="bs">
  💾 Save & Connect
</button>
<script>
function toggleKey(cb,id){var e=document.getElementById(id);if(e)e.disabled=!cb.checked;}
</script>
</form>)raw";

// Settings page template — printf tokens in order:
//   ssid, ip, saved_banner, ssid, lat, lon, radius,
//   logoChecked, logoKeyDisabled, logostreamApiKey,
//   airlabsChecked, airlabsKeyDisabled, airlabsApiKey
static const char PAGE_SETTINGS_TPL[] PROGMEM = R"raw(
<div class="icon">⚙️</div>
<h1>Flight Tracker Settings</h1>
<p class="sub">Network: <strong>%s</strong> &mdash; %s</p>
%s
<form method="POST" action="/settings/save">
  <p class="sec">WiFi</p>
  <div class="f"><label>SSID</label>
    <input type="text" name="ssid" value="%s" maxlength="63" required></div>
  <div class="f"><label>Password <span style="font-weight:400;color:#475569">(blank = keep current)</span></label>
    <input type="password" name="pass" placeholder="unchanged" maxlength="63"></div>

  <p class="sec">Radar coverage</p>
  <div class="row3">
    <div class="f"><label>Latitude</label>
      <input type="text" name="lat" value="%.5f" maxlength="12" required></div>
    <div class="f"><label>Longitude</label>
      <input type="text" name="lon" value="%.5f" maxlength="12" required></div>
    <div class="f"><label>Radius km</label>
      <input type="number" name="radius" value="%.1f" min="10" max="500" required></div>
  </div>
  <p class="sec">Airline Logos</p>

  <div class="f">
    <label>
      <input type="checkbox"
            name="logoSupport"
            %s
            onchange="toggleKey(this,'logostreamKey')">
      Enable airline logos
    </label>
  </div>

  <div class="f">
    <label>LogoStream API Key</label>
    <input type="text"
          name="logostreamApiKey"
          id="logostreamKey"
          value="%s"
          %s>
  </div>

  <p class="sec">Data Sources</p>

  <div class="f">
    <label>
      <input type="checkbox"
            name="airlabsFallback"
            %s
            onchange="toggleKey(this,'airlabsKey')">
      Enable AirLabs route fallback
    </label>
  </div>

  <div class="f">
    <label>AirLabs API Key <span style="font-weight:400;color:#475569">(optional — route fallback)</span></label>
    <input type="text"
          name="airlabsApiKey"
          id="airlabsKey"
          value="%s"
          %s>
  </div>

  <button type="submit" class="bs">💾 Save &amp; Reboot</button>
</form>
<script>
function toggleKey(cb,id){var e=document.getElementById(id);if(e)e.disabled=!cb.checked;}
</script>
<form method="POST" action="/reset"
      onsubmit="return confirm('Erase all settings and restart in setup mode?')">
  <button type="submit" class="br">🗑️ Factory Reset</button>
</form>
<p class="note">Device reboots after saving.</p>)raw";

static const char PAGE_SAVED[] PROGMEM = R"raw(
<div class="icon">✅</div>
<h1>Saved</h1>
<p class="sub">Rebooting — close this tab.</p>)raw";

// ─── NVS helpers ──────────────────────────────────────────────────────────────

static bool nvsLoad(FlightConfig& c) {
    s_prefs.begin(NVS_NS, true);
    bool valid = s_prefs.getBool(KEY_VALID, false);
    if (!valid) { s_prefs.end(); return false; }

    s_prefs.getString(KEY_SSID,   c.ssid,        sizeof(c.ssid));
    s_prefs.getString(KEY_PASS,   c.password,     sizeof(c.password));
    c.lat       = s_prefs.getFloat(KEY_LAT, 0.0f);
    c.lon       = s_prefs.getFloat(KEY_LON, 0.0f);
    c.radiusKm  = s_prefs.getFloat(KEY_RADIUS, 150.0f);
    c.logoSupport = s_prefs.getBool(LOGO_SUPPORT, false);
    s_prefs.getString(LOGOSTREAM_KEY, c.logostreamApiKey, sizeof(c.logostreamApiKey));
    c.airlabsFallback = s_prefs.getBool(AIRLABS_FALLBACK, false);
    s_prefs.getString(AIRLABS_KEY, c.airlabsApiKey, sizeof(c.airlabsApiKey));
    s_prefs.end();
    return true;
}

static void nvsSave(const FlightConfig& c) {
    s_prefs.begin(NVS_NS, false);
    s_prefs.putString(KEY_SSID,   c.ssid);
    s_prefs.putString(KEY_PASS,   c.password);
    s_prefs.putFloat(KEY_LAT,     c.lat);
    s_prefs.putFloat(KEY_LON,     c.lon);
    s_prefs.putFloat(KEY_RADIUS,  c.radiusKm);
    s_prefs.putBool(LOGO_SUPPORT, c.logoSupport);
    s_prefs.putString(LOGOSTREAM_KEY, c.logostreamApiKey);
    s_prefs.putBool(AIRLABS_FALLBACK, c.airlabsFallback);
    s_prefs.putString(AIRLABS_KEY, c.airlabsApiKey);
    s_prefs.putBool(KEY_VALID,    true);
    s_prefs.end();
}

// ─── Form helpers ─────────────────────────────────────────────────────────────

static String formArg(const char* name) {
    return s_server.hasArg(name) ? s_server.arg(name) : String();
}

// Parse POST body into dst.  isEdit=true → blank password fields keep existing value.
static void parseForm(FlightConfig& dst, bool isEdit) {
    strncpy(dst.ssid, formArg("ssid").c_str(), sizeof(dst.ssid) - 1);

    String p = formArg("pass");
    if (!isEdit || p.length())
        strncpy(dst.password, p.c_str(), sizeof(dst.password) - 1);

    dst.lat      = formArg("lat").toFloat();
    dst.lon      = formArg("lon").toFloat();
    dst.radiusKm = formArg("radius").toFloat();
    if (dst.radiusKm < 10) dst.radiusKm = 10;

    dst.logoSupport = formArg("logoSupport") == "on";
    dst.airlabsFallback = formArg("airlabsFallback") == "on";

    // Disabled inputs are not submitted by the browser. When a feature is
    // turned off, keep the previously-saved key so re-enabling it restores
    // the value; on a fresh setup (isEdit=false) leave it empty.
    if (dst.logoSupport || !isEdit)
        strncpy(dst.logostreamApiKey, formArg("logostreamApiKey").c_str(), sizeof(dst.logostreamApiKey) - 1);
    if (dst.airlabsFallback || !isEdit)
        strncpy(dst.airlabsApiKey, formArg("airlabsApiKey").c_str(), sizeof(dst.airlabsApiKey) - 1);
}

// ─── Response helpers ─────────────────────────────────────────────────────────

static void sendPage(const char* body_P) {
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "text/html", "");
    s_server.sendContent_P(HTML_HEAD);
    s_server.sendContent_P(body_P);
    s_server.sendContent_P(HTML_FOOT);
}

static void redirect(const char* to) {
    s_server.sendHeader("Location", to, true);
    s_server.send(302, "text/plain", "");
}

// ─── AP-mode handlers ─────────────────────────────────────────────────────────

static void onSetupGet()  { sendPage(PAGE_SETUP); }

static void onSetupPost() {
    FlightConfig newCfg;
    memset(&newCfg, 0, sizeof(newCfg));
    newCfg.refreshSec = 30;
    parseForm(newCfg, false);
    nvsSave(newCfg);
    sendPage(PAGE_SAVED);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
}

// ─── Station-mode handlers ────────────────────────────────────────────────────

static void onSettingsGet() {
    // Take a snapshot under the mutex for rendering
    FlightConfig snap;
    xSemaphoreTake(s_cfgMutex, portMAX_DELAY);
    snap = s_cfg;
    xSemaphoreGive(s_cfgMutex);

    char body[4096];
    snprintf(body, sizeof(body), PAGE_SETTINGS_TPL,
        snap.ssid,
        WiFi.localIP().toString().c_str(),
        "",           // saved-banner slot (empty on GET)
        snap.ssid,
        snap.lat, snap.lon, snap.radiusKm,
        snap.logoSupport ? "checked" : "",
        snap.logoSupport ? "" : "disabled",
        snap.logostreamApiKey,
        snap.airlabsFallback ? "checked" : "",
        snap.airlabsFallback ? "" : "disabled",
        snap.airlabsApiKey);

    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "text/html", "");
    s_server.sendContent_P(HTML_HEAD);
    s_server.sendContent(body);
    s_server.sendContent_P(HTML_FOOT);
}

static void onSettingsPost() {
    // Work on a mutable copy
    FlightConfig updated;
    xSemaphoreTake(s_cfgMutex, portMAX_DELAY);
    updated = s_cfg;
    xSemaphoreGive(s_cfgMutex);

    parseForm(updated, true);   // blank password = keep existing in updated

    // Restore existing passwords from NVS if left blank
    String newPass   = formArg("pass");
    String newOsPass = formArg("osPass");
    if (newPass.length() == 0 || newOsPass.length() == 0) {
        FlightConfig saved;
        memset(&saved, 0, sizeof(saved));
        nvsLoad(saved);
        if (newPass.length()   == 0) strncpy(updated.password,    saved.password,    sizeof(updated.password)    - 1);
    }

    nvsSave(updated);

    xSemaphoreTake(s_cfgMutex, portMAX_DELAY);
    s_cfg = updated;
    xSemaphoreGive(s_cfgMutex);

    xEventGroupSetBits(s_evtGroup, CFG_EVT_CHANGED);

    sendPage(PAGE_SAVED);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
}

static void onReset() { webConfigReset(); }

// ─── AP mode (runs until form submitted) ─────────────────────────────────────

static void runAPMode() {
    log_i("[WebConfig] No config — starting AP: %s", AP_SSID);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS[0] ? AP_PASS : nullptr);

    s_dns.start(DNS_PORT, "*", AP_IP);

    s_server.on("/",     HTTP_GET,  onSetupGet);
    s_server.on("/save", HTTP_POST, onSetupPost);
    s_server.onNotFound([]() { redirect("/"); });
    s_server.begin();

    log_i("[WebConfig] AP IP: %s", WiFi.softAPIP().toString().c_str());

    if (s_apReadyCb)
    {
      s_apReadyCb(AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    }

    // Loop until onSetupPost() reboots the device
    while (true) {
        s_dns.processNextRequest();
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── WiFi connection (with retry) ────────────────────────────────────────────

static bool connectWiFi(const FlightConfig& c, uint32_t timeoutMs = 15000) {
    log_i("[WebConfig] Connecting to %s …", c.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(c.ssid, c.password);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    log_i("[WebConfig] Connected. IP: %s", WiFi.localIP().toString().c_str());
    return true;
}

// ─── Station-mode server setup ────────────────────────────────────────────────

static void startSettingsServer() {
    s_server.on("/",              HTTP_GET,  []() { redirect("/settings"); });
    s_server.on("/settings",      HTTP_GET,  onSettingsGet);
    s_server.on("/settings/save", HTTP_POST, onSettingsPost);
    s_server.on("/reset",         HTTP_POST, onReset);
    s_server.begin();
    log_i("[WebConfig] Settings at http://%s/settings", WiFi.localIP().toString().c_str());
}

// ─── FreeRTOS task ────────────────────────────────────────────────────────────

static void configTask(void* /*pvParams*/) {
    // 1. Load NVS
    bool valid = nvsLoad(s_cfg);

    if (!valid) {
        // 2a. First boot — block in AP mode until the user configures and saves.
        //     onSetupPost() will ESP.restart() so we never return from runAPMode().
        runAPMode();
    }

    // 2b. Have config — connect to WiFi
    if (!connectWiFi(s_cfg)) {
        log_w("[WebConfig] WiFi timeout — falling back to AP mode");
        memset(&s_cfg, 0, sizeof(s_cfg));
        runAPMode();
    }

    // 3. Publish that we're ready
    xEventGroupSetBits(s_evtGroup, CFG_EVT_READY);
    vTaskDelete(NULL); // delete self — we don't need to run a server on this task, just needed to manage config and WiFi setup

    // 4. Start the settings web server
    // startSettingsServer();

    // 5. Service HTTP clients forever
    // while (true) {
    //     s_server.handleClient();
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
}

// ─── Public API ───────────────────────────────────────────────────────────────

EventGroupHandle_t webConfigInit(BaseType_t coreID, WebConfigAPReadyCb apReadyCb) {
    s_cfgMutex = xSemaphoreCreateMutex();
    s_evtGroup = xEventGroupCreate();
    s_apReadyCb = apReadyCb;
    memset(&s_cfg, 0, sizeof(s_cfg));

    xTaskCreatePinnedToCore(
        configTask,
        "webConfig",
        8192,
        nullptr,
        8,
        &s_configTaskHandle,
        coreID
    );

    return s_evtGroup;
}

FlightConfig webConfigGet() {
    FlightConfig snap;
    xSemaphoreTake(s_cfgMutex, portMAX_DELAY);
    snap = s_cfg;
    xSemaphoreGive(s_cfgMutex);
    return snap;
}

void webConfigReset() {
    s_prefs.begin(NVS_NS, false);
    s_prefs.clear();
    s_prefs.end();
    log_i("[WebConfig] NVS cleared — rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP.restart();
}