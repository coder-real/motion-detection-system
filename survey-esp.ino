#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define FW_VERSION  "4.6.2"

// ══════════════ PINS ═══════════════════════════════════════
#define PIR_PIN         25
#define TRIG_PIN        26
#define ECHO_PIN        27
#define LED_PIN         23
#define BATTERY_PIN     35
#define GPS_RX          18
#define GPS_TX          19
#define GSM_RX          32
#define GSM_TX           4

// ══════════════ TIMING ══════════════════════════════════════
#define COOLDOWN_DEFAULT_MS    5000
#define STATUS_BROADCAST_MS   60000
#define PIR_DEBOUNCE_MS          150
#define LED_FLASH_MS              60
#define MDNS_TIMEOUT_MS         4000
// LAN HTTP round-trips complete in <30ms. 80ms gives 2.5× margin
// without being so long the scan appears frozen.
// 253 hosts × 80ms = ~20s worst case (previously 400ms = 101s).
#define SCAN_TIMEOUT_MS          300   // 253×300ms = ~75s worst case; typical <5s with last-IP cache
#define WIFI_PORTAL_TIMEOUT_S    180

// ══════════════ SENSOR PACKET ═════════════════════════════
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

static_assert(sizeof(SensorPacket) == 73,
    "SensorPacket must be 73 bytes — must match CAM firmware.");

SensorPacket pkt;

uint8_t camMAC[6]   = {0};
bool    camMacValid = false;

// ══════════════ OBJECTS ═════════════════════════════════════
HardwareSerial gpsSerial(1);
HardwareSerial gsmSerial(2);
TinyGPSPlus    gps;
Preferences    prefs;

// ══════════════ STATE ════════════════════════════════════════
char     deviceId[12]    = "UNIT_002";
char     phoneNumber[20] = "+2348129018208";
uint32_t alertCount      = 0;
uint32_t cooldownMs      = COOLDOWN_DEFAULT_MS;
int      espnowChannel   = 6;

volatile bool lastSendOk       = false;
unsigned long lastAlertMs      = 0;
unsigned long lastStatusMs     = 0;
unsigned long pirDebounceStart = 0;
bool          pirArmed         = false;

bool    gsmReady = false, espnowReady = false, gpsHasFix = false;
char    camIpHint[20] = "esp32cam-sentinel-2";
int8_t  gsmCSQ   = 99;

struct GPSCache {
    float lat=0, lng=0, alt=0, speed=0, hdop=99.9f;
    uint8_t sats=0;
    bool valid=false;
} gC;

// ══════════════ SEND CALLBACK ════════════════════════════════
void IRAM_ATTR onDataSent(const uint8_t *mac, esp_now_send_status_t s) {
    lastSendOk = (s == ESP_NOW_SEND_SUCCESS);
}

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println(F("\n╔════════════════════════════════════════╗"));
    Serial.println(F("║  ESP32 SENSOR HUB v4.6.1               ║"));
    Serial.println(F("║  WiFiManager + Fast Subnet Discovery    ║"));
    Serial.println(F("╚════════════════════════════════════════╝\n"));

    initGPIO();
    loadConfig();
    connectWiFi();

    espnowChannel = WiFi.channel();
    lg("CHAN", "Router channel: " + String(espnowChannel));

    discoverCamMac();

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    delay(100);
    lg("WIFI", "Disconnected — ESP-NOW mode only");

    initESPNOW();
    initUARTs();
    optimiseGPS();
    initGSMQuick();
    pirWarmup();

    strncpy(pkt.deviceId,  deviceId,   sizeof(pkt.deviceId)  - 1);
    strncpy(pkt.fwVersion, FW_VERSION, sizeof(pkt.fwVersion) - 1);
    pkt.espnowReady = espnowReady;

    printStatus();
    lg("SYS", "Ready v" + String(FW_VERSION) + " Ch." + String(espnowChannel));
    lg("SYS", "SensorPacket=" + String(sizeof(SensorPacket)) + " bytes");
}

