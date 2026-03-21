

#include "esp_camera.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <Preferences.h>

#define VERSION   "10.8.3"
#define HOSTNAME  "esp32cam-sentinel-2"
#define FLASH_LED 4


// ══════════════════════════════════════════════════════════════
//  ★  DEPLOYMENT MODE  ★
//
//  0 = LOCAL  — server on your PC, same WiFi as ESP32
//               SERVER_HOST = WiFi IP from `ipconfig`
//               SERVER_PORT = 3000
//
//  1 = CLOUD  — server on Railway / Render / Fly.io
//               SERVER_HOST = "your-app.railway.app"
//               SERVER_PORT = 443
// ══════════════════════════════════════════════════════════════
#define USE_CLOUD 1

// ══════════════ WIFI ════════════════════════════════════════
const char* WIFI_SSID = "ULTRA Network";
const char* WIFI_PASS = "&12345@100%";

// ══════════════ SERVER ═══════════════════════════════════════
#if USE_CLOUD
  // Cloud deployment — no https:// prefix, just the hostname
  const char* SERVER_HOST = "motion-detection-system-jai3.onrender.com";  // ← set this
  const int   SERVER_PORT = 443;
#else
  // Local LAN — must be your PC's WiFi IP, NOT VPN/Ethernet.
  // Run `ipconfig` → "Wireless LAN adapter Wi-Fi" → IPv4 Address
  // Server startup now prints ALL adapter IPs to help you pick.
  const char* SERVER_HOST = "10.236.0.132";  // ← CHANGE THIS
  const int   SERVER_PORT = 3000;
#endif

const char* DEVICE_TOKEN = "";
const char* DEVICE_ID    = "ESP32-CAM-02";


// ══════════════ TIMING ═══════════════════════════════════════
const unsigned long HEARTBEAT_MS        = 30000UL;
const unsigned long WIFI_WATCHDOG_MS    = 120000UL;
const unsigned long MOTION_COOLDOWN_MS  = 8000UL;
const unsigned long PERIODIC_CAPTURE_MS = 5UL * 60UL * 1000UL;
const unsigned long UPLOAD_TIMEOUT_MS   = 35000UL;
const unsigned long HTTP_TIMEOUT_MS     = 8000UL;
const unsigned long WS_RECONNECT_MS     = 3000UL;

