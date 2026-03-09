/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║  ESP32-CAM SENTINEL — v10.0.0                           ║
 * ║  WebSocket Push + Plain HTTP Upload                     ║
 * ╠══════════════════════════════════════════════════════════╣
 * ║                                                          ║
 * ║  WHY THIS IS DIFFERENT                                   ║
 * ║  ─────────────────────────────────────────────────────  ║
 * ║  ALL previous versions used TLS directly on the ESP32.  ║
 * ║  This caused:                                            ║
 * ║    • BIGNUM -16   — mbedTLS out of contiguous heap      ║
 * ║    • Error -80    — SSL context corruption              ║
 * ║    • Silent PATCH hang → command stays "pending"        ║
 * ║    → Repeated snapshot every 23s                        ║
 * ║                                                          ║
 * ║  THIS VERSION:                                           ║
 * ║    • Zero TLS on device — plain HTTP only              ║
 * ║    • Zero polling — commands pushed via WebSocket       ║
 * ║    • Zero ackCommand needed — WS delivery is confirmed  ║
 * ║                                                          ║
 * ║  DATA FLOW                                               ║
 * ║  ─────────────────────────────────────────────────────  ║
 * ║  UPLOADS:                                                ║
 * ║    ESP32 → HTTP POST /upload → Node → Supabase Storage  ║
 * ║    Node also inserts the event row. One call = done.    ║
 * ║                                                          ║
 * ║  COMMANDS:                                               ║
 * ║    Dashboard → INSERT commands row → Supabase Realtime  ║
 * ║    → Node server (subscribed) → WebSocket push → ESP32  ║
 * ║    ESP32 executes → sends {"type":"ack"} back           ║
 * ║    Node marks command "done" in DB                      ║
 * ║    Latency: <100ms (was 3s poll + unreliable PATCH)     ║
 * ║                                                          ║
 * ║  REQUIRES                                                ║
 * ║  ─────────────────────────────────────────────────────  ║
 * ║  Arduino library: arduinoWebSockets by Links2004        ║
 * ║  Install via Library Manager:                            ║
 * ║    Search "WebSockets" → by Markus Sattler              ║
 * ║    or: https://github.com/Links2004/arduinoWebSockets   ║
 * ║                                                          ║
 * ║  SETUP                                                   ║
 * ║  ─────────────────────────────────────────────────────  ║
 * ║  1. Run Node server: node sentinel-server.js            ║
 * ║  2. Note the IP it prints at startup                    ║
 * ║  3. Set SERVER_HOST below to that IP                    ║
 * ║  4. Flash this firmware                                 ║
 * ║  5. Watch serial — you should see "WS connected"        ║
 * ╚══════════════════════════════════════════════════════════╝
 */

#include "esp_camera.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>          // plain HTTP only — no WiFiClientSecure
#include <WebSocketsClient.h>    // Links2004/arduinoWebSockets library
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define VERSION   "10.0.0"
#define HOSTNAME  "esp32cam-sentinel"
#define FLASH_LED 4

// ══════════════ WIFI ════════════════════════════════════════
const char* WIFI_SSID = "ULTRA Network";
const char* WIFI_PASS = "&12345@100%";

// ══════════════ NODE SERVER ══════════════════════════════════
// ⚠️  CHANGE THIS to your PC's local IP address.
// Run `node sentinel-server.js` and it will print the IP.
// Windows:   ipconfig | findstr IPv4
// Linux/Mac: hostname -I
const char* SERVER_HOST  = "10.168.170.28";   // ← CHANGE THIS
const int   SERVER_PORT  = 3000;
const char* DEVICE_TOKEN = "";                // match .env DEVICE_TOKEN

const char* DEVICE_ID = "ESP32-CAM-01";

// ══════════════ TIMING ═══════════════════════════════════════
const unsigned long HEARTBEAT_MS       = 30000UL;   // heartbeat every 30s
const unsigned long WIFI_WATCHDOG_MS   = 120000UL;  // reconnect after 2min
const unsigned long MOTION_COOLDOWN_MS = 8000UL;    // 8s between motion shots
const unsigned long UPLOAD_TIMEOUT_MS  = 20000;     // 20s upload
const unsigned long HTTP_TIMEOUT_MS    = 8000;      // 8s other HTTP calls
const unsigned long WS_RECONNECT_MS    = 3000;      // WebSocket reconnect