// ══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    while (gpsSerial.available()) {
        if (gps.encode(gpsSerial.read())) updateGPSCache();
    }

    bool pirNow = (digitalRead(PIR_PIN) == HIGH);
    if (pirNow && !pirArmed)  { pirArmed = true; pirDebounceStart = now; }
    else if (!pirNow)           pirArmed = false;

    if (pirArmed
        && (now - pirDebounceStart >= PIR_DEBOUNCE_MS)
        && (now - lastAlertMs      >  cooldownMs))
    {
        lastAlertMs = now;
        handleMotion();
    }

    if (now - lastStatusMs >= STATUS_BROADCAST_MS) {
        lastStatusMs = now;
        buildPacket(1);
        if (espnowReady && camMacValid) {
            esp_now_send(camMAC, (uint8_t*)&pkt, sizeof(pkt));
            lg("STATUS", "Broadcast heap=" + String(pkt.freeHeap/1024)
                + "KB bat=" + String(pkt.batteryVoltage,1) + "V");
        }
    }

    while (gsmSerial.available()) Serial.write(gsmSerial.read());
    if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));
}

// ══════════════════════════════════════════════════════════════
//  WIFI — captive portal + optional CAM IP hint in portal
// ══════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(20);
    wm.setTitle("SENTINEL Hub Setup");

    // ── Pre-fill all fields from saved Preferences ────────────
    prefs.begin("cfg", false);
    String savedIp  = prefs.getString("camIp", "");
    String savedId  = prefs.getString("deviceId", "UNIT_002");
    String savedPh  = prefs.getString("phone", "+2348129018208");
    String savedCd  = String(prefs.getUInt("cooldown", COOLDOWN_DEFAULT_MS));
    prefs.end();
    savedIp.toCharArray(camIpHint, sizeof(camIpHint));

    // ── Custom portal fields ───────────────────────────────────
    WiFiManagerParameter camIpParam (
        "camip",  "CAM IP (leave blank to auto-scan)", camIpHint, 15);
    WiFiManagerParameter idParam(
        "devid",  "Device ID (max 11 chars)",          savedId.c_str(), 11);
    WiFiManagerParameter phoneParam(
        "phone",  "SMS phone number (+country code)",  savedPh.c_str(), 19);
    WiFiManagerParameter cdParam(
        "cd",     "Motion cooldown (ms)",              savedCd.c_str(), 8);

    wm.addParameter(&camIpParam);
    wm.addParameter(&idParam);
    wm.addParameter(&phoneParam);
    wm.addParameter(&cdParam);

    Serial.println(F("[WIFI] To change settings: run CLEARWIFI, connect to"));
    Serial.println(F("[WIFI] 'SENTINEL-HUB' AP → visit 192.168.4.1"));

    bool connected = wm.autoConnect("SENTINEL-HUB", "sentinel123");

    if (connected) {
        lg("WIFI", "Connected — IP=" + WiFi.localIP().toString()
            + " GW=" + WiFi.gatewayIP().toString()
            + " Ch=" + String(WiFi.channel())
            + " RSSI=" + String(WiFi.RSSI()) + "dBm");

        prefs.begin("cfg", false);

        // CAM IP hint
        String enteredIp = String(camIpParam.getValue()); enteredIp.trim();
        if (enteredIp.length() >= 7) {
            enteredIp.toCharArray(camIpHint, sizeof(camIpHint));
            prefs.putString("camIp", enteredIp);
            lg("WIFI", "CAM IP: " + enteredIp);
        }

        // Device ID
        String enteredId = String(idParam.getValue()); enteredId.trim();
        if (enteredId.length() > 0 && enteredId != savedId) {
            enteredId.toCharArray(deviceId, sizeof(deviceId));
            prefs.putString("deviceId", enteredId);
            lg("CFG", "Device ID → " + enteredId);
        }

        // Phone number
        String enteredPh = String(phoneParam.getValue()); enteredPh.trim();
        if (enteredPh.length() > 0 && enteredPh != savedPh) {
            enteredPh.toCharArray(phoneNumber, sizeof(phoneNumber));
            prefs.putString("phone", enteredPh);
            lg("CFG", "Phone → " + enteredPh);
        }

        // Cooldown
        String enteredCd = String(cdParam.getValue()); enteredCd.trim();
        uint32_t newCd = enteredCd.toInt();
        if (newCd >= 1000 && newCd != cooldownMs) {
            cooldownMs = newCd;
            prefs.putUInt("cooldown", cooldownMs);
            lg("CFG", "Cooldown → " + String(cooldownMs) + "ms");
        }

        prefs.end();
    } else {
        lg("WIFI", "Portal timed out — using saved settings");
        prefs.begin("cfg", false);
        espnowChannel = prefs.getInt("channel", 6);
        prefs.end();
    }
}