// ══════════════ SENSOR PACKET ════════════════════════════════
// ⚠  MUST match Hub byte-for-byte.
// char deviceId[12] → 73 bytes total. Hub sends 73.
typedef struct __attribute__((packed)) {
    uint8_t  packetType;
    char     deviceId[12];   // ← [12] NOT [16]. Matches Hub.
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

static_assert(sizeof(SensorPacket) == 73,
    "SensorPacket must be 73 bytes — matches Hub. Check deviceId[] size.");

SensorPacket hub;
volatile bool newPacketReady  = false;
volatile bool captureBusy    = false;

// WS binary upload state ─────────────────────────────────────
bool          wsUploadPending = false; // waiting for upload_ack
String        wsUploadCmdId   = "";    // original cmd id for ack
unsigned long wsUploadSentMs  = 0;     // for timeout detection

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
Preferences    camPrefs;
WebSocketsClient wsClient;

bool camReady    = false;
bool wifiReady   = false;
bool espnowReady = false;
bool wsConnected = false;
int  wifiChannel = 1;

unsigned long alertCount   = 0;
unsigned long uploadOK     = 0;
unsigned long uploadFail   = 0;
unsigned long lastHeartMs    = 0;
unsigned long lastWifiOkMs   = 0;
unsigned long lastMotionMs   = 0;
unsigned long lastPeriodicMs = 0;

String pendingCmdId = "";
bool   deviceRegistered = false;  // set true after successful /device POST

// ══════════════ URL / HTTP HELPERS ═══════════════════════════
String serverUrl(const char* path) {
#if USE_CLOUD
    return "https://" + String(SERVER_HOST) + path;
#else
    return "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + path;
#endif
}

// beginRequest — pairs HTTPClient with the right transport.
// Cloud:  WiFiClientSecure (setInsecure, skips cert check)
// Local:  plain URL (no TLS, no heap overhead)
void beginRequest(WiFiClientSecure& sec, HTTPClient& http,
                  const char* path, unsigned long timeoutMs) {
#if USE_CLOUD
    sec.setInsecure();
    http.begin(sec, serverUrl(path));
#else
    http.begin(serverUrl(path));
#endif
    http.setTimeout(timeoutMs);
    if (strlen(DEVICE_TOKEN) > 0)
        http.addHeader("X-Device-Token", DEVICE_TOKEN);
}

// ══════════════════════════════════════════════════════════════
//  sendLog — POSTs to /log → Supabase logs table
//  Best-effort: 4s timeout, failure silently ignored.
// ══════════════════════════════════════════════════════════════
void sendLog(const char* level, const char* category, const String& message) {
    if (!wifiReady) return;
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["level"]     = level;
    doc["category"]  = category;
    doc["message"]   = message;

    if (wsConnected) {
        // Route through existing WS — zero new TLS connections.
        doc["type"] = "ws_log";
        String msg; serializeJson(doc, msg);
        wsSend(msg);
        return;
    }
    // WS down: fallback to direct HTTP
    doc.remove("type");
    WiFiClientSecure sec; HTTPClient http;
    beginRequest(sec, http, "/log", 4000);
    http.addHeader("Content-Type", "application/json");
    String body; serializeJson(doc, body);
    http.POST(body); http.end();
}

void wsSend(const String& json) {
    if (wsConnected) wsClient.sendTXT((uint8_t*)json.c_str(), json.length());
}

void sendCmdAck(const String& id, const char* status) {
    if (id.length() == 0) return;
    StaticJsonDocument<128> doc;
    doc["type"] = "ack"; doc["id"] = id; doc["status"] = status;
    String msg; serializeJson(doc, msg);
    wsSend(msg);
    lg("ACK", String(status) + "  id=" + id.substring(0,8));
}

// ══════════════ ESP-NOW CALLBACK ════════════════════════════
void IRAM_ATTR onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != (int)sizeof(SensorPacket)) {
        Serial.printf("[ESPNOW] Size mismatch got=%d expected=%d\n",
                      len, (int)sizeof(SensorPacket));
        return;
    }
    memcpy((void*)&hub, data, sizeof(SensorPacket));
    newPacketReady = true;
}

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET EVENT HANDLER
// ═══════════════════════════════════════════════════════════════
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_DISCONNECTED:
            wsConnected = false;
            lg("WS", "Disconnected");
            sendLog("warn", "websocket", "Disconnected from server");
            break;

        case WStype_CONNECTED: {
            wsConnected = true;
            lg("WS", "Connected (" + String(USE_CLOUD?"wss":"ws") + ")");
            sendLog("info", "websocket",
                "Connected " + String(USE_CLOUD?"wss":"ws")
                + "://" + String(SERVER_HOST));
            StaticJsonDocument<192> doc;
            doc["type"]     = "hello";
            doc["deviceId"] = DEVICE_ID;
            doc["version"]  = VERSION;
            doc["freeHeap"] = ESP.getFreeHeap();
            doc["channel"]  = wifiChannel;
            String msg; serializeJson(doc, msg);
            wsClient.sendTXT(msg);
            // If boot-time registration failed (DNS not ready), retry now.
            // DNS is confirmed working since WS just connected.
            if (!deviceRegistered) {
                lg("DB", "Retrying device registration...");
                registerDevice();
            }
            break;
        }

        case WStype_TEXT: {
            DynamicJsonDocument doc(512);
            if (deserializeJson(doc, payload, length)) return;
            String msgType = doc["type"] | "";

            // ── upload_ack — binary WS upload confirmed by server ──
            if (msgType == "upload_ack") {
                String status = doc["status"] | "done";
                bool ok = (status == "done");
                unsigned long elapsed = millis() - wsUploadSentMs;
                lg("CAM", "WS ack: " + status + "  " + String(elapsed) + "ms");
                if (ok) {
                    sendLog("info","camera","WS upload OK "+String(elapsed)+"ms");
                } else {
                    uploadFail++;
                    sendLog("error","camera","WS upload FAILED by server");
                }
                if (wsUploadCmdId.length() > 0) {
                    sendCmdAck(wsUploadCmdId, ok ? "done" : "failed");
                    wsUploadCmdId = "";
                }
                wsUploadPending = false;

            // ── command — capture / set_quality / set_resolution etc. ──
            } else if (msgType == "command") {
                String id      = doc["id"]      | "";
                String command = doc["command"] | "";
                lg("WS", "CMD: " + command + "  id=" + id.substring(0,8));

                if (command == "capture") {
                    if (!captureBusy) {
                        pendingCmdId = id;
                        captureAndUpload("manual", false);
                    } else {
                        sendLog("warn","command","Capture busy  id="+id.substring(0,8));
                        sendCmdAck(id, "failed");
                    }
                } else if (command.startsWith("set_quality:")) {
                    int q = constrain(command.substring(12).toInt(), 1, 63);
                    sensor_t* s = esp_camera_sensor_get();
                    if (s) s->set_quality(s, q);
                    sendLog("info","command","Quality="+String(q));
                    sendCmdAck(id, "done");
                } else if (command.startsWith("set_resolution:")) {
                    // set_resolution:UXGA | SVGA | XGA | HD | VGA
                    String res = command.substring(15);
                    res.trim();
                    sensor_t* s = esp_camera_sensor_get();
                    if (s) {
                        framesize_t fs = FRAMESIZE_UXGA;
                        if      (res == "UXGA") fs = FRAMESIZE_UXGA;
                        else if (res == "SXGA") fs = FRAMESIZE_SXGA;
                        else if (res == "XGA" ) fs = FRAMESIZE_XGA;
                        else if (res == "HD"  ) fs = FRAMESIZE_HD;
                        else if (res == "SVGA") fs = FRAMESIZE_SVGA;
                        else if (res == "VGA" ) fs = FRAMESIZE_VGA;
                        s->set_framesize(s, fs);
                        sendLog("info","command","Resolution="+res);
                        sendCmdAck(id, "done");
                    } else {
                        sendCmdAck(id, "failed");
                    }
                } else if (command.startsWith("set_brightness:")) {
                    int b = constrain(command.substring(15).toInt(), -2, 2);
                    sensor_t* s = esp_camera_sensor_get();
                    if (s) s->set_brightness(s, b);
                    sendLog("info","command","Brightness="+String(b));
                    sendCmdAck(id, "done");
                } else if (command == "reboot") {
                    sendLog("warn","command","Reboot commanded");
                    sendCmdAck(id, "done");
                    delay(300); ESP.restart();
                } else {
                    sendLog("warn","command","Unknown: "+command);
                    sendCmdAck(id, "failed");
                }

            // ── ping — keep-alive ──────────────────────────────────
            } else if (msgType == "ping") {
                wsSend("{\"type\":\"pong\"}");
            }
            break;
        }

        case WStype_ERROR:
            lg("WS", "Socket error");
            sendLog("error", "websocket", "Socket error");
            break;

        default: break;
    }
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
    Serial.printf( "║  ESP32-CAM SENTINEL v%-18s║\n", VERSION);
    Serial.printf( "║  Mode: %-32s║\n",
        USE_CLOUD ? "CLOUD (HTTPS + WSS)" : "LOCAL (HTTP + WS)");
    Serial.println(F("╚════════════════════════════════════════╝\n"));
    Serial.printf("[BOOT] SensorPacket=%d bytes  Server=%s://%s:%d\n",
        (int)sizeof(SensorPacket),
        USE_CLOUD?"https":"http", SERVER_HOST, SERVER_PORT);

    initCamera();
    loadCamSettings();   // apply any saved quality override
    initWiFi();
    initESPNOW();
    initWebSocket();
    setupMDNS();
    startWebServer();
    registerDevice();

    sendLog("info", "boot",
        "v" + String(VERSION)
        + " mode=" + String(USE_CLOUD?"cloud":"local")
        + " IP=" + WiFi.localIP().toString()
        + " heap=" + String(ESP.getFreeHeap()/1024) + "KB");

    captureAndUpload("startup", false);

    Serial.println(F("\n✅ SYSTEM READY"));
    Serial.printf("   Camera  : %s\n",  camReady    ? "OK" : "FAIL");
    Serial.printf("   WiFi    : %s  %s\n", wifiReady?"OK":"FAIL",
                  WiFi.localIP().toString().c_str());
    Serial.printf("   ESP-NOW : %s  Ch=%d\n", espnowReady?"OK":"FAIL", wifiChannel);
    Serial.printf("   Mode    : %s\n", USE_CLOUD?"CLOUD (HTTPS/WSS)":"LOCAL (HTTP/WS)");
    Serial.printf("   Server  : %s://%s:%d\n\n",
        USE_CLOUD?"https":"http", SERVER_HOST, SERVER_PORT);
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();
    wsClient.loop();
    if (wifiReady) web.handleClient();

    if (newPacketReady) { newPacketReady = false; handleSensorPacket(now); }

    if (!captureBusy && (now - lastHeartMs >= HEARTBEAT_MS)) {
        lastHeartMs = now; sendHeartbeat();
    }
    if (!captureBusy && lastPeriodicMs > 0
            && (now - lastPeriodicMs >= PERIODIC_CAPTURE_MS)) {
        lastPeriodicMs = now;
        sendLog("info", "camera", "Periodic capture");
        captureAndUpload("periodic", false);
    }
    if (lastPeriodicMs == 0 && camReady && wifiReady) lastPeriodicMs = now;

    // WS upload timeout — if ack not received in 30s, release busy flag
    if (wsUploadPending && (millis() - wsUploadSentMs > 30000UL)) {
        wsUploadPending = false;
        wsUploadCmdId   = "";
        lg("CAM", "WS upload timeout — no ack in 30s");
    }

    if (WiFi.status() == WL_CONNECTED) {
        lastWifiOkMs = now; wifiReady = true;
    } else if (wifiReady && (now - lastWifiOkMs > WIFI_WATCHDOG_MS)) {
        lg("WIFI", "Watchdog — reconnecting");
        wifiReady = false; WiFi.reconnect();
    }
}