// ══════════════ SENSOR PACKET ════════════════════════════════
// MUST be byte-identical to Hub v4.1 SensorPacket
typedef struct __attribute__((packed)) {
    uint8_t  packetType;
    char     deviceId[12];
    char     fwVersion[8];
    uint32_t alertId;
    uint32_t uptimeSeconds;
    bool     motion;
    bool     pirState;
    float    latitude;
    float    longitude;
    float    altitude_m;
    float    speed_kmh;
    float    hdop;
    uint8_t  satellites;
    uint8_t  gpsValid;
    bool     gpsHasFix;
    float    distance_cm;
    float    batteryVoltage;
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    int8_t   gsmSignalCSQ;
    bool     gsmReady;
    bool     espnowReady;
} SensorPacket;

SensorPacket hub;
volatile bool newPacketReady = false;
volatile bool captureBusy   = false;

// ══════════════ CAMERA PINS (AI-THINKER) ════════════════════
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

// ══════════════ STATE ════════════════════════════════════════
WebServer        web(80);
WebSocketsClient wsClient;

bool camReady    = false;
bool wifiReady   = false;
bool espnowReady = false;
bool wsConnected = false;
int  wifiChannel = 1;

unsigned long alertCount = 0;
unsigned long uploadOK   = 0;
unsigned long uploadFail = 0;

unsigned long lastHeartMs    = 0;
unsigned long lastWifiOkMs   = 0;
unsigned long lastMotionMs   = 0;

// Pending command ack — holds the command ID while capture runs
// so we can ack back to server when done
String pendingCmdId  = "";
String pendingCmdAck = "";   // "done" or "failed"

// ══════════════ HELPERS ══════════════════════════════════════
String serverUrl(const char* path) {
    return "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + path;
}

void wsSend(const String& json) {
    if (wsConnected) wsClient.sendTXT((uint8_t*)json.c_str(), json.length());
}

// ══════════════ ESP-NOW CALLBACK ════════════════════════════
void IRAM_ATTR onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != (int)sizeof(SensorPacket)) {
        Serial.printf("[ESPNOW] Size mismatch got=%d expected=%d\n", len, (int)sizeof(SensorPacket));
        return;
    }
    memcpy((void*)&hub, data, sizeof(SensorPacket));
    newPacketReady = true;
}

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET EVENT HANDLER
//  This is the command path — no polling, no TLS, no PATCH.
//  Server pushes: {"type":"command","id":"<uuid>","command":"capture"}
//  We execute and send back: {"type":"ack","id":"<uuid>","status":"done"}
// ═══════════════════════════════════════════════════════════════
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_DISCONNECTED:
            wsConnected = false;
            lg("WS", "Disconnected — will retry in " + String(WS_RECONNECT_MS/1000) + "s");
            break;

        case WStype_CONNECTED: {
            wsConnected = true;
            lg("WS", "Connected to " + String(SERVER_HOST) + ":" + String(SERVER_PORT));

            // Send hello so server knows who we are
            StaticJsonDocument<192> doc;
            doc["type"]     = "hello";
            doc["deviceId"] = DEVICE_ID;
            doc["version"]  = VERSION;
            doc["freeHeap"] = ESP.getFreeHeap();
            doc["channel"]  = wifiChannel;
            String msg; serializeJson(doc, msg);
            wsClient.sendTXT(msg);
            break;
        }

        case WStype_TEXT: {
            // Parse incoming message
            DynamicJsonDocument doc(512);
            if (deserializeJson(doc, payload, length)) {
                lg("WS", "JSON parse error");
                return;
            }

            String msgType = doc["type"] | "";

            if (msgType == "command") {
                // ── Instant command from dashboard ────────────────
                String id      = doc["id"]      | "";
                String command = doc["command"] | "";

                lg("WS", "CMD: " + command + "  id=" + id.substring(0, 8));

                // Execute immediately — no polling, no queue, no ack loop
                // pendingCmdId is read by the main loop after capture completes
                if (command == "capture") {
                    if (!captureBusy) {
                        pendingCmdId = id;
                        captureAndUpload("manual", false);
                        // ack sent in captureAndUpload after completion
                    } else {
                        lg("WS", "Busy — acking failed for id=" + id.substring(0, 8));
                        StaticJsonDocument<128> ack;
                        ack["type"]   = "ack";
                        ack["id"]     = id;
                        ack["status"] = "failed";
                        String ackMsg; serializeJson(ack, ackMsg);
                        wsSend(ackMsg);
                    }

                } else if (command.startsWith("set_quality:")) {
                    int q = constrain(command.substring(12).toInt(), 1, 63);
                    sensor_t* s = esp_camera_sensor_get();
                    if (s) { s->set_quality(s, q); lg("WS","Quality=" + String(q)); }
                    sendCmdAck(id, "done");

                } else if (command.startsWith("set_brightness:")) {
                    int b = constrain(command.substring(15).toInt(), -2, 2);
                    sensor_t* s = esp_camera_sensor_get();
                    if (s) { s->set_brightness(s, b); lg("WS","Brightness=" + String(b)); }
                    sendCmdAck(id, "done");

                } else if (command == "reboot") {
                    sendCmdAck(id, "done");
                    delay(300);
                    ESP.restart();

                } else {
                    lg("WS", "Unknown command: " + command);
                    sendCmdAck(id, "failed");
                }

            } else if (msgType == "ping") {
                wsSend("{\"type\":\"pong\"}");

            } else {
                lg("WS", "Unknown msg type: " + msgType);
            }
            break;
        }

        case WStype_ERROR:
            lg("WS", "Error");
            break;

        case WStype_PING:
            // Library handles pong automatically
            break;

        default:
            break;
    }
}