// ══════════════════════════════════════════════════════════════
//  MAC HELPERS
// ══════════════════════════════════════════════════════════════
bool parseMacString(const String& macStr, uint8_t* out) {
    if (macStr.length() < 17) return false;
    for (int i = 0; i < 6; i++)
        out[i] = (uint8_t)strtol(macStr.substring(i*3, i*3+2).c_str(), nullptr, 16);
    return true;
}

bool tryFetchMac(const String& ip) {
    HTTPClient http;
    http.begin("http://" + ip + "/status");
    http.setTimeout(SCAN_TIMEOUT_MS);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
    String mac = doc["ap_mac"] | "";
    if (mac.length() != 17) return false;
    if (!parseMacString(mac, camMAC)) return false;
    camMacValid = true;
    lg("DISC", "✅ MAC found at " + ip + " → " + mac);
    prefs.begin("cfg", false);
    prefs.putString("camMac", mac);
    prefs.putString("camIp",  ip);
    prefs.putInt("channel", espnowChannel);
    prefs.end();
    return true;
}

// ══════════════════════════════════════════════════════════════
//  CAM MAC DISCOVERY — 4 layers
//
//  Layer 0: Preferences cache          (instant)
//  Layer 1: Manual IP hint             (portal field / SETCAMIP)
//  Layer 2: mDNS esp32cam-sentinel     (fast, ~4s)
//  Layer 3: Subnet scan .1–.254        (~20s worst case)
//
//  IMPORTANT: Hub and CAM must be on the same WiFi network.
//  If the scan fails, check both devices joined the same router.
//  The CAM's AP is on the subnet of its STA IP.
//  We derive the scan base from WiFi.gatewayIP() so we always
//  scan the router's subnet regardless of the hub's own IP.
// ══════════════════════════════════════════════════════════════
void discoverCamMac() {
    // ── Layer 0: Cache ────────────────────────────────────────
    prefs.begin("cfg", false);
    String cached = prefs.getString("camMac", "");
    prefs.end();
    if (cached.length() == 17 && parseMacString(cached, camMAC)) {
        camMacValid = true;
        lg("DISC", "Cached MAC: " + cached);
        return;
    }

    // ── Layer 1: Manual hint ─────────────────────────────────
    String hint = String(camIpHint);
    hint.trim();
    if (hint.length() >= 7) {
        lg("DISC", "Trying IP hint: " + hint);
        if (tryFetchMac(hint)) return;
        lg("DISC", "Hint failed — continuing");
    }

    // ── Layer 2: mDNS ────────────────────────────────────────
    lg("DISC", "Trying mDNS (esp32cam-sentinel-2)...");
    MDNS.begin("sentinel-hub");
    IPAddress mIP = MDNS.queryHost("esp32cam-sentinel-2", MDNS_TIMEOUT_MS);
    if (mIP != INADDR_NONE) {
        lg("DISC", "mDNS → " + mIP.toString());
        if (tryFetchMac(mIP.toString())) return;
    } else {
        lg("DISC", "mDNS timed out");
    }

    // ── Layer 2.5: Last known IP (fast path before full scan) ──
    // CAM IP rarely changes once assigned by DHCP.
    // Trying the cached IP saves the full ~75s scan on most reboots.
    prefs.begin("cfg", false);
    String lastIp = prefs.getString("camIp", "");
    prefs.end();
    lastIp.trim();
    if (lastIp.length() >= 7) {
        lg("DISC", "Trying last known IP: " + lastIp);
        if (tryFetchMac(lastIp)) return;
        lg("DISC", "Last IP failed — running full scan");
    }

    // ── Layer 3: Subnet scan ─────────────────────────────────
    IPAddress gw   = WiFi.gatewayIP();
    IPAddress myIP = WiFi.localIP();
    String base = String(gw[0]) + "." + String(gw[1]) + "." + String(gw[2]) + ".";

    int hubOctet = (int)myIP[3];
    int gwOctet  = (int)gw[3];

    // 253 hosts × 80ms = ~20s worst case
    int estSecs = (253 * SCAN_TIMEOUT_MS) / 1000;
    lg("DISC", "Subnet scan " + base + "1-254  ~" + String(estSecs) + "s max");
    lg("DISC", "Hub=" + myIP.toString() + "  GW=" + gw.toString());
    lg("DISC", "Both devices must be on the same WiFi network.");

    int found = 0;
    for (int i = 1; i <= 254 && !found; i++) {
        if (i == hubOctet || i == gwOctet) continue;   // skip self + gateway

        // Progress log every 32 hosts so it doesn't look frozen
        if (i % 32 == 1) {
            lg("DISC", "Scanning " + base + String(i) + "–"
                + String(min(i+31, 254)) + "  heap=" + String(ESP.getFreeHeap()/1024) + "KB");
        }

        if (tryFetchMac(base + String(i))) {
            found = 1;
            break;
        }
        // Keep GPS parser alive during scan
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
    }

    if (!found) {
        lg("DISC", "⚠ CAM not found on " + base + "0/24");
        lg("DISC", "Checklist:");
        lg("DISC", "  1. CAM is powered and connected to same WiFi as this hub");
        lg("DISC", "  2. CAM runs firmware v10.7.1+ (has ap_mac in /status)");
        lg("DISC", "  3. CAM hostname SENTINEL-HUB portal matches router SSID");
        lg("DISC", "  Fixes: SETCAMIP <ip>  or  CLEARMAC + reboot");
    }
}