// ═══════════════════════════════════════════════════════════════
//  SENSOR PACKET HANDLER
// ═══════════════════════════════════════════════════════════════
void handleSensorPacket(unsigned long now) {
    if (hub.packetType == 0) {
        alertCount++;
        String gps = hub.gpsValid
            ? String(hub.latitude,5)+","+String(hub.longitude,5) : "NoFix";
        lg("PKT", "MOTION #" + String(alertCount)
            + " " + String(hub.deviceId)
            + " dist=" + String(hub.distance_cm,1) + "cm  gps=" + gps);
        sendLog("info", "motion",
            "Motion #" + String(alertCount)
            + " from=" + String(hub.deviceId)
            + " dist=" + String(hub.distance_cm,0) + "cm  gps=" + gps);

        if (!captureBusy && (now - lastMotionMs >= MOTION_COOLDOWN_MS)) {
            lastMotionMs = now;
            captureAndUpload("motion", true);
        }
    } else {
        sendHubHeartbeat();
    }
}


// ══════════════════════════════════════════════════════════════
//  streamUpload — streams multipart/form-data over raw TLS
//
//  Why not HTTPClient.POST()?
//    On HTTPS, HTTPClient buffers the entire body before sending.
//    A 235KB image causes "Failed to send chunk!" (HTTP -3) on
//    Render because the underlying SSL record loop stalls on
//    large single-shot writes.
//
//  This function opens the TLS socket directly, writes the HTTP
//  request headers manually, then streams the body in 4KB blocks.
//  Returns the HTTP response code, or -1 on connection failure.
// ══════════════════════════════════════════════════════════════
static const size_t STREAM_CHUNK = 4096;

int streamUpload(const String& boundary, const String& header,
                 const uint8_t* imgBuf, size_t imgLen,
                 const String& footer, int retryCount = 0) {
    // Retry up to 2 times on connect or stall failure.
    // HTTP error codes (4xx/5xx) are not retried.

    size_t totalBody = header.length() + imgLen + footer.length();

#if USE_CLOUD
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(45);          // 45s socket timeout
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        lg("CAM", "streamUpload: TLS connect failed");
        return -1;
    }
#else
    WiFiClient client;
    client.setTimeout(45);
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        lg("CAM", "streamUpload: TCP connect failed");
        return -1;
    }
#endif

    // ── HTTP request line + headers ─────────────────────────────
    String req = "POST /upload HTTP/1.1\r\n";
    req += "Host: " + String(SERVER_HOST) + "\r\n";
    req += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
    req += "Content-Length: " + String(totalBody) + "\r\n";
    if (strlen(DEVICE_TOKEN) > 0)
        req += "X-Device-Token: " + String(DEVICE_TOKEN) + "\r\n";
    req += "Connection: close\r\n\r\n";
    client.print(req);

    // ── Multipart text fields ────────────────────────────────────
    client.print(header);

    // ── Image in 4KB chunks ─────────────────────────────────────
    size_t sent = 0;
    while (sent < imgLen) {
        size_t chunk = min(STREAM_CHUNK, imgLen - sent);
        size_t written = client.write(imgBuf + sent, chunk);
        if (written == 0) {
            lg("CAM", "streamUpload: write stalled at " + String(sent) + "B");
            client.stop();
            return -3;
        }
        sent += written;
    }

    // ── Closing boundary ────────────────────────────────────────
    client.print(footer);

    // ── Read response ────────────────────────────────────────────
    unsigned long deadline = millis() + 15000UL;
    while (client.connected() && millis() < deadline) {
        if (client.available()) break;
        delay(10);
    }

    int httpCode = -1;
    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        // "HTTP/1.1 200 OK"
        int sp1 = statusLine.indexOf(' ');
        int sp2 = statusLine.indexOf(' ', sp1 + 1);
        if (sp1 > 0 && sp2 > sp1)
            httpCode = statusLine.substring(sp1 + 1, sp2).toInt();
    }

    client.stop();
    // Retry on network failure (not on HTTP 4xx/5xx)
    if ((httpCode == -1 || httpCode == -3) && retryCount < 2) {
        lg("CAM", "streamUpload retry " + String(retryCount+1) + "/2");
        delay(500 * (retryCount + 1));   // back-off: 500ms, 1000ms
        return streamUpload(boundary, header, imgBuf, imgLen, footer, retryCount + 1);
    }
    return httpCode;
}