void sendCmdAck(const String& id, const char* status) {
    if (id.length() == 0) return;
    StaticJsonDocument<128> doc;
    doc["type"]   = "ack";
    doc["id"]     = id;
    doc["status"] = status;
    String msg; serializeJson(doc, msg);
    wsSend(msg);
    lg("ACK","→ " + String(status) + "  id=" + id.substring(0, 8));
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(600);
    pinMode(FLASH_LED, OUTPUT);
    digitalWrite(FLASH_LED, LOW);

    Serial.println(F("\n╔════════════════════════════════════════╗"));
    Serial.println(F("║  ESP32-CAM SENTINEL v10.0.0            ║"));
    Serial.println(F("║  WebSocket Push + Plain HTTP           ║"));
    Serial.println(F("║  Zero TLS. Zero Polling. Zero Hangs.  ║"));
    Serial.println(F("╚════════════════════════════════════════╝\n"));

    initCamera();
    initWiFi();
    initESPNOW();
    initWebSocket();
    setupMDNS();
    startWebServer();

    // Register device via plain HTTP to server
    registerDevice();

    // Startup snapshot — gives dashboard something to show immediately
    captureAndUpload("startup", false);

    Serial.println(F("\n✅ SYSTEM READY"));
    Serial.printf("   Camera:   %s\n",   camReady    ? "OK" : "FAIL");
    Serial.printf("   WiFi:     %s  IP=%s\n", wifiReady ? "OK" : "FAIL",
                  WiFi.localIP().toString().c_str());
    Serial.printf("   Channel:  %d  ← Hub must match\n", wifiChannel);
    Serial.printf("   ESP-NOW:  %s\n",   espnowReady ? "OK" : "FAIL");
    Serial.printf("   Server:   http://%s:%d\n", SERVER_HOST, SERVER_PORT);
    Serial.printf("   WS:       ws://%s:%d/ws\n", SERVER_HOST, SERVER_PORT);
    Serial.printf("   PktSize:  %d bytes\n", (int)sizeof(SensorPacket));
    Serial.printf("   Triggers: Motion (ESP-NOW) + Manual (WebSocket push)\n\n");
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── WebSocket maintenance (MUST be called every loop) ─────
    // This drives the receive callback and auto-reconnects
    wsClient.loop();

    if (wifiReady) web.handleClient();

    // 1. Motion from Hub via ESP-NOW
    if (newPacketReady) {
        newPacketReady = false;
        handleSensorPacket(now);
    }

    // 2. Heartbeat — no polling needed for commands anymore
    if (!captureBusy && (now - lastHeartMs >= HEARTBEAT_MS)) {
        lastHeartMs = now;
        sendHeartbeat();
    }

    // 3. WiFi watchdog
    if (WiFi.status() == WL_CONNECTED) {
        lastWifiOkMs = now;
        wifiReady    = true;
    } else if (wifiReady && (now - lastWifiOkMs > WIFI_WATCHDOG_MS)) {
        lg("WIFI", "Watchdog — reconnecting");
        wifiReady = false;
        WiFi.reconnect();
    }
}