// ══════════════════════════════════════════════════════════════
//  ESP-NOW
// ══════════════════════════════════════════════════════════════
void initESPNOW() {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(espnowChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
        lg("ESPNOW", "Init FAILED"); espnowReady = false; return;
    }
    esp_now_register_send_cb(onDataSent);

    if (camMacValid) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, camMAC, 6);
        peer.channel = espnowChannel;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            lg("ESPNOW", "Add peer FAILED"); espnowReady = false; return;
        }
        lg("ESPNOW", "✅ Ready Ch." + String(espnowChannel));
        Serial.print("[ESPNOW] Targeting: ");
        for (int i=0;i<6;i++){Serial.printf("%02X",camMAC[i]);if(i<5)Serial.print(':');}
        Serial.println();
    } else {
        lg("ESPNOW", "Init OK but no peer — MAC not discovered");
    }
    espnowReady = true;
}

void addCamPeer() {
    esp_now_del_peer(camMAC);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, camMAC, 6);
    peer.channel = espnowChannel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// ══════════════════════════════════════════════════════════════
//  MOTION ALERT
// ══════════════════════════════════════════════════════════════
void handleMotion() {
    alertCount++;
    digitalWrite(LED_PIN, HIGH);
    lg("MOTION", "ALERT #" + String(alertCount));
    buildPacket(0);

    lg("GPS",  gC.valid
        ? String(pkt.latitude,6)+","+String(pkt.longitude,6)
          +" HDOP:"+String(pkt.hdop,2)+" sats:"+String(pkt.satellites)
        : "No fix (" + String(pkt.satellites) + " sats)");
    lg("DIST", String(pkt.distance_cm,1) + " cm");
    lg("BAT",  String(pkt.batteryVoltage,2) + " V");
    lg("HEAP", String(pkt.freeHeap/1024) + " KB");

    if (espnowReady && camMacValid) {
        esp_err_t r = esp_now_send(camMAC, (uint8_t*)&pkt, sizeof(pkt));
        lg("ESPNOW", r == ESP_OK ? "📡 Queued" : "❌ Error 0x" + String(r, HEX));
    } else if (!camMacValid) {
        lg("ESPNOW", "⚠ No MAC — run CLEARMAC to retry");
    }

    if (gsmReady) sendSMS() ? lg("SMS","✅ Sent") : lg("SMS","❌ Failed");

    prefs.begin("cfg", false);
    prefs.putUInt("alertCount", alertCount);
    prefs.end();

    delay(LED_FLASH_MS);
    digitalWrite(LED_PIN, LOW);
    Serial.println(F("────────────────────────────────────────\n"));
}