// ══════════════════════════════════════════════════════════════
//  uploadViaWebSocket — sends JPEG as binary WS frame
//
//  Frame format: [4 bytes uint32-LE: JSON metadata length]
//                [JSON metadata string]
//                [raw JPEG bytes]
//
//  Uses the already-open WSS connection → zero new TLS handshake.
//  ~3-5x faster than streamUpload() for the same image.
//
//  Returns true if frame was sent to WS stack (does not wait for
//  server ack — that arrives asynchronously as upload_ack text).
// ══════════════════════════════════════════════════════════════
bool uploadViaWebSocket(const uint8_t* imgBuf, size_t imgLen,
                        const char* snapType, const String& cmdId) {
    if (!wsConnected) return false;

    // Build JSON metadata (same fields as multipart HTTP upload)
    StaticJsonDocument<512> metaDoc;
    metaDoc["snapType"]    = snapType;
    metaDoc["cmdId"]       = cmdId;
    metaDoc["deviceId"]    = DEVICE_ID;
    metaDoc["triggeredBy"] = hub.deviceId[0] ? hub.deviceId : DEVICE_ID;
    metaDoc["latitude"]    = hub.gpsValid ? String(hub.latitude,  6) : "0";
    metaDoc["longitude"]   = hub.gpsValid ? String(hub.longitude, 6) : "0";
    metaDoc["gpsValid"]    = hub.gpsValid ? "1" : "0";
    metaDoc["distanceCm"]  = String(hub.distance_cm, 0);
    metaDoc["smsSent"]     = hub.gsmReady ? "1" : "0";
    metaDoc["altitudeM"]   = String(hub.altitude_m,  1);
    metaDoc["speedKmh"]    = String(hub.speed_kmh,   1);
    metaDoc["hdop"]        = String(hub.hdop,         2);
    metaDoc["satellites"]  = hub.satellites;
    metaDoc["hubHeap"]     = hub.freeHeap;
    metaDoc["hubBattery"]  = String(hub.batteryVoltage, 3);
    metaDoc["hubGsmCsq"]   = hub.gsmSignalCSQ;
    metaDoc["hubUptimeS"]  = hub.uptimeSeconds;
    metaDoc["hubFwVersion"]= String(hub.fwVersion);
    metaDoc["camHeap"]     = ESP.getFreeHeap();
    metaDoc["camRssi"]     = wifiReady ? WiFi.RSSI() : 0;

    String metaStr;
    serializeJson(metaDoc, metaStr);
    uint32_t metaLen = metaStr.length();

    // Allocate frame: [4-byte len][JSON][JPEG]
    size_t frameLen = 4 + metaLen + imgLen;
    uint8_t* frame = (uint8_t*)ps_malloc(frameLen);
    if (!frame) frame = (uint8_t*)malloc(frameLen);
    if (!frame) {
        lg("CAM", "uploadViaWebSocket: malloc failed " + String(frameLen/1024) + "KB");
        return false;
    }

    // Write 4-byte LE length prefix
    frame[0] = (metaLen)       & 0xFF;
    frame[1] = (metaLen >>  8) & 0xFF;
    frame[2] = (metaLen >> 16) & 0xFF;
    frame[3] = (metaLen >> 24) & 0xFF;
    memcpy(frame + 4,           metaStr.c_str(), metaLen);
    memcpy(frame + 4 + metaLen, imgBuf,          imgLen);

    bool sent = wsClient.sendBIN(frame, frameLen);
    free(frame);

    if (sent) {
        lg("CAM", "WS upload sent " + String(imgLen/1024) + "KB  type=" + String(snapType));
    } else {
        lg("CAM", "WS sendBIN failed");
    }
    return sent;
}



// ═══════════════════════════════════════════════════════════════
//  LOAD CAM SETTINGS from Preferences
//  Called at boot — applies any saved quality, cooldown, etc.
// ═══════════════════════════════════════════════════════════════
void loadCamSettings() {
    camPrefs.begin("camcfg", true);
    uint32_t q = camPrefs.getUInt("quality", 0);
    camPrefs.end();
    if (q >= 4 && q <= 63) {
        sensor_t* s = esp_camera_sensor_get();
        if (s) { s->set_quality(s, q); lg("CFG","Quality loaded: "+String(q)); }
    }
}

// ═══════════════════════════════════════════════════════════════
//  CAMERA RECOVERY
//  Called when esp_camera_fb_get() returns NULL.
//  Deinits and reinitialises the camera driver.
//  Returns true if recovery succeeded.
// ═══════════════════════════════════════════════════════════════
bool camReinit() {
    lg("CAM", "Recovery: deinit");
    esp_camera_deinit();
    delay(300);
    camReady = false;

    camera_config_t cfg;
    cfg.ledc_channel=LEDC_CHANNEL_0; cfg.ledc_timer=LEDC_TIMER_0;
    cfg.pin_d0=Y2_GPIO_NUM; cfg.pin_d1=Y3_GPIO_NUM;
    cfg.pin_d2=Y4_GPIO_NUM; cfg.pin_d3=Y5_GPIO_NUM;
    cfg.pin_d4=Y6_GPIO_NUM; cfg.pin_d5=Y7_GPIO_NUM;
    cfg.pin_d6=Y8_GPIO_NUM; cfg.pin_d7=Y9_GPIO_NUM;
    cfg.pin_xclk=XCLK_GPIO_NUM;  cfg.pin_pclk=PCLK_GPIO_NUM;
    cfg.pin_vsync=VSYNC_GPIO_NUM; cfg.pin_href=HREF_GPIO_NUM;
    cfg.pin_sscb_sda=SIOD_GPIO_NUM; cfg.pin_sscb_scl=SIOC_GPIO_NUM;
    cfg.pin_pwdn=PWDN_GPIO_NUM;  cfg.pin_reset=RESET_GPIO_NUM;
    cfg.xclk_freq_hz=20000000;
    cfg.pixel_format=PIXFORMAT_JPEG;
    if (psramFound()) {
        cfg.frame_size=FRAMESIZE_UXGA; cfg.jpeg_quality=10; cfg.fb_count=2;
    } else {
        cfg.frame_size=FRAMESIZE_XGA;  cfg.jpeg_quality=15; cfg.fb_count=1;
    }
    if (esp_camera_init(&cfg) != ESP_OK) {
        lg("CAM", "Recovery: FAILED — camera offline");
        return false;
    }
    // Re-apply sensor tuning
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s,0);  s->set_contrast(s,1);
        s->set_saturation(s,0);  s->set_sharpness(s,1);
        s->set_whitebal(s,1);    s->set_awb_gain(s,1);
        s->set_wb_mode(s,0);     s->set_exposure_ctrl(s,1);
        s->set_aec2(s,1);        s->set_ae_level(s,0);
        s->set_aec_value(s,300); s->set_gain_ctrl(s,1);
        s->set_agc_gain(s,0);    s->set_gainceiling(s,(gainceiling_t)6);
        s->set_bpc(s,1);         s->set_wpc(s,1);
        s->set_raw_gma(s,1);     s->set_lenc(s,1);
        s->set_hmirror(s,0);     s->set_vflip(s,0);
        s->set_dcw(s,1);
    }
    camReady = true;
    lg("CAM", "Recovery: OK");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  CAPTURE + UPLOAD