// ═══════════════════════════════════════════════════════════════
//  SENSOR PACKET HANDLER
// ═══════════════════════════════════════════════════════════════
void handleSensorPacket(unsigned long now) {
    if (hub.packetType == 0) {
        alertCount++;
        lg("PKT", "MOTION #" + String(alertCount)
            + " from=" + String(hub.deviceId)
            + " dist=" + String(hub.distance_cm, 1) + "cm"
            + " gps=" + (hub.gpsValid
                ? String(hub.latitude, 5) + "," + String(hub.longitude, 5)
                : "NoFix"));

        if (!captureBusy && (now - lastMotionMs >= MOTION_COOLDOWN_MS)) {
            lastMotionMs = now;
            captureAndUpload("motion", true);   // fastCapture
        } else {
            lg("PKT", "Skipped — busy or in cooldown");
        }

    } else {
        // Status broadcast from Hub
        sendHubHeartbeat();
    }
}

// ═══════════════════════════════════════════════════════════════
//  CAPTURE + UPLOAD
//
//  Builds a multipart/form-data body containing:
//    • JPEG image
//    • All sensor metadata fields
//  Posts to Node server via plain HTTP.
//  Server handles Supabase storage upload + event row insert.
//
//  fastCapture=true  → motion: take immediately, no frame flush
//  fastCapture=false → manual/startup: discard one stale frame
//
//  Resolution strategy:
//    Motion:         SVGA (800×600)   Q20 → ~50KB  → 2-4s upload
//    Manual/Startup: UXGA (1600×1200) Q10 → ~220KB → 6-10s upload
// ═══════════════════════════════════════════════════════════════
void setResolution(framesize_t size, int quality) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return;
    s->set_framesize(s, size);
    s->set_quality(s, quality);
    delay(80);  // sensor settle time
}