// ══════════════════════════════════════════════════════════════
//  BUILD PACKET
// ══════════════════════════════════════════════════════════════
void buildPacket(uint8_t type) {
    pkt.packetType    = type;
    pkt.alertId       = alertCount;
    pkt.uptimeSeconds = millis() / 1000;
    pkt.motion        = (type == 0);
    pkt.pirState      = (digitalRead(PIR_PIN) == HIGH);
    strncpy(pkt.deviceId,  deviceId,   sizeof(pkt.deviceId)  - 1);
    strncpy(pkt.fwVersion, FW_VERSION, sizeof(pkt.fwVersion) - 1);
    pkt.latitude   = gC.lat;   pkt.longitude  = gC.lng;
    pkt.altitude_m = gC.alt;   pkt.speed_kmh  = gC.speed;
    pkt.hdop       = gC.hdop;  pkt.satellites = gC.sats;
    pkt.gpsValid   = gC.valid ? 1 : 0;
    pkt.gpsHasFix  = gpsHasFix;
    pkt.distance_cm    = getDistance();
    pkt.batteryVoltage = readBattery();
    pkt.freeHeap       = ESP.getFreeHeap();
    pkt.minFreeHeap    = ESP.getMinFreeHeap();
    pkt.gsmSignalCSQ   = gsmCSQ;
    pkt.gsmReady       = gsmReady;
    pkt.espnowReady    = espnowReady;
}

// ══════════════════════════════════════════════════════════════
//  GPS
// ══════════════════════════════════════════════════════════════
void updateGPSCache() {
    gC.sats  = gps.satellites.isValid() ? gps.satellites.value() : 0;
    gC.valid = gps.location.isValid() && gps.location.age() < 2000;
    if (gps.location.isValid()) {
        gC.lat = gps.location.lat(); gC.lng = gps.location.lng();
        if (!gpsHasFix) { gpsHasFix=true; lg("GPS","First fix! sats="+String(gC.sats)); }
    }
    if (gps.altitude.isValid()) gC.alt   = gps.altitude.meters();
    if (gps.speed.isValid())    gC.speed = gps.speed.kmph();
    if (gps.hdop.isValid())     gC.hdop  = gps.hdop.hdop();
}

// ══════════════════════════════════════════════════════════════
//  optimiseGPS — safe baud negotiation with fallback
//
//  Problem with blind baud switching:
//    If $PMTK251,38400 is ignored (some clones, cold modules),
//    the GPS stays at 9600 while the ESP32 moves to 38400.
//    Result: pure garbage in, TinyGPS++ parses nothing, 0 sats.
//
//  Fix: after switching, listen for 1.5s. If no valid $GP bytes
//  arrive, fall back to 9600 and apply remaining commands there.
// ══════════════════════════════════════════════════════════════
bool gpsCheckData(uint32_t waitMs) {
    // Returns true if any byte that looks like start of NMEA arrives
    unsigned long deadline = millis() + waitMs;
    while (millis() < deadline) {
        if (gpsSerial.available()) {
            char c = gpsSerial.read();
            if (c == '$' || c == 0xB5) return true;  // NMEA or UBX
        }
    }
    return false;
}

void applyGPSTuning() {
    // Send these at whatever baud is currently active
    gpsSerial.println(F("$PMTK220,200*2C"));           // 5 Hz
    delay(80);
    gpsSerial.println(F("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28")); // GGA+RMC only
    delay(80);
    gpsSerial.println(F("$PMTK313,1*2E"));             // SBAS enable
    delay(60);
    gpsSerial.println(F("$PMTK301,2*2E"));             // SBAS WAAS
    delay(60);
    gpsSerial.println(F("$PMTK286,1*23"));             // AIC anti-interference
    delay(60);
    gpsSerial.println(F("$PMTK869,1,1*35"));           // Easy Mode (orbit predict)
    delay(60);
    gpsSerial.println(F("$PMTK225,0*2B"));             // Full-power continuous
    delay(60);
}

void optimiseGPS() {
    lg("GPS", "Negotiating baud rate...");

    // ── Step 1: Start at 9600 (factory default) ───────────────
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    delay(200);

    // Flush any garbage in buffer
    while (gpsSerial.available()) gpsSerial.read();

    // ── Step 2: Verify data is arriving at 9600 ───────────────
    if (!gpsCheckData(1500)) {
        lg("GPS", "⚠ No data at 9600 baud — check wiring (RX/TX, power)");
        lg("GPS", "  GPS_RX=pin " + String(GPS_RX) + "  GPS_TX=pin " + String(GPS_TX));
        // Stay at 9600, apply tuning anyway in case GPS is just slow to start
        applyGPSTuning();
        lg("GPS", "Running at 9600 baud (default)");
        return;
    }
    lg("GPS", "Data confirmed at 9600 baud");

    // ── Step 3: Request switch to 38400 ──────────────────────
    gpsSerial.println(F("$PMTK251,38400*27"));
    delay(150);   // module needs time to switch
    gpsSerial.flush();
    gpsSerial.end();

    // ── Step 4: Open port at 38400, check for data ────────────
    gpsSerial.begin(38400, SERIAL_8N1, GPS_RX, GPS_TX);
    delay(100);
    while (gpsSerial.available()) gpsSerial.read();   // flush

    if (gpsCheckData(1500)) {
        lg("GPS", "38400 baud confirmed — applying turbo settings");
        applyGPSTuning();
        lg("GPS", "Turbo: 38400baud / 5Hz / SBAS / AIC / Easy ✅");
    } else {
        // Module did not switch — fall back to 9600
        lg("GPS", "38400 failed — falling back to 9600 baud");
        gpsSerial.end();
        gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
        delay(100);
        applyGPSTuning();
        lg("GPS", "Running at 9600 baud / 5Hz / SBAS / AIC / Easy");
    }
}