// ═══════════════════════════════════════════════════════════════
void captureAndUpload(const char* snapType, bool fastCapture) {
    if (!camReady || !wifiReady || captureBusy) return;
    captureBusy = true;

    // Flush stale frames — flush exactly fb_count frames so the
    // driver can refill with a fresh exposure.
    // UXGA needs 300ms for a clean frame; 150ms caused NULL returns.
    for (int fi = 0; fi < 2; fi++) {
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }
    if (!fastCapture) delay(300);   // 300ms — enough for UXGA re-expose

    unsigned long t0 = millis();
    camera_fb_t* fb  = esp_camera_fb_get();

    // Recovery: if grab fails, try one deinit/reinit cycle
    if (!fb) {
        lg("CAM", "Frame grab NULL — heap=" + String(ESP.getFreeHeap()/1024)
            + "KB  psram=" + String(ESP.getFreePsram()/1024) + "KB");
        sendLog("warn","camera","Frame grab NULL — attempting recovery");
        if (camReinit()) {
            delay(500);
            fb = esp_camera_fb_get();   // one retry after reinit
        }
    }

    if (!fb) {
        sendLog("error","camera","Frame grab failed after recovery ("+String(snapType)+")");
        captureBusy = false;
        if (pendingCmdId.length()>0){sendCmdAck(pendingCmdId,"failed");pendingCmdId="";}
        return;
    }
    size_t imgKB = fb->len/1024;
    lg("CAM","Captured "+String(imgKB)+"KB  "+String(millis()-t0)+"ms  "+String(snapType));
    sendLog("info","camera",String(snapType)+" captured "+String(imgKB)+"KB");

    size_t imgLen = fb->len;
    uint8_t* imgBuf = (uint8_t*)ps_malloc(imgLen);
    if (!imgBuf) imgBuf = (uint8_t*)malloc(imgLen);
    if (!imgBuf) {
        sendLog("error","camera","malloc failed image "+String(imgLen/1024)+"KB");
        esp_camera_fb_return(fb); uploadFail++; captureBusy=false;
        if(pendingCmdId.length()>0){sendCmdAck(pendingCmdId,"failed");pendingCmdId="";}
        return;
    }
    memcpy(imgBuf, fb->buf, imgLen);
    esp_camera_fb_return(fb);

    // Build multipart body
    String bnd = "SntBnd" + String(millis());
    String hdr;
    struct Field { const char* name; String value; };
    Field fields[] = {
        {"snapType",     String(snapType)},
        {"deviceId",     String(DEVICE_ID)},
        {"triggeredBy",  hub.deviceId[0] ? String(hub.deviceId) : String(DEVICE_ID)},
        {"latitude",     hub.gpsValid ? String(hub.latitude, 6) : "0"},
        {"longitude",    hub.gpsValid ? String(hub.longitude,6) : "0"},
        {"distanceCm",   String(hub.distance_cm,0)},
        {"smsSent",      hub.gsmReady?"1":"0"},
        {"altitudeM",    String(hub.altitude_m,1)},
        {"speedKmh",     String(hub.speed_kmh,1)},
        {"hdop",         String(hub.hdop,2)},
        {"satellites",   String(hub.satellites)},
        {"gpsValid",     String((int)hub.gpsValid)},
        {"gpsHasFix",    String((int)hub.gpsHasFix)},
        {"hubHeap",      String(hub.freeHeap)},
        {"hubBattery",   String(hub.batteryVoltage,3)},
        {"hubGsmCsq",    String(hub.gsmSignalCSQ)},
        {"hubUptimeS",   String(hub.uptimeSeconds)},
        {"hubFwVersion", String(hub.fwVersion)},
        {"camHeap",      String(ESP.getFreeHeap())},
        {"camRssi",      String(wifiReady?WiFi.RSSI():0)},
    };
    for (auto& f : fields) {
        hdr += "--"+bnd+"\r\nContent-Disposition: form-data; name=\""
            +String(f.name)+"\"\r\n\r\n"+f.value+"\r\n";
    }
    hdr += "--"+bnd+"\r\nContent-Disposition: form-data; name=\"image\"; filename=\"snap.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String ftr = "\r\n--"+bnd+"--\r\n";

    // ── Upload strategy ──────────────────────────────────────────
    //  1st choice: binary WS frame over existing TLS connection.
    //              Zero new TLS handshake → ~3-5x faster.
    //              Server sends back upload_ack text frame.
    //
    //  Fallback:   streamUpload() (raw TLS, 4KB chunks).
    //              Used if WS is down at capture time.
    t0 = millis();

    if (wsConnected) {
        bool sent = uploadViaWebSocket(imgBuf, imgLen, snapType, pendingCmdId);
        if (sent) {
            // SUCCESS — WS path owns imgBuf from here (already copied into frame
            // inside uploadViaWebSocket). Free it now, ack arrives asynchronously.
            free(imgBuf);
            wsUploadPending = true;
            wsUploadCmdId   = pendingCmdId;
            wsUploadSentMs  = millis();
            pendingCmdId    = "";
            captureBusy     = false;
            uploadOK++;
            return;  // upload_ack arrives via webSocketEvent
        }
        // sendBIN failed (WS unstable) — imgBuf still valid, fall through to HTTP.
        // DO NOT free imgBuf here — HTTP fallback still needs it.
        lg("CAM", "WS sendBIN failed — falling back to HTTP upload");
    }

    {
        // HTTP fallback: 4KB chunked TLS stream.
        // imgBuf is still valid whether WS was tried or not.
        int code = streamUpload(bnd, hdr, imgBuf, imgLen, ftr);
        free(imgBuf);   // single owner, freed exactly once
        unsigned long elapsed = millis()-t0;
        lg("CAM","HTTP upload "+String(code)+"  "+String(elapsed)+"ms");
        bool ok = (code==200);
        if (ok) {
            uploadOK++;
            sendLog("info","camera",
                String(snapType)+" HTTP upload OK "+String(elapsed)+"ms "+String(imgKB)+"KB");
        } else {
            uploadFail++;
            lg("CAM","HTTP FAILED "+String(code));
            sendLog("error","camera",
                String(snapType)+" HTTP FAILED "+String(code));
        }
        if(pendingCmdId.length()>0){sendCmdAck(pendingCmdId,ok?"done":"failed");pendingCmdId="";}
    }
    captureBusy=false;
}