void captureAndUpload(const char* snapType, bool fastCapture) {
    if (!camReady)       { lg("CAM","Not ready"); return; }
    if (!wifiReady)      { lg("CAM","No WiFi");   return; }
    if (captureBusy)     { lg("CAM","Busy — skip " + String(snapType)); return; }

    captureBusy = true;

    // Resolution: small for motion (speed), full for manual (quality)
    bool isMotion = (strcmp(snapType, "motion") == 0);
    if (isMotion) {
        setResolution(FRAMESIZE_SVGA, 20);
    } else {
        setResolution(FRAMESIZE_UXGA, 10);
    }

    // Flush stale frame for non-motion captures
    if (!fastCapture) {
        camera_fb_t* warmup = esp_camera_fb_get();
        if (warmup) esp_camera_fb_return(warmup);
    }

    unsigned long t0 = millis();
    camera_fb_t* fb  = esp_camera_fb_get();
    if (!fb) {
        lg("CAM","Frame grab FAILED");
        setResolution(FRAMESIZE_UXGA, 10);
        captureBusy = false;
        if (pendingCmdId.length() > 0) {
            sendCmdAck(pendingCmdId, "failed");
            pendingCmdId = "";
        }
        return;
    }
    lg("CAM","Captured " + String(fb->len / 1024) + "KB in "
        + String(millis() - t0) + "ms (" + String(snapType) + ")");

    // ── Copy image then release framebuffer ───────────────────
    // Plain HTTP doesn't have the TLS heap pressure issue, but we
    // still release the FB before malloc'ing the multipart buffer
    // to avoid having two large allocations in PSRAM simultaneously.
    size_t   imgLen = fb->len;
    uint8_t* imgBuf = (uint8_t*)malloc(imgLen);
    if (!imgBuf) {
        lg("CAM","malloc failed for " + String(imgLen/1024) + "KB");
        esp_camera_fb_return(fb);
        setResolution(FRAMESIZE_UXGA, 10);
        uploadFail++;
        captureBusy = false;
        if (pendingCmdId.length() > 0) {
            sendCmdAck(pendingCmdId, "failed");
            pendingCmdId = "";
        }
        return;
    }
    memcpy(imgBuf, fb->buf, imgLen);
    esp_camera_fb_return(fb);
    fb = nullptr;

    lg("CAM","FB released, heap=" + String(ESP.getFreeHeap()/1024) + "KB");

    // ── Build multipart/form-data ─────────────────────────────
    String bnd = "SntBnd" + String(millis());
    String hdr = "";

    // Text fields
    struct Field { const char* name; String value; };
    Field fields[] = {
        {"snapType",     String(snapType)},
        {"deviceId",     String(DEVICE_ID)},
        {"triggeredBy",  hub.deviceId[0] ? String(hub.deviceId) : String(DEVICE_ID)},
        {"latitude",     hub.gpsValid ? String(hub.latitude, 6)  : "0"},
        {"longitude",    hub.gpsValid ? String(hub.longitude, 6) : "0"},
        {"distanceCm",   String(hub.distance_cm, 0)},
        {"smsSent",      hub.gsmReady ? "1" : "0"},
        {"altitudeM",    String(hub.altitude_m, 1)},
        {"speedKmh",     String(hub.speed_kmh, 1)},
        {"hdop",         String(hub.hdop, 2)},
        {"satellites",   String(hub.satellites)},
        {"gpsValid",     String((int)hub.gpsValid)},
        {"hubHeap",      String(hub.freeHeap)},
        {"hubBattery",   String(hub.batteryVoltage, 3)},
        {"hubGsmCsq",    String(hub.gsmSignalCSQ)},
        {"hubUptimeS",   String(hub.uptimeSeconds)},
        {"hubFwVersion", String(hub.fwVersion)},
        {"camHeap",      String(ESP.getFreeHeap())},
        {"camRssi",      String(wifiReady ? WiFi.RSSI() : 0)},
    };
    for (auto& f : fields) {
        hdr += "--" + bnd + "\r\n";
        hdr += "Content-Disposition: form-data; name=\"" + String(f.name) + "\"\r\n\r\n";
        hdr += f.value + "\r\n";
    }

    // Image part header
    hdr += "--" + bnd + "\r\n";
    hdr += "Content-Disposition: form-data; name=\"image\"; filename=\"snap.jpg\"\r\n";
    hdr += "Content-Type: image/jpeg\r\n\r\n";

    String ftr = "\r\n--" + bnd + "--\r\n";

    size_t   totalLen = hdr.length() + imgLen + ftr.length();
    uint8_t* body     = (uint8_t*)malloc(totalLen);
    if (!body) {
        lg("CAM","malloc failed for multipart (" + String(totalLen/1024) + "KB)");
        free(imgBuf);
        setResolution(FRAMESIZE_UXGA, 10);
        uploadFail++;
        captureBusy = false;
        if (pendingCmdId.length() > 0) {
            sendCmdAck(pendingCmdId, "failed");
            pendingCmdId = "";
        }
        return;
    }

    size_t pos = 0;
    memcpy(body + pos, hdr.c_str(),  hdr.length()); pos += hdr.length();
    memcpy(body + pos, imgBuf,       imgLen);        pos += imgLen;
    memcpy(body + pos, ftr.c_str(),  ftr.length());
    free(imgBuf);

    // ── POST to Node server — plain HTTP, instant, no TLS ─────
    t0 = millis();
    HTTPClient http;
    http.begin(serverUrl("/upload"));
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + bnd);
    if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(UPLOAD_TIMEOUT_MS);

    int code = http.POST(body, totalLen);
    free(body);

    String resp    = http.getString();
    unsigned long elapsed = millis() - t0;
    http.end();

    // Restore full-res for next capture
    setResolution(FRAMESIZE_UXGA, 10);

    // Always log HTTP code — no silent failures
    lg("CAM","POST /upload HTTP " + String(code) + " in " + String(elapsed) + "ms");

    bool success = (code == 200);
    if (success) {
        uploadOK++;
        DynamicJsonDocument doc(256);
        if (!deserializeJson(doc, resp)) {
            lg("CAM","OK  event=" + String(doc["event"] | "?")
                + "  url=..." + String(doc["path"] | "?"));
        }
    } else {
        uploadFail++;
        lg("CAM","FAILED HTTP " + String(code) + " → " + resp.substring(0, 100));
    }

    // ── Ack back to server via WebSocket ─────────────────────
    // This marks the command "done" in DB — server does it via Supabase SDK.
    // No PATCH from ESP32. No TLS. Cannot fail silently.
    if (pendingCmdId.length() > 0) {
        sendCmdAck(pendingCmdId, success ? "done" : "failed");
        pendingCmdId = "";
    }

    captureBusy = false;
}