// ══════════════════════════════════════════════════════════════
//  SENSORS
// ══════════════════════════════════════════════════════════════
float getDistance() {
    digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long us = pulseIn(ECHO_PIN, HIGH, 25000);
    return us ? us * 0.01715f : 0.0f;
}

float readBattery() {
    analogSetAttenuation(ADC_11db);
    uint32_t sum = 0;
    for (int i=0;i<8;i++) sum+=analogRead(BATTERY_PIN);
    return (sum/8.0f/4095.0f)*3.3f*2.0f;
}

// ══════════════════════════════════════════════════════════════
//  GSM
//
//  The SIM800L generates "OVER-VOLTAGE POWER DOWN" when Vcc
//  exceeds ~4.4V and resets itself. After reset the module
//  loses its AT session but gsmReady stays true in firmware.
//
//  Fix: probeGSM() sends a quick AT ping before every SMS.
//  If the module doesn't respond (post-reset), it re-runs the
//  full init sequence. SMS is only attempted after confirmed OK.
//
//  Hardware note: power SIM800L from a dedicated 3.7–4.2V LiPo
//  or an LDO regulator (e.g. AMS1117-3.8). Do NOT feed 5V
//  directly — the module will OVER-VOLTAGE and reset mid-send.
// ══════════════════════════════════════════════════════════════

// Probe the module and re-init if it reset since last contact.
// Returns true if module is alive and ready.
bool probeGSM() {
    // Flush anything the module printed (OVER-VOLTAGE messages etc.)
    while (gsmSerial.available()) gsmSerial.read();

    gsmSerial.println(F("AT"));
    unsigned long t = millis(); String r = "";
    while (millis()-t < 2000) {
        while (gsmSerial.available()) r += (char)gsmSerial.read();
    }
    if (r.indexOf("OK") >= 0) return true;   // alive

    // Module didn't respond — it reset. Re-initialise.
    lg("GSM", "Module offline (OVER-VOLTAGE reset?) — re-initialising");
    gsmReady = false;
    initGSMQuick();
    return gsmReady;
}

void initGSMQuick() {
    for (int i=0;i<4;i++) {
        gsmSerial.println(F("AT"));
        unsigned long t=millis(); String r="";
        while (millis()-t<2000){while(gsmSerial.available())r+=(char)gsmSerial.read();}
        if (r.indexOf("OK")>=0) {
            gsmSerial.println(F("ATE0")); delay(150); gsmSerial.readString();
            gsmSerial.println(F("AT+CMGF=1")); delay(150); gsmSerial.readString();
            gsmSerial.println(F("AT+CSQ")); delay(400);
            String cr=gsmSerial.readString();
            int idx=cr.indexOf("+CSQ:");
            if (idx>=0) gsmCSQ=cr.substring(idx+5,idx+8).toInt();
            gsmReady=true; lg("GSM","Ready CSQ="+String(gsmCSQ)); return;
        }
        delay(800);
    }
    lg("GSM","Not available");
}