// ═══════════════════════════════════════════════════════════════
//  HEARTBEAT
// ═══════════════════════════════════════════════════════════════
void sendHeartbeat() {
    if (!wifiReady) return;
    DynamicJsonDocument doc(512);
    doc["device_id"]        = DEVICE_ID;
    doc["rssi"]             = WiFi.RSSI();
    doc["free_heap"]        = ESP.getFreeHeap();
    doc["uptime_seconds"]   = millis()/1000;
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
    int code = -99;
    if (wsConnected) {
        // Piggyback on existing WS TLS — no new TLS handshake.
        doc["type"] = "ws_heartbeat";
        String wsMsg; serializeJson(doc, wsMsg);
        wsSend(wsMsg);
        code = 0;  // 0 = sent via WS
    } else {
        // WS down: fallback to direct HTTP
        WiFiClientSecure sec; HTTPClient http;
        beginRequest(sec, http, "/heartbeat", HTTP_TIMEOUT_MS);
        http.addHeader("Content-Type","application/json");
        code = http.POST(body); http.end();
    }
    lg("HB", String(code==0?"WS":"HTTP "+String(code))
        +" heap="+String(ESP.getFreeHeap()/1024)+"KB"
        +" rssi="+String(WiFi.RSSI())+"dBm"
        +" ws="+(wsConnected?"UP":"DOWN"));
}

void sendHubHeartbeat() {
    if (!wifiReady) return;
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
    if (wsConnected) {
        doc["type"] = "ws_hub_heartbeat";
        String wsMsg; serializeJson(doc, wsMsg);
        wsSend(wsMsg);
    } else {
        String body; serializeJson(doc, body);
        WiFiClientSecure sec; HTTPClient http;
        beginRequest(sec, http, "/heartbeat", HTTP_TIMEOUT_MS);
        http.addHeader("Content-Type","application/json");
        http.POST(body); http.end();
    }
}