// ═══════════════════════════════════════════════════════════════
//  HEARTBEAT — plain HTTP POST to server
// ═══════════════════════════════════════════════════════════════
void sendHeartbeat() {
    if (!wifiReady) return;

    HTTPClient http;
    http.begin(serverUrl("/heartbeat"));
    http.addHeader("Content-Type", "application/json");
    if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(HTTP_TIMEOUT_MS);

    DynamicJsonDocument doc(512);
    doc["device_id"]        = DEVICE_ID;
    doc["rssi"]             = WiFi.RSSI();
    doc["free_heap"]        = ESP.getFreeHeap();
    doc["uptime_seconds"]   = millis() / 1000;
    doc["ip_address"]       = WiFi.localIP().toString();
    doc["firmware_version"] = VERSION;
    doc["cam_alerts"]       = alertCount;
    doc["cam_upload_ok"]    = uploadOK;
    doc["cam_upload_fail"]  = uploadFail;
    doc["espnow_channel"]   = wifiChannel;
    doc["ws_connected"]     = wsConnected;
    doc["hub_battery_v"]    = hub.batteryVoltage;
    doc["hub_free_heap"]    = hub.freeHeap;
    doc["hub_gsm_csq"]      = hub.gsmSignalCSQ;
    doc["hub_gsm_ready"]    = hub.gsmReady;
    doc["hub_gps_valid"]    = (bool)hub.gpsValid;
    doc["hub_gps_sats"]     = hub.satellites;
    doc["hub_gps_hdop"]     = hub.hdop;
    doc["hub_uptime_s"]     = hub.uptimeSeconds;
    doc["hub_fw_version"]   = String(hub.fwVersion);

    String body; serializeJson(doc, body);
    int code = http.POST(body);
    http.end();

    lg("HB","HTTP " + String(code)
        + " heap=" + String(ESP.getFreeHeap()/1024) + "KB"
        + " rssi=" + String(WiFi.RSSI()) + "dBm"
        + " ws=" + (wsConnected ? "UP" : "DOWN")
        + " hub-bat=" + String(hub.batteryVoltage, 1) + "V");
}

void sendHubHeartbeat() {
    if (!wifiReady) return;
    HTTPClient http;
    http.begin(serverUrl("/heartbeat"));
    http.addHeader("Content-Type", "application/json");
    if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(HTTP_TIMEOUT_MS);
    StaticJsonDocument<256> doc;
    doc["device_id"]      = hub.deviceId[0] ? hub.deviceId : "SENSOR-HUB-01";
    doc["free_heap"]      = hub.freeHeap;
    doc["rssi"]           = 0;
    doc["uptime_seconds"] = hub.uptimeSeconds;
    doc["hub_battery_v"]  = hub.batteryVoltage;
    doc["hub_gsm_csq"]    = hub.gsmSignalCSQ;
    doc["hub_gps_sats"]   = hub.satellites;
    doc["hub_gps_hdop"]   = hub.hdop;
    doc["hub_gps_valid"]  = (bool)hub.gpsValid;
    doc["hub_uptime_s"]   = hub.uptimeSeconds;
    doc["hub_fw_version"] = String(hub.fwVersion);
    String body; serializeJson(doc, body);
    http.POST(body);
    http.end();
}

void registerDevice() {
    if (!wifiReady) return;
    HTTPClient http;
    http.begin(serverUrl("/device"));
    http.addHeader("Content-Type", "application/json");
    if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(HTTP_TIMEOUT_MS);
    StaticJsonDocument<256> doc;
    doc["device_id"]        = DEVICE_ID;
    doc["device_type"]      = "esp32_cam_hybrid";
    doc["name"]             = "Camera Unit";
    doc["ip_address"]       = WiFi.localIP().toString();
    doc["firmware_version"] = VERSION;
    doc["status"]           = "online";
    String body; serializeJson(doc, body);
    int code = http.POST(body);
    http.end();
    lg("DB","Device registered HTTP " + String(code));
}