bool sendSMS() {
    // Probe first — detect post-OVER-VOLTAGE reset silently
    if (!probeGSM()) {
        lg("GSM", "SMS skipped — module not responding after probe");
        return false;
    }

    // Set text mode and send
    gsmSerial.println(F("AT+CMGF=1")); delay(300); gsmSerial.readString();
    gsmSerial.print(F("AT+CMGS=\"")); gsmSerial.print(phoneNumber); gsmSerial.println(F("\""));

    // Wait up to 4s for '>' prompt (module can be slow after OVER-VOLTAGE recovery)
    unsigned long t=millis(); String prompt="";
    while (millis()-t < 4000) { while(gsmSerial.available()) prompt+=(char)gsmSerial.read(); }
    if (prompt.indexOf('>') < 0) {
        lg("GSM", "No prompt — module may have reset again");
        gsmReady = false;   // force re-probe on next alert
        return false;
    }

    gsmSerial.print(F("ALERT #")); gsmSerial.print(alertCount);
    gsmSerial.print(F(" [")); gsmSerial.print(deviceId); gsmSerial.print(F("]"));
    if (gC.valid){
        gsmSerial.print(F(" GPS:")); gsmSerial.print(pkt.latitude,5);
        gsmSerial.print(','); gsmSerial.print(pkt.longitude,5);
    }
    gsmSerial.print(F(" Dist:")); gsmSerial.print(pkt.distance_cm,0);
    gsmSerial.print(F("cm Bat:")); gsmSerial.print(pkt.batteryVoltage,1); gsmSerial.print('V');
    gsmSerial.write(26);   // Ctrl-Z = send

    // Wait up to 8s for +CMGS confirmation (network can be slow)
    t=millis(); String resp="";
    while (millis()-t < 8000) { while(gsmSerial.available()) resp+=(char)gsmSerial.read(); }

    bool ok = resp.indexOf("+CMGS") >= 0;
    if (!ok) {
        lg("GSM", "No +CMGS received — resp: " + resp.substring(0,40));
    }
    return ok;
}

// ══════════════════════════════════════════════════════════════
//  PIR WARMUP
// ══════════════════════════════════════════════════════════════
void pirWarmup() {
    lg("PIR","Warmup 15s — feeding GPS...");
    unsigned long start=millis(); int prevRem=16;
    while (millis()-start<15000) {
        while (gpsSerial.available()) if (gps.encode(gpsSerial.read())) updateGPSCache();
        int rem=15-(int)((millis()-start)/1000);
        if (rem<prevRem&&rem>0&&rem%5==0){
            prevRem=rem;
            lg("PIR",String(rem)+"s — sats:"+String(gC.sats));
            digitalWrite(LED_PIN,HIGH); delay(80); digitalWrite(LED_PIN,LOW);
        }
    }
    lg("PIR","Ready");
}

// ══════════════════════════════════════════════════════════════
//  INIT HELPERS
// ══════════════════════════════════════════════════════════════
void initGPIO() {
    pinMode(PIR_PIN,INPUT); pinMode(TRIG_PIN,OUTPUT);
    pinMode(ECHO_PIN,INPUT); pinMode(LED_PIN,OUTPUT);
    pinMode(BATTERY_PIN,INPUT); analogSetAttenuation(ADC_11db);
    digitalWrite(LED_PIN,LOW); digitalWrite(TRIG_PIN,LOW);
}

void initUARTs() {
    gpsSerial.begin(9600,SERIAL_8N1,GPS_RX,GPS_TX);
    gsmSerial.begin(9600,SERIAL_8N1,GSM_RX,GSM_TX);
    gsmSerial.setTimeout(1500);
}

// ══════════════════════════════════════════════════════════════
//  CONFIG
// ══════════════════════════════════════════════════════════════
void loadConfig() {
    prefs.begin("cfg",false);
    prefs.getString("deviceId","UNIT_002").toCharArray(deviceId,12);
    prefs.getString("phone","+2348129018208").toCharArray(phoneNumber,20);
    alertCount=prefs.getUInt("alertCount",0);
    cooldownMs=prefs.getUInt("cooldown",COOLDOWN_DEFAULT_MS);
    prefs.end();
    lg("CFG","Loaded — alertCount="+String(alertCount));
}

void saveConfig() {
    prefs.begin("cfg",false);
    prefs.putString("deviceId",deviceId);
    prefs.putString("phone",phoneNumber);
    prefs.putUInt("alertCount",alertCount);
    prefs.putUInt("cooldown",cooldownMs);
    prefs.end();
}