void registerDevice() {
    if (!wifiReady) return;
    WiFiClientSecure sec; HTTPClient http;
    beginRequest(sec, http, "/device", HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type","application/json");
    StaticJsonDocument<256> doc;
    doc["device_id"]        = DEVICE_ID;
    doc["device_type"]      = "esp32_cam";
    doc["name"]             = "Camera Unit";
    doc["ip_address"]       = WiFi.localIP().toString();
    doc["firmware_version"] = VERSION;
    doc["status"]           = "online";
    String body; serializeJson(doc,body);
    int code = http.POST(body); http.end();
    if (code == 200 || code == 201) {
        deviceRegistered = true;
        lg("DB","Device registered OK");
    } else {
        lg("DB","Registration failed HTTP "+String(code)+" — will retry on WS connect");
    }
}

// ═══════════════════════════════════════════════════════════════
//  WEBSOCKET INIT
//  Local → begin()    (plain ws://)
//  Cloud → beginSSL() (wss://, TLS)
// ═══════════════════════════════════════════════════════════════
void initWebSocket() {
#if USE_CLOUD
    wsClient.beginSSL(SERVER_HOST, SERVER_PORT, "/ws");
    lg("WS","wss://"+String(SERVER_HOST)+":"+String(SERVER_PORT)+"/ws");
#else
    wsClient.begin(SERVER_HOST, SERVER_PORT, "/ws");
    lg("WS","ws://"+String(SERVER_HOST)+":"+String(SERVER_PORT)+"/ws");
#endif
    wsClient.onEvent(webSocketEvent);
    wsClient.setReconnectInterval(WS_RECONNECT_MS);
    wsClient.enableHeartbeat(15000, 5000, 2);
}

// ═══════════════════════════════════════════════════════════════
//  CAMERA INIT
//  PSRAM:    UXGA (1600×1200) Q10 — reliable ceiling for OV2640
//  No PSRAM: XGA  (1024×768)  Q15
//
//  ⚠  Q6 causes DMA buffer overflow at UXGA → fb_get returns NULL.
//     Q10 is the sweet spot: visibly sharp, always succeeds.
// ═══════════════════════════════════════════════════════════════
void initCamera() {
    camera_config_t cfg;
    cfg.ledc_channel=LEDC_CHANNEL_0; cfg.ledc_timer=LEDC_TIMER_0;
    cfg.pin_d0=Y2_GPIO_NUM; cfg.pin_d1=Y3_GPIO_NUM;
    cfg.pin_d2=Y4_GPIO_NUM; cfg.pin_d3=Y5_GPIO_NUM;
    cfg.pin_d4=Y6_GPIO_NUM; cfg.pin_d5=Y7_GPIO_NUM;
    cfg.pin_d6=Y8_GPIO_NUM; cfg.pin_d7=Y9_GPIO_NUM;
    cfg.pin_xclk=XCLK_GPIO_NUM;  cfg.pin_pclk=PCLK_GPIO_NUM;
    cfg.pin_vsync=VSYNC_GPIO_NUM; cfg.pin_href=HREF_GPIO_NUM;
    cfg.pin_sscb_sda=SIOD_GPIO_NUM; cfg.pin_sscb_scl=SIOC_GPIO_NUM;
    cfg.pin_pwdn=PWDN_GPIO_NUM;  cfg.pin_reset=RESET_GPIO_NUM;
    cfg.xclk_freq_hz=20000000;
    cfg.pixel_format=PIXFORMAT_JPEG;
    if (psramFound()) {
        // UXGA (1600x1200) Q10 in all modes.
        // streamUpload() sends in 4KB chunks — file size is not a limit.
        // Q10 is the reliable ceiling for OV2640 at UXGA.
        // Q6 causes DMA buffer overflow → esp_camera_fb_get() returns NULL.
        cfg.frame_size=FRAMESIZE_UXGA; cfg.jpeg_quality=10; cfg.fb_count=2;
        lg("CAM","PSRAM — UXGA Q10 fb=2");
    } else {
        cfg.frame_size=FRAMESIZE_XGA;  cfg.jpeg_quality=15; cfg.fb_count=1;
        lg("CAM","No PSRAM — XGA Q15 fb=1");
    }
    if (esp_camera_init(&cfg)!=ESP_OK) { lg("CAM","Init FAILED"); camReady=false; return; }
    sensor_t* s = esp_camera_sensor_get();
    s->set_brightness(s,0);  s->set_contrast(s,1);
    s->set_saturation(s,0);  s->set_sharpness(s,1);
    s->set_whitebal(s,1);    s->set_awb_gain(s,1);
    s->set_wb_mode(s,0);     s->set_exposure_ctrl(s,1);
    s->set_aec2(s,1);        s->set_ae_level(s,0);
    s->set_aec_value(s,300); s->set_gain_ctrl(s,1);
    s->set_agc_gain(s,0);    s->set_gainceiling(s,(gainceiling_t)6);
    s->set_bpc(s,1);         s->set_wpc(s,1);
    s->set_raw_gma(s,1);     s->set_lenc(s,1);
    s->set_hmirror(s,0);     s->set_vflip(s,0);
    s->set_dcw(s,1);
    camReady=true;
lg("CAM", psramFound() ? "Ready — UXGA Q10" : "Ready — XGA Q15");
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════
void initWiFi() {
    // ── Phase 1: STA connection via WiFiManager ──────────────────
    // First boot: CAM creates AP "ESP32CAM-SENTINEL-2", user visits
    // 192.168.4.1 and enters WiFi password once. Saved to flash.
    // Subsequent boots connect automatically — no portal shown.
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(20);
    wm.setTitle("SENTINEL CAM Setup");

    Serial.println(F("[WIFI] Starting WiFiManager..."));
    Serial.println(F("[WIFI] First boot? Connect to 'ESP32CAM-SENTINEL-2' → 192.168.4.1"));

    // autoConnect: if saved creds work, connects silently.
    // If not, opens portal on "ESP32CAM-SENTINEL-2" / "sentinel123".
    bool ok = wm.autoConnect("ESP32CAM-SENTINEL-2", "sentinel123");

    if (ok) {
        wifiReady = true;
        lastWifiOkMs = millis();
        wifiChannel  = WiFi.channel();
        lg("WIFI","IP="+WiFi.localIP().toString()+" Ch="+String(wifiChannel)+" RSSI="+String(WiFi.RSSI())+"dBm");
    } else {
        lg("WIFI","Portal timed out — no STA connection");
        wifiChannel = 6;   // fallback channel for ESP-NOW
    }

    // ── Phase 2: AP for ESP-NOW + local web access ───────────────
    // SoftAP must be on the same channel as the STA connection so
    // ESP-NOW (which binds to the AP MAC) is reachable by the Hub.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32CAM-SENTINEL-2", "sentinel123", wifiChannel);
    lg("WIFI","SoftAP Ch."+String(wifiChannel));

    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
}

// ═══════════════════════════════════════════════════════════════
//  ESP-NOW
// ═══════════════════════════════════════════════════════════════
void initESPNOW() {
    if (esp_now_init()!=ESP_OK) { lg("ESPNOW","Init FAILED"); return; }
    esp_now_register_recv_cb(onDataRecv);
    espnowReady=true;
    lg("ESPNOW","Listening Ch."+String(wifiChannel)+" pktSize="+String(sizeof(SensorPacket)));
    uint8_t apMAC[6]; esp_wifi_get_mac(WIFI_IF_AP,apMAC);
    char buf[18];
    sprintf(buf,"%02X:%02X:%02X:%02X:%02X:%02X",apMAC[0],apMAC[1],apMAC[2],apMAC[3],apMAC[4],apMAC[5]);
    lg("ESPNOW","AP MAC: "+String(buf));
}

void setupMDNS() {
    if (!wifiReady) return;
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http","tcp",80);
}

void startWebServer() {
    if (!wifiReady) return;
    web.on("/status",[](){
        StaticJsonDocument<512> d;
        d["version"]=VERSION; d["mode"]=USE_CLOUD?"cloud":"local";
        d["server"]=String(SERVER_HOST)+":"+String(SERVER_PORT);
        d["camera"]=camReady; d["wifi"]=wifiReady; d["espnow"]=espnowReady;
        d["wsConnected"]=wsConnected; d["busy"]=captureBusy;
        d["alertCount"]=alertCount; d["uploadOK"]=uploadOK; d["uploadFail"]=uploadFail;
        d["freeHeap"]=ESP.getFreeHeap(); d["uptime"]=millis()/1000;
        d["rssi"]=WiFi.RSSI(); d["pktSize"]=(int)sizeof(SensorPacket);
        // ap_mac — used by Sensor Hub auto-discovery (no hardcoded MAC needed)
        uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_AP, mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        d["ap_mac"] = macStr;
        String out; serializeJson(d,out);
        web.send(200,"application/json",out);
    });
    web.on("/capture",[](){
        if(!camReady){web.send(500,"text/plain","Camera not ready");return;}
        if(captureBusy){web.send(503,"text/plain","Busy");return;}
        camera_fb_t* w1=esp_camera_fb_get(); if(w1) esp_camera_fb_return(w1);
        camera_fb_t* w2=esp_camera_fb_get(); if(w2) esp_camera_fb_return(w2);
        camera_fb_t* fb=esp_camera_fb_get();
        if(!fb){web.send(500,"text/plain","Capture failed");return;}
        web.sendHeader("Content-Type","image/jpeg");
        web.sendHeader("Content-Length",String(fb->len));
        web.sendHeader("Cache-Control","no-cache");
        web.send_P(200,"image/jpeg",(const char*)fb->buf,fb->len);
        esp_camera_fb_return(fb);
    });
    web.on("/",[](){
        String mode=USE_CLOUD?"[CLOUD]":"[LOCAL]";
        String col =USE_CLOUD?"#4f4":"#fa0";
        String h="<html><head><title>SENTINEL v"+String(VERSION)+"</title></head>"
            "<body style='font-family:monospace;background:#0a0a0a;color:#ddd;padding:24px'>"
            "<h2 style='color:#4af'>SENTINEL v"+String(VERSION)
            +" <span style='color:"+col+"'>"+mode+"</span></h2>"
            "<table style='border-spacing:0 10px'>"
            "<tr><td style='color:#888;padding-right:20px'>WS</td><td style='color:"
            +String(wsConnected?"#4f4":"#f44")+"'>"
            +String(wsConnected?"Connected ✓":"Disconnected ✗")+"</td></tr>"
            "<tr><td style='color:#888'>IP</td><td>"+WiFi.localIP().toString()
            +" ("+String(WiFi.RSSI())+" dBm)</td></tr>"
            "<tr><td style='color:#888'>Heap</td><td>"+String(ESP.getFreeHeap()/1024)+" KB</td></tr>"
            "<tr><td style='color:#888'>Alerts</td><td>"+String(alertCount)+"</td></tr>"
            "<tr><td style='color:#888'>Uploads</td><td>"+String(uploadOK)+" OK / "+String(uploadFail)+" fail</td></tr>"
            "</table><br>"
            "<a href='/capture' style='color:#4af'>📷 Snapshot</a>&nbsp;"
            "<a href='/settings' style='color:#4af'>⚙ Settings</a>&nbsp;"
            "<a href='/status' style='color:#4af'>📊 JSON</a>&nbsp;"
            "<a href='/reboot' style='color:#fa0'>↺ Reboot</a></body></html>";
        web.send(200,"text/html",h);
    });
    // ── /settings GET — show config form ────────────────────────
    web.on("/settings", HTTP_GET, [](){
        camPrefs.begin("camcfg", true);
        String devId   = camPrefs.getString("device_id",   DEVICE_ID);
        String motCool = String(camPrefs.getUInt("mot_cool", MOTION_COOLDOWN_MS / 1000));
        String perMin  = String(camPrefs.getUInt("per_min",  PERIODIC_CAPTURE_MS / 60000));
        String quality = String(camPrefs.getUInt("quality",  10));
        camPrefs.end();

        String h = "<!DOCTYPE html><html><head>"
            "<title>SENTINEL Settings</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>"
            "body{font-family:monospace;background:#0a0a0a;color:#ddd;padding:20px;max-width:480px}"
            "h2{color:#4af}label{color:#aaa;display:block;margin-top:14px;font-size:13px}"
            "input{width:100%;box-sizing:border-box;background:#1a1a1a;color:#fff;"
            "border:1px solid #333;padding:8px;margin-top:4px;border-radius:4px;font-family:monospace}"
            "button{margin-top:20px;width:100%;padding:10px;background:#4af;color:#000;"
            "border:none;border-radius:4px;font-size:15px;cursor:pointer;font-weight:bold}"
            "button:active{background:#28f}.note{color:#666;font-size:11px;margin-top:4px}"
            ".ok{color:#4f4;margin-top:12px;display:none}"
            "</style></head><body>"
            "<h2>⚙ SENTINEL CAM " + String(VERSION) + "</h2>"
            "<form method='POST' action='/settings'>"
            "<label>Device ID</label>"
            "<input name='device_id' value='" + devId + "' maxlength='11'>"
            "<p class='note'>Identifies this camera in the dashboard</p>"
            "<label>Motion cooldown (seconds)</label>"
            "<input name='mot_cool' type='number' value='" + motCool + "' min='1' max='300'>"
            "<p class='note'>Minimum gap between motion-triggered captures</p>"
            "<label>Periodic capture interval (minutes)</label>"
            "<input name='per_min' type='number' value='" + perMin + "' min='1' max='1440'>"
            "<p class='note'>How often to capture automatically (0 = disable)</p>"
            "<label>JPEG quality (1=best/large … 63=worst/small)</label>"
            "<input name='quality' type='number' value='" + quality + "' min='4' max='63'>"
            "<p class='note'>Recommended: 10 (UXGA). Lower = sharper but larger file.</p>"
            "<button type='submit'>💾 Save &amp; Apply</button>"
            "</form>"
            "<p style='color:#666;font-size:11px;margin-top:20px'>"
            "Changes apply instantly. No reboot needed.<br>"
            "To change WiFi: reboot and connect to ESP32CAM-SENTINEL-2 AP.</p>"
            "</body></html>";
        web.send(200, "text/html", h);
    });

    // ── /settings POST — save and apply ──────────────────────────
    web.on("/settings", HTTP_POST, [](){
        camPrefs.begin("camcfg", false);
        bool changed = false;

        if (web.hasArg("device_id")) {
            String v = web.arg("device_id"); v.trim();
            if (v.length() > 0) { camPrefs.putString("device_id", v); changed = true; }
        }
        if (web.hasArg("mot_cool")) {
            uint32_t v = web.arg("mot_cool").toInt();
            if (v >= 1 && v <= 300) { camPrefs.putUInt("mot_cool", v); changed = true; }
        }
        if (web.hasArg("per_min")) {
            uint32_t v = web.arg("per_min").toInt();
            if (v <= 1440) { camPrefs.putUInt("per_min", v); changed = true; }
        }
        if (web.hasArg("quality")) {
            uint32_t q = web.arg("quality").toInt();
            if (q >= 4 && q <= 63) {
                camPrefs.putUInt("quality", q);
                // Apply immediately to live sensor
                sensor_t* s = esp_camera_sensor_get();
                if (s) s->set_quality(s, q);
                changed = true;
            }
        }
        camPrefs.end();

        if (changed) lg("CFG", "Settings updated via web");
        web.sendHeader("Location", "/settings");
        web.send(303);
    });

    // ── /reboot ──────────────────────────────────────────────────
    web.on("/reboot", [](){
        web.send(200,"text/html","<html><body style='font-family:monospace;background:#0a0a0a;color:#ddd;padding:20px'>"
            "<p style='color:#fa0'>Rebooting in 2 seconds...</p></body></html>");
        delay(2000); ESP.restart();
    });

    web.begin();
    lg("WEB","http://"+WiFi.localIP().toString()+" — /settings to configure");
}

void lg(const String& m, const String& msg) {
    uint32_t t=millis()/1000;
    Serial.printf("[%02lu:%02lu:%02lu] %-8s %s\n",
        (t/3600)%24,(t/60)%60,t%60,m.c_str(),msg.c_str());
}