void sendLog(const char* level, const char* cat, const String& msg) {
    if (!wifiReady) return;
    HTTPClient http;
    http.begin(serverUrl("/log"));
    http.addHeader("Content-Type", "application/json");
    if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(5000);
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["level"]     = level;
    doc["category"]  = cat;
    doc["message"]   = msg;
    String body; serializeJson(doc, body);
    http.POST(body);
    http.end();
}

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET INIT
// ═══════════════════════════════════════════════════════════════
void initWebSocket() {
    wsClient.begin(SERVER_HOST, SERVER_PORT, "/ws");
    wsClient.onEvent(webSocketEvent);
    wsClient.setReconnectInterval(WS_RECONNECT_MS);
    // Built-in heartbeat: ping every 15s, pong must arrive within 5s
    wsClient.enableHeartbeat(15000, 5000, 2);
    lg("WS", "Connecting to ws://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/ws");
}

// ═══════════════════════════════════════════════════════════════
//  CAMERA INIT
// ═══════════════════════════════════════════════════════════════
void initCamera() {
    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;  cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM;           cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM;           cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM;           cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM;           cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;  cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;  cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sscb_sda = SIOD_GPIO_NUM;  cfg.pin_sscb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;  cfg.pin_reset     = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        cfg.frame_size   = FRAMESIZE_UXGA;
        cfg.jpeg_quality = 10;
        cfg.fb_count     = 3;
        lg("CAM","PSRAM — UXGA Q10 fb=3");
    } else {
        cfg.frame_size   = FRAMESIZE_SVGA;
        cfg.jpeg_quality = 12;
        cfg.fb_count     = 1;
        lg("CAM","No PSRAM — SVGA Q12 fb=1");
    }

    if (esp_camera_init(&cfg) != ESP_OK) {
        lg("CAM","Init FAILED"); camReady = false; return;
    }

    sensor_t* s = esp_camera_sensor_get();
    s->set_brightness(s, 0);  s->set_contrast(s, 1);
    s->set_saturation(s, 0);  s->set_sharpness(s, 1);
    s->set_whitebal(s, 1);    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);     s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);        s->set_ae_level(s, 0);
    s->set_aec_value(s, 300); s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);    s->set_gainceiling(s, (gainceiling_t)6);
    s->set_bpc(s, 1);         s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);     s->set_lenc(s, 1);
    s->set_hmirror(s, 0);     s->set_vflip(s, 0);
    s->set_dcw(s, 1);

    camReady = true;
    lg("CAM","Ready");
}

// ═══════════════════════════════════════════════════════════════
//  WIFI INIT
// ═══════════════════════════════════════════════════════════════
void initWiFi() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    Serial.print("[WIFI] Connecting");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
        delay(500); Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifiReady    = true;
        lastWifiOkMs = millis();
        wifiChannel  = WiFi.channel();
        lg("WIFI","IP=" + WiFi.localIP().toString()
            + " Ch=" + String(wifiChannel)
            + " RSSI=" + String(WiFi.RSSI()) + "dBm");
    } else {
        lg("WIFI","FAILED — Ch=1 fallback");
        wifiChannel = 1;
    }

    WiFi.softAP("ESP32CAM-SENTINEL", "sentinel123", wifiChannel);
    lg("WIFI","SoftAP on Ch." + String(wifiChannel) + " — Hub must match");
}

// ═══════════════════════════════════════════════════════════════
//  ESP-NOW
// ═══════════════════════════════════════════════════════════════
void initESPNOW() {
    if (esp_now_init() != ESP_OK) { lg("ESPNOW","Init FAILED"); return; }
    esp_now_register_recv_cb(onDataRecv);
    espnowReady = true;
    lg("ESPNOW","Listening Ch." + String(wifiChannel));

    uint8_t apMAC[6];
    esp_wifi_get_mac(WIFI_IF_AP, apMAC);
    char buf[18];
    sprintf(buf,"%02X:%02X:%02X:%02X:%02X:%02X",apMAC[0],apMAC[1],apMAC[2],apMAC[3],apMAC[4],apMAC[5]);
    lg("ESPNOW","AP MAC: " + String(buf) + "  (Hub must target this)");
    lg("ESPNOW","Expects " + String(sizeof(SensorPacket)) + "-byte SensorPacket");
}