// ══════════════════════════════════════════════════════════════
//  STATUS PRINT
// ══════════════════════════════════════════════════════════════
void printStatus() {
    Serial.println(F("\n╔════════════════════════════════════════╗"));
    Serial.println(F("║         SYSTEM STATUS                  ║"));
    Serial.println(F("╚════════════════════════════════════════╝"));
    Serial.printf("Device:   %s  v%s\n",deviceId,FW_VERSION);
    Serial.printf("Cooldown: %lu ms\n",cooldownMs);
    Serial.printf("Alerts:   %lu\n",alertCount);
    Serial.printf("Battery:  %.2f V\n",readBattery());
    Serial.printf("Heap:     %lu KB\n",ESP.getFreeHeap()/1024);
    Serial.printf("GPS:      %s\n",gpsHasFix?"Fix":"Searching");
    Serial.printf("GSM CSQ:  %d\n",gsmCSQ);
    Serial.printf("ESP-NOW:  %s  Ch.%d\n",espnowReady?"Ready":"Failed",espnowChannel);
    Serial.print("CAM MAC:  ");
    if (camMacValid) {
        for (int i=0;i<6;i++){Serial.printf("%02X",camMAC[i]);if(i<5)Serial.print(':');}
        Serial.println(" ✅");
    } else { Serial.println("Not discovered ⚠"); }
    Serial.printf("Pkt size: %d bytes\n",(int)sizeof(SensorPacket));
    Serial.println(F("════════════════════════════════════════\n"));
}

// ══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ══════════════════════════════════════════════════════════════
void handleCommand(String cmd) {
    cmd.trim(); cmd.toUpperCase();
    if      (cmd=="STATUS")         printStatus();
    else if (cmd=="TEST")           lg("TEST","Dist:"+String(getDistance(),1)+"cm Bat:"+String(readBattery(),2)+"V Heap:"+String(ESP.getFreeHeap()/1024)+"KB GPS:"+String(gC.sats)+"sats");
    else if (cmd=="SEND")           { alertCount++; handleMotion(); }
    else if (cmd=="RESET")          { alertCount=0; saveConfig(); lg("CMD","Count reset"); }
    else if (cmd=="REBOOT")         ESP.restart();
    else if (cmd=="LEDON")          digitalWrite(LED_PIN,HIGH);
    else if (cmd=="LEDOFF")         digitalWrite(LED_PIN,LOW);
    else if (cmd=="CLEARMAC") {
        prefs.begin("cfg",false); prefs.remove("camMac"); prefs.end();
        camMacValid=false; memset(camMAC,0,6);
        lg("CMD","MAC cleared — reboot to re-discover");
    }
    else if (cmd=="CLEARWIFI") {
        WiFiManager wm; wm.resetSettings();
        lg("CMD","WiFi cleared — rebooting to portal...");
        delay(500); ESP.restart();
    }
    else if (cmd.startsWith("SETCAMIP ")) {
        String ip=cmd.substring(9); ip.trim();
        lg("CMD","Trying CAM IP: "+ip);
        if (WiFi.status()!=WL_CONNECTED) {
            ip.toCharArray(camIpHint,sizeof(camIpHint));
            prefs.begin("cfg",false); prefs.putString("camIp",ip); prefs.end();
            lg("CMD","IP hint saved — reboot to apply");
        } else {
            if (tryFetchMac(ip)) { addCamPeer(); lg("CMD","MAC set — ESP-NOW active"); }
            else lg("CMD","No CAM at "+ip+" — check firmware v10.7.1+");
        }
    }
    else if (cmd.startsWith("SETID "))    { cmd.substring(6).toCharArray(deviceId,12); saveConfig(); lg("CMD","ID="+String(deviceId)); }
    else if (cmd.startsWith("SETPHONE ")) { cmd.substring(9).toCharArray(phoneNumber,20); saveConfig(); lg("CMD","Phone="+String(phoneNumber)); }
    else if (cmd.startsWith("COOLDOWN ")) { cooldownMs=cmd.substring(9).toInt(); saveConfig(); lg("CMD","Cooldown="+String(cooldownMs)+"ms"); }
    else {
        Serial.println(F("Commands: STATUS TEST SEND RESET REBOOT LEDON LEDOFF"));
        Serial.println(F("          CLEARMAC CLEARWIFI SETCAMIP SETID SETPHONE COOLDOWN"));
    }
}

// ══════════════════════════════════════════════════════════════
//  LOGGER
// ══════════════════════════════════════════════════════════════
void lg(String m, String msg) {
    uint32_t t=millis()/1000;
    Serial.printf("[%02lu:%02lu:%02lu] %-8s %s\n",(t/3600)%24,(t/60)%60,t%60,m.c_str(),msg.c_str());
}