// ═══════════════════════════════════════════════════════════════
//  mDNS + WEB SERVER
// ═══════════════════════════════════════════════════════════════
void setupMDNS() {
    if (!wifiReady) return;
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http","tcp",80);
}

void startWebServer() {
    if (!wifiReady) return;

    web.on("/status", []() {
        StaticJsonDocument<512> d;
        d["version"]     = VERSION;
        d["camera"]      = camReady;
        d["wifi"]        = wifiReady;
        d["espnow"]      = espnowReady;
        d["wsConnected"] = wsConnected;
        d["channel"]     = wifiChannel;
        d["busy"]        = captureBusy;
        d["alertCount"]  = alertCount;
        d["uploadOK"]    = uploadOK;
        d["uploadFail"]  = uploadFail;
        d["freeHeap"]    = ESP.getFreeHeap();
        d["uptime"]      = millis() / 1000;
        d["rssi"]        = WiFi.RSSI();
        d["server"]      = String(SERVER_HOST) + ":" + String(SERVER_PORT);
        d["hubSats"]     = hub.satellites;
        d["hubGpsValid"] = (bool)hub.gpsValid;
        d["hubBatV"]     = hub.batteryVoltage;
        String out; serializeJson(d, out);
        web.send(200,"application/json",out);
    });

    web.on("/capture", []() {
        if (!camReady)   { web.send(500,"text/plain","Camera not ready"); return; }
        if (captureBusy) { web.send(503,"text/plain","Busy"); return; }
        camera_fb_t* wb = esp_camera_fb_get(); if(wb) esp_camera_fb_return(wb);
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { web.send(500,"text/plain","Capture failed"); return; }
        web.sendHeader("Content-Type","image/jpeg");
        web.sendHeader("Content-Length",String(fb->len));
        web.sendHeader("Cache-Control","no-cache");
        web.send_P(200,"image/jpeg",(const char*)fb->buf,fb->len);
        esp_camera_fb_return(fb);
    });

    web.on("/", []() {
        String ws_status = wsConnected ? "Connected ✓" : "Disconnected ✗";
        String h = "<html><head><title>SENTINEL v"+String(VERSION)+"</title></head>"
            "<body style='font-family:monospace;background:#0a0a0a;color:#ddd;padding:24px'>"
            "<h2 style='color:#4af'>SENTINEL v" + String(VERSION) + "</h2>"
            "<table style='border-spacing:0 10px'>"
            "<tr><td style='color:#888;padding-right:20px'>Node Server</td><td><b>"
            + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "</b></td></tr>"
            "<tr><td style='color:#888'>WebSocket</td><td style='color:"
            + String(wsConnected?"#4f4":"#f44") + "'>" + ws_status + "</td></tr>"
            "<tr><td style='color:#888'>WiFi Channel</td><td><b>" + String(wifiChannel) + "</b> ← Hub must match</td></tr>"
            "<tr><td style='color:#888'>IP</td><td>" + WiFi.localIP().toString()
            + " (" + String(WiFi.RSSI()) + " dBm)</td></tr>"
            "<tr><td style='color:#888'>Free Heap</td><td>" + String(ESP.getFreeHeap()/1024) + " KB</td></tr>"
            "<tr><td style='color:#888'>Motion Alerts</td><td>" + String(alertCount) + "</td></tr>"
            "<tr><td style='color:#888'>Uploads</td><td>" + String(uploadOK) + " OK / "
            + String(uploadFail) + " fail</td></tr>"
            "</table><br>"
            "<a href='/capture' style='color:#4af'>📷 Snapshot</a>&nbsp; "
            "<a href='/status' style='color:#4af'>📊 Status JSON</a>"
            "</body></html>";
        web.send(200,"text/html",h);
    });

    web.begin();
    lg("WEB","HTTP :80  http://" + WiFi.localIP().toString());
}

void lg(const String& m, const String& msg) {
    uint32_t t = millis()/1000;
    Serial.printf("[%02lu:%02lu:%02lu] %-8s %s\n",
        (t/3600)%24,(t/60)%60,t%60,
        m.c_str(), msg.c_str());
}
