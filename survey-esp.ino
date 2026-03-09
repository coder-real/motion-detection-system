// /*
//  * ESP32 SENSOR HUB - ESP-NOW Sender
//  * Version: 3.0.0
//  * 
//  * CHANGES:
//  * - Removed UART completely
//  * - ESP-NOW wireless communication
//  * - Binary packet transmission
//  * - Non-blocking operation
//  */

// #include <TinyGPSPlus.h>
// #include <HardwareSerial.h>
// #include <Preferences.h>
// #include <WiFi.h>
// #include <esp_now.h>
// #include <esp_wifi.h>

// #define VERSION "3.0.0"

// // ================== PINS ==================
// #define PIR_PIN         25
// #define TRIG_PIN        26
// #define ECHO_PIN        27
// #define LED_PIN         23
// #define BATTERY_PIN     35
// #define GPS_RX          18
// #define GPS_TX          19
// #define GSM_RX          32
// #define GSM_TX          4

// // ================== ESP-NOW PACKET ==================
// typedef struct {
//     bool motion;
//     float latitude;
//     float longitude;
//     float distance;
//     uint8_t satellites;
//     uint8_t gpsValid;
//     uint32_t alertId;
// } AlertPacket;

// AlertPacket outgoingPacket;

// // ESP32-CAM MAC Address
// uint8_t camMAC[] = {0x68, 0x25, 0xDD, 0x2D, 0xBC, 0xA4};

// // ================== OBJECTS ==================
// HardwareSerial gpsSerial(1);
// HardwareSerial gsmSerial(2);
// TinyGPSPlus gps;
// Preferences prefs;

// // ================== STATE ==================
// char deviceId[12] = "UNIT_001";
// char phoneNumber[20] = "+2348129018208";
// uint32_t alertCount = 0;
// uint32_t cooldown = 5000;
// unsigned long lastAlert = 0;

// bool gsmReady = false;
// bool espnowReady = false;
// bool gpsReady = false;
// bool lastSendSuccess = false;

// // ================== ESP-NOW CALLBACK ==================
// void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
//     lastSendSuccess = (status == ESP_NOW_SEND_SUCCESS);
//     Serial.print("[ESP-NOW] Send: ");
//     Serial.println(lastSendSuccess ? "✅ OK" : "❌ FAIL");
// }

// // ================== SETUP ==================
// void setup() {
//     Serial.begin(115200);
//     delay(1000);
    
//     Serial.println("\n╔════════════════════════════════════════╗");
//     Serial.println("║  ESP32 SENSOR HUB v" + String(VERSION) + "        ║");
//     Serial.println("║  ESP-NOW Wireless Architecture         ║");
//     Serial.println("╚════════════════════════════════════════╝\n");
    
//     initGPIO();
//     loadConfig();
//     initESPNOW();
//     initUARTs();
//     checkDevicesOnce();
//     performPIRWarmup();
    
//     log("SYSTEM", "✅ Ready - Motion detection active");
//     printSystemStatus();
    
//     Serial.println("\n════════════════════════════════════════");
//     Serial.println("Monitoring for motion...");
//     Serial.println("════════════════════════════════════════\n");
// }

// // ================== ESP-NOW INIT ==================
// void initESPNOW() {
//     WiFi.mode(WIFI_STA);
    
//     // Set WiFi channel to 1 (must match ESP32-CAM)
//     esp_wifi_set_promiscuous(true);
//     esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);   // Your router channel

//     esp_wifi_set_promiscuous(false);
    
//     Serial.print("ESP32 MAC: ");
//     Serial.println(WiFi.macAddress());
//     Serial.print("WiFi Channel: 1");
//     Serial.println();
    
//     if (esp_now_init() != ESP_OK) {
//         log("ESP-NOW", "❌ Init failed");
//         espnowReady = false;
//         return;
//     }
    
//     esp_now_register_send_cb(onDataSent);
    
//     esp_now_peer_info_t peerInfo = {};
//     memcpy(peerInfo.peer_addr, camMAC, 6);
//     peerInfo.channel = 6;  // CRITICAL: Must match ESP32-CAM channel
//     peerInfo.encrypt = false;
    
//     if (esp_now_add_peer(&peerInfo) != ESP_OK) {
//         log("ESP-NOW", "❌ Failed to add peer");
//         espnowReady = false;
//         return;
//     }
    
//     espnowReady = true;
//     log("ESP-NOW", "✅ Initialized on channel 1");
// }

// // ================== GPIO ==================
// void initGPIO() {
//     pinMode(PIR_PIN, INPUT);
//     pinMode(TRIG_PIN, OUTPUT);
//     pinMode(ECHO_PIN, INPUT);
//     pinMode(LED_PIN, OUTPUT);
//     pinMode(BATTERY_PIN, INPUT);
//     digitalWrite(LED_PIN, LOW);
//     digitalWrite(TRIG_PIN, LOW);
//     log("GPIO", "✅ Configured");
// }

// // ================== UART ==================
// void initUARTs() {
//     gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
//     log("UART", "✅ GPS Serial");
    
//     gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
//     log("UART", "✅ GSM Serial");
    
//     delay(1000);
// }

// // ================== DEVICE CHECK ==================
// void checkDevicesOnce() {
//     log("CHECK", "Testing devices...");
    
//     // GPS
//     Serial.print("  GPS: ");
//     unsigned long start = millis();
//     while (millis() - start < 3000) {
//         while (gpsSerial.available()) {
//             gps.encode(gpsSerial.read());
//         }
//     }
//     gpsReady = (gps.satellites.value() > 0);
//     Serial.println(gpsReady ? "✅ " + String(gps.satellites.value()) + " sats" : "⚠️ No fix");
    
//     // GSM
//     Serial.print("  GSM: ");
//     gsmReady = initGSM();
//     Serial.println(gsmReady ? "✅ Ready" : "⚠️ Not available");
    
//     Serial.print("  ESP-NOW: ");
//     Serial.println(espnowReady ? "✅ Connected" : "❌ Failed");
    
//     log("CHECK", "Complete");
// }

// // ================== LOOP ==================
// void loop() {
//     // Update GPS
//     while (gpsSerial.available()) {
//         gps.encode(gpsSerial.read());
//     }
    
//     // Commands
//     if (Serial.available()) {
//         handleCommand(Serial.readStringUntil('\n'));
//     }
    
//     // Motion detection
//     if (digitalRead(PIR_PIN) == HIGH && millis() - lastAlert > cooldown) {
//         handleMotion();
//     }

//     while (gsmSerial.available()) {
//     Serial.write(gsmSerial.read());
// }

    
//     delay(10);
// }

// // ================== MOTION HANDLER ==================
// void handleMotion() {
//     digitalWrite(LED_PIN, HIGH);
//     lastAlert = millis();
//     alertCount++;
    
//     log("MOTION", "🚨 ALERT #" + String(alertCount));
    
//     // Prepare packet
//     outgoingPacket.motion = true;
//     outgoingPacket.distance = getDistance();
//     outgoingPacket.alertId = alertCount;
    
//     if (gps.location.isValid()) {
//         outgoingPacket.latitude = gps.location.lat();
//         outgoingPacket.longitude = gps.location.lng();
//         outgoingPacket.satellites = gps.satellites.value();
//         outgoingPacket.gpsValid = 1;
//         log("GPS", String(outgoingPacket.latitude, 6) + ", " + String(outgoingPacket.longitude, 6));
//     } else {
//         outgoingPacket.latitude = 0;
//         outgoingPacket.longitude = 0;
//         outgoingPacket.satellites = gps.satellites.value();
//         outgoingPacket.gpsValid = 0;
//         log("GPS", "No fix (" + String(gps.satellites.value()) + " sats)");
//     }
    
//     log("DIST", String(outgoingPacket.distance, 1) + "cm");
    
//     // Send via ESP-NOW
//     if (espnowReady) {
//         esp_err_t result = esp_now_send(camMAC, (uint8_t *)&outgoingPacket, sizeof(outgoingPacket));
//         if (result == ESP_OK) {
//             log("ESP-NOW", "📡 Sent");
//         } else {
//             log("ESP-NOW", "❌ Send failed");
//         }
//     } else {
//         log("ESP-NOW", "⚠️ Not ready");
//     }
    
//     // Send SMS
//     if (gsmReady && sendSMS()) {
//         log("SMS", "✅ Sent");
//     } else {
//         log("SMS", "⏭️ Skipped");
//     }
    
//     // Save count
//     prefs.begin("cfg", false);
//     prefs.putUInt("alertCount", alertCount);
//     prefs.end();
    
//     digitalWrite(LED_PIN, LOW);
//     Serial.println("────────────────────────────────────────\n");
// }

// // ================== GSM ==================
// bool initGSM() {
//     Serial.println("[GSM] Initializing...");

//     gsmSerial.setTimeout(2000);

//     for (int i = 0; i < 5; i++) {
//         gsmSerial.println("AT");
//         String resp = gsmSerial.readString();
//         Serial.println("[GSM] AT -> " + resp);

//         if (resp.indexOf("OK") >= 0) {
//             gsmSerial.println("ATE0");      // disable echo
//             gsmSerial.readString();

//             gsmSerial.println("AT+CMGF=1"); // SMS text mode
//             String cmgf = gsmSerial.readString();
//             Serial.println("[GSM] CMGF -> " + cmgf);

//             gsmSerial.println("AT+CSQ");    // signal quality
//             String csq = gsmSerial.readString();
//             Serial.println("[GSM] CSQ -> " + csq);

//             gsmSerial.println("AT+CREG?");  // network registration
//             String creg = gsmSerial.readString();
//             Serial.println("[GSM] CREG -> " + creg);

//             return true;
//         }

//         delay(1000);
//     }

//     Serial.println("[GSM] ❌ No response");
//     return false;
// }


// bool sendSMS() {
//     Serial.println("[GSM] Sending SMS...");

//     gsmSerial.println("AT+CMGF=1");
//     delay(500);
//     Serial.println(gsmSerial.readString());

//     gsmSerial.print("AT+CMGS=\"");
//     gsmSerial.print(phoneNumber);
//     gsmSerial.println("\"");

//     delay(1000);
//     String prompt = gsmSerial.readString();
//     Serial.println("[GSM] Prompt: " + prompt);

//     if (prompt.indexOf(">") < 0) {
//         Serial.println("[GSM] ❌ No > prompt");
//         return false;
//     }

//     gsmSerial.print("ALERT #");
//     gsmSerial.print(alertCount);
//     gsmSerial.print(" ");
//     gsmSerial.print(deviceId);

//     if (outgoingPacket.gpsValid) {
//         gsmSerial.print(" ");
//         gsmSerial.print(outgoingPacket.latitude, 6);
//         gsmSerial.print(",");
//         gsmSerial.print(outgoingPacket.longitude, 6);
//     }

//     gsmSerial.write(26);  // CTRL+Z

//     String resp = gsmSerial.readString();
//     Serial.println("[GSM] SendResp: " + resp);

//     return resp.indexOf("+CMGS") >= 0;
// }


// // ================== SENSORS ==================
// float getDistance() {
//     digitalWrite(TRIG_PIN, LOW);
//     delayMicroseconds(2);
//     digitalWrite(TRIG_PIN, HIGH);
//     delayMicroseconds(10);
//     digitalWrite(TRIG_PIN, LOW);
    
//     long duration = pulseIn(ECHO_PIN, HIGH, 30000);
//     return duration ? duration * 0.034 / 2 : 0;
// }

// float readBattery() {
//     int raw = analogRead(BATTERY_PIN);
//     return (raw / 4095.0) * 3.3 * 2.0;
// }

// // ================== UTILITY ==================
// void performPIRWarmup() {
//     log("PIR", "Warming up (15s)...");
//     for (int i = 15; i > 0; i--) {
//         if (i % 5 == 0) {
//             Serial.print("  " + String(i) + "s... ");
//             digitalWrite(LED_PIN, HIGH);
//             delay(200);
//             digitalWrite(LED_PIN, LOW);
//         }
//         while (gpsSerial.available()) {
//             gps.encode(gpsSerial.read());
//         }
//         delay(1000);
//     }
//     log("PIR", "✅ Ready");
// }

// void printSystemStatus() {
//     Serial.println("\n╔════════════════════════════════════════╗");
//     Serial.println("║         SYSTEM STATUS                  ║");
//     Serial.println("╚════════════════════════════════════════╝");
//     Serial.print("Device ID: "); Serial.println(deviceId);
//     Serial.print("Phone: "); Serial.println(phoneNumber);
//     Serial.print("Alerts: "); Serial.println(alertCount);
//     Serial.print("Cooldown: "); Serial.print(cooldown); Serial.println("ms");
//     Serial.print("Battery: "); Serial.print(readBattery(), 2); Serial.println("V");
//     Serial.println("\n--- Devices ---");
//     Serial.print("GPS: "); Serial.println(gpsReady ? "✅" : "⚠️");
//     Serial.print("ESP-NOW: "); Serial.println(espnowReady ? "✅" : "❌");
//     Serial.print("GSM: "); Serial.println(gsmReady ? "✅" : "⚠️");
//     Serial.println("════════════════════════════════════════\n");
// }

// void log(String module, String message) {
//     char timestamp[12];
//     uint32_t t = millis() / 1000;
//     sprintf(timestamp, "%02lu:%02lu:%02lu", (t / 3600) % 24, (t / 60) % 60, t % 60);
//     Serial.print("["); Serial.print(timestamp); Serial.print("] ");
//     Serial.print(module); Serial.print(": "); Serial.println(message);
// }

// // ================== CONFIG ==================
// void loadConfig() {
//     prefs.begin("cfg", false);
//     String id = prefs.getString("deviceId", "UNIT_001");
//     id.toCharArray(deviceId, 12);
//     String phone = prefs.getString("phone", "+2348129018208");
//     phone.toCharArray(phoneNumber, 20);
//     alertCount = prefs.getUInt("alertCount", 0);
//     cooldown = prefs.getUInt("cooldown", 5000);
//     prefs.end();
//     log("CONFIG", "Loaded");
// }

// void saveConfig() {
//     prefs.begin("cfg", false);
//     prefs.putString("deviceId", deviceId);
//     prefs.putString("phone", phoneNumber);
//     prefs.putUInt("alertCount", alertCount);
//     prefs.putUInt("cooldown", cooldown);
//     prefs.end();
//     log("CONFIG", "Saved");
// }

// // ================== COMMANDS ==================
// void handleCommand(String cmd) {
//     cmd.trim();
//     cmd.toUpperCase();
    
//     if (cmd.startsWith("SETID ")) {
//         String id = cmd.substring(6);
//         id.toCharArray(deviceId, 12);
//         saveConfig();
//         log("CMD", "ID: " + id);
//     } else if (cmd.startsWith("SETPHONE ")) {
//         String phone = cmd.substring(9);
//         phone.toCharArray(phoneNumber, 20);
//         saveConfig();
//         log("CMD", "Phone: " + phone);
//     } else if (cmd.startsWith("COOLDOWN ")) {
//         cooldown = cmd.substring(9).toInt();
//         saveConfig();
//         log("CMD", "Cooldown: " + String(cooldown) + "ms");
//     } else if (cmd == "STATUS") {
//         printSystemStatus();
//     } else if (cmd == "TEST") {
//         testSensors();
//     } else if (cmd == "RESET") {
//         alertCount = 0;
//         saveConfig();
//         log("CMD", "Alert reset");
//     } else if (cmd == "CHECK") {
//         checkDevicesOnce();
//     } else if (cmd == "LEDON") {
//         digitalWrite(LED_PIN, HIGH);
//         log("CMD", "LED ON");
//     } else if (cmd == "LEDOFF") {
//         digitalWrite(LED_PIN, LOW);
//         log("CMD", "LED OFF");
//     } else {
//         Serial.println("Commands: STATUS, TEST, RESET, CHECK, LEDON, LEDOFF, SETID, SETPHONE, COOLDOWN");
//     }
// }

// void testSensors() {
//     log("TEST", "Distance: " + String(getDistance(), 1) + "cm");
//     log("TEST", "Battery: " + String(readBattery(), 2) + "V");
//     log("TEST", "PIR: " + String(digitalRead(PIR_PIN) ? "HIGH" : "LOW"));
//     log("TEST", "GPS Sats: " + String(gps.satellites.value()));
// }

/*
 * ESP32 SENSOR HUB - ESP-NOW Sender
 * Version: 3.1.0 (UPDATED)
 * 
 * CHANGES:
 * - Fixed channel synchronization with receiver (channel 6)
 * - Removed UART completely
 * - ESP-NOW wireless communication
 * - Binary packet transmission
 * - Non-blocking operation
 */

/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║  ESP32 SENSOR HUB — v4.1.0                              ║
 * ║  CRITICAL FIX: Auto-Detect WiFi Channel for ESP-NOW     ║
 * ╠══════════════════════════════════════════════════════════╣
 * ║  THE BUG (all 8 problems had this single source):       ║
 * ║  Hub was hardcoded to ESP-NOW Ch.6.                     ║
 * ║  Router assigned CAM to Ch.11.                          ║
 * ║  CAM's radio was locked to Ch.11 by the router.        ║
 * ║  Hub sent on Ch.6 → CAM heard nothing → no triggers.   ║
 * ║                                                          ║
 * ║  THE FIX:                                               ║
 * ║  Hub now connects to the same WiFi router briefly,      ║
 * ║  reads WiFi.channel() (the real router channel),        ║
 * ║  disconnects, then uses that channel for ESP-NOW.       ║
 * ║  This is fully automatic — no hardcoding needed.        ║
 * ║  Every boot, Hub auto-discovers the correct channel.    ║
 * ╚══════════════════════════════════════════════════════════╝
 */

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define VERSION         "4.1.0"

// ══════════════ WIFI (used ONLY to discover router channel) ══
// Hub connects briefly at boot, reads the channel, then disconnects.
// It does NOT stay connected — all data goes via ESP-NOW, not WiFi.
const char* WIFI_SSID = "ULTRA Network";
const char* WIFI_PASS = "&12345@100%";

// ══════════════ PINS ═════════════════════════════════════════
#define PIR_PIN      25
#define TRIG_PIN     26
#define ECHO_PIN     27
#define LED_PIN      23
#define BATTERY_PIN  35
#define GPS_RX       18
#define GPS_TX       19
#define GSM_RX       32
#define GSM_TX        4

// ══════════════ TIMING ═══════════════════════════════════════
#define MOTION_COOLDOWN_MS  5000UL
#define STATUS_INTERVAL_MS 60000UL   // send type-1 status every 60s

// ══════════════ SENSOR PACKET ════════════════════════════════
// CRITICAL: Must be byte-for-byte identical to CAM v9.3 SensorPacket.
// If you change one side you MUST change the other.
// CAM will log the expected size — if it prints a mismatch error,
// compare both struct definitions carefully.
typedef struct __attribute__((packed)) {
    uint8_t  packetType;      // 0=motion alert, 1=status update
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

SensorPacket pkt;

// CAM MAC address — STATION MAC from CAM serial log line:
// "[ESPNOW] CAM STA MAC: XX:XX:XX:XX:XX:XX"
uint8_t camMAC[] = {0x68, 0x25, 0xDD, 0x2D, 0xBC, 0xA4};

// ══════════════ STATE ════════════════════════════════════════
HardwareSerial gpsSerial(1);
HardwareSerial gsmSerial(2);
TinyGPSPlus    gps;
Preferences    prefs;

char     deviceId[12]   = "UNIT_001";
char     phoneNumber[20]= "+2348129018208";
uint32_t alertCount     = 0;
uint32_t cooldown       = MOTION_COOLDOWN_MS;
uint32_t minFreeHeap    = 0xFFFFFFFF;

int      espnowChannel  = 1;    // set after WiFi discovery
bool     gsmReady       = false;
bool     espnowOK       = false;
bool     lastSendOK     = false;
bool     gpsReady       = false;

unsigned long lastAlert  = 0;
unsigned long lastStatus = 0;

// ══════════════ ESP-NOW SEND CALLBACK ════════════════════════
void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    lastSendOK = (status == ESP_NOW_SEND_SUCCESS);
    if (lastSendOK) Serial.println("[ESPNOW] ✅ Packet delivered");
    else            Serial.println("[ESPNOW] ❌ Delivery failed");
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println(F("\n╔════════════════════════════════════════╗"));
    Serial.println(F("║  ESP32 SENSOR HUB v4.1.0               ║"));
    Serial.println(F("║  Auto-Channel ESP-NOW Fix               ║"));
    Serial.println(F("╚════════════════════════════════════════╝\n"));

    initGPIO();
    loadConfig();

    // ── CRITICAL: Discover router's WiFi channel ─────────────
    // Hub connects, reads channel, disconnects.
    // This ensures Hub and CAM both use the exact same channel.
    discoverWifiChannel();

    initESPNOW();   // uses espnowChannel discovered above
    initUARTs();
    checkDevices();
    performPIRWarmup();

    // Fill static packet fields
    pkt.packetType = 0;
    strncpy(pkt.deviceId,  deviceId, sizeof(pkt.deviceId));
    strncpy(pkt.fwVersion, VERSION,  sizeof(pkt.fwVersion));
    pkt.espnowReady = espnowOK;

    printStatus();
    log("SYS","✅ Ready — motion monitoring active (Ch." + String(espnowChannel) + ")");
    log("SYS","SensorPacket size: " + String(sizeof(SensorPacket)) + " bytes (must match CAM)");
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // Feed GPS parser
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

    // Serial console commands
    if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));

    // GSM passthrough
    while (gsmSerial.available()) Serial.write(gsmSerial.read());

    // Track minimum heap
    uint32_t h = ESP.getFreeHeap();
    if (h < minFreeHeap) minFreeHeap = h;

    // Motion alert
    if (digitalRead(PIR_PIN) == HIGH && (now - lastAlert) > cooldown) {
        handleMotion();
    }

    // Periodic status update (type=1) — keeps CAM heartbeat fresh
    if (now - lastStatus > STATUS_INTERVAL_MS) {
        lastStatus = now;
        sendStatusPacket();
    }
}

// ═══════════════════════════════════════════════════════════════
//  DISCOVER WIFI CHANNEL — THE KEY FIX
//  Hub connects to the same router the CAM uses, reads the
//  actual channel the router operates on, then disconnects.
//  This is done ONCE at boot and takes ~5-10 seconds.
// ═══════════════════════════════════════════════════════════════
void discoverWifiChannel() {
    Serial.println(F("[CHAN] Connecting to WiFi to discover router channel..."));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 20) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        espnowChannel = WiFi.channel();   // ← actual router channel
        Serial.printf("[CHAN] ✅ Router is on channel %d — ESP-NOW will use Ch.%d\n",
                      espnowChannel, espnowChannel);
    } else {
        espnowChannel = 6;   // fallback if WiFi unavailable
        Serial.printf("[CHAN] ⚠ WiFi connect failed — using fallback Ch.%d\n", espnowChannel);
        Serial.println(F("[CHAN]   Check WIFI_SSID and WIFI_PASS match your router"));
    }

    // Disconnect WiFi — Hub does not need it, only used ESP-NOW
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);    // keep in STA mode for ESP-NOW
    delay(100);

    Serial.println(F("[CHAN] WiFi disconnected — ESP-NOW mode only"));
}

// ═══════════════════════════════════════════════════════════════
//  MOTION ALERT — packet type 0
// ═══════════════════════════════════════════════════════════════
void handleMotion() {
    digitalWrite(LED_PIN, HIGH);
    lastAlert = millis();
    alertCount++;

    log("MOTION","🚨 ALERT #" + String(alertCount));

    fillSharedFields();
    pkt.packetType  = 0;
    pkt.motion      = true;
    pkt.pirState    = true;
    pkt.alertId     = alertCount;
    pkt.distance_cm = getDistance();

    log("DIST","  " + String(pkt.distance_cm, 1) + " cm");
    log("GPS", pkt.gpsValid
        ? "  " + String(pkt.latitude,6) + ", " + String(pkt.longitude,6)
          + " (" + String(pkt.satellites) + " sats)"
        : "  No fix");
    log("BAT", "  " + String(pkt.batteryVoltage,2) + " V");
    log("HEAP","  " + String(pkt.freeHeap/1024) + " KB free");

    if (espnowOK) sendPacket();

    if (gsmReady) {
        if (sendSMS()) log("SMS","✅ Sent");
        else           log("SMS","❌ Failed");
    }

    prefs.begin("cfg", false);
    prefs.putUInt("alertCount", alertCount);
    prefs.end();

    digitalWrite(LED_PIN, LOW);
    Serial.println(F("────────────────────────────────────────\n"));
}

// ═══════════════════════════════════════════════════════════════
//  STATUS UPDATE — packet type 1
// ═══════════════════════════════════════════════════════════════
void sendStatusPacket() {
    fillSharedFields();
    pkt.packetType  = 1;
    pkt.motion      = false;
    pkt.pirState    = (bool)digitalRead(PIR_PIN);
    pkt.distance_cm = getDistance();

    log("STATUS","Sending telemetry → CAM "
        "heap=" + String(pkt.freeHeap/1024) + "KB "
        "bat=" + String(pkt.batteryVoltage,2) + "V "
        "sats=" + String(pkt.satellites) + " "
        "ch=" + String(espnowChannel)
    );

    if (espnowOK) sendPacket();
}

// ═══════════════════════════════════════════════════════════════
//  FILL SHARED FIELDS
// ═══════════════════════════════════════════════════════════════
void fillSharedFields() {
    pkt.uptimeSeconds = millis() / 1000;
    pkt.freeHeap      = ESP.getFreeHeap();
    pkt.minFreeHeap   = minFreeHeap;
    pkt.batteryVoltage= readBattery();
    pkt.gsmReady      = gsmReady;
    pkt.gsmSignalCSQ  = gsmReady ? readGsmCSQ() : -1;
    pkt.espnowReady   = espnowOK;

    // GPS
    if (gps.location.isValid() && gps.location.age() < 3000) {
        pkt.latitude  = gps.location.lat();
        pkt.longitude = gps.location.lng();
        pkt.gpsValid  = 1;
        pkt.gpsHasFix = true;
    } else {
        pkt.latitude  = 0.0f;
        pkt.longitude = 0.0f;
        pkt.gpsValid  = 0;
        pkt.gpsHasFix = false;
    }

    pkt.altitude_m = gps.altitude.isValid()   ? gps.altitude.meters()  : 0.0f;
    pkt.speed_kmh  = gps.speed.isValid()      ? gps.speed.kmph()       : 0.0f;
    pkt.hdop       = gps.hdop.isValid()       ? gps.hdop.hdop()        : 99.0f;
    pkt.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
}

// ═══════════════════════════════════════════════════════════════
//  SEND VIA ESP-NOW
// ═══════════════════════════════════════════════════════════════
void sendPacket() {
    esp_err_t result = esp_now_send(camMAC, (uint8_t*)&pkt, sizeof(pkt));
    if (result == ESP_OK) {
        log("ESPNOW","📡 Queued type=" + String(pkt.packetType) + " size=" + String(sizeof(pkt)) + "B");
    } else {
        log("ESPNOW","❌ Send error 0x" + String(result, HEX)
            + " — check CAM MAC and channel");
    }
}

// ═══════════════════════════════════════════════════════════════
//  ESP-NOW INIT — uses channel discovered from WiFi
// ═══════════════════════════════════════════════════════════════
void initESPNOW() {
    // Set the radio to the discovered channel before init
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(espnowChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
        log("ESPNOW","❌ Init failed");
        espnowOK = false;
        return;
    }

    esp_now_register_send_cb(onDataSent);

    // Add CAM as peer
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, camMAC, 6);
    peer.channel = espnowChannel;   // ← matches router channel
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        log("ESPNOW","❌ Failed to add peer");
        espnowOK = false;
        return;
    }

    espnowOK = true;
    log("ESPNOW","✅ Ready — Ch." + String(espnowChannel));

    Serial.print("[ESPNOW] Hub MAC: ");
    Serial.println(WiFi.macAddress());

    Serial.print("[ESPNOW] Targeting CAM MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", camMAC[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
}

// ═══════════════════════════════════════════════════════════════
//  GPIO
// ═══════════════════════════════════════════════════════════════
void initGPIO() {
    pinMode(PIR_PIN,     INPUT);
    pinMode(TRIG_PIN,    OUTPUT);
    pinMode(ECHO_PIN,    INPUT);
    pinMode(LED_PIN,     OUTPUT);
    pinMode(BATTERY_PIN, INPUT);
    digitalWrite(LED_PIN,  LOW);
    digitalWrite(TRIG_PIN, LOW);
    log("GPIO","✅ Configured");
}

// ═══════════════════════════════════════════════════════════════
//  UARTs
// ═══════════════════════════════════════════════════════════════
void initUARTs() {
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    gsmSerial.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);
    delay(1000);
    log("UART","GPS + GSM started");
}

// ═══════════════════════════════════════════════════════════════
//  DEVICE CHECK
// ═══════════════════════════════════════════════════════════════
void checkDevices() {
    // GPS
    Serial.print("  GPS: ");
    unsigned long start = millis();
    while (millis() - start < 3000) {
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
    }
    gpsReady = (gps.satellites.value() > 0);
    Serial.println(gpsReady ? "✅ " + String(gps.satellites.value()) + " sats" : "⚠️ No fix yet (normal on first boot)");

    // GSM
    Serial.print("  GSM: ");
    gsmReady = initGSM();
    Serial.println(gsmReady ? "✅ Ready" : "⚠️ Not available");

    // ESP-NOW
    Serial.print("  ESP-NOW: ");
    Serial.println(espnowOK ? "✅ Ch." + String(espnowChannel) : "❌ Init failed");
}

// ═══════════════════════════════════════════════════════════════
//  SENSORS
// ═══════════════════════════════════════════════════════════════
float getDistance() {
    digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long dur = pulseIn(ECHO_PIN, HIGH, 30000);
    return dur ? dur * 0.034f / 2.0f : 0.0f;
}

float readBattery() {
    int raw = analogRead(BATTERY_PIN);
    return (raw / 4095.0f) * 3.3f * 2.0f;
}

int8_t readGsmCSQ() {
    gsmSerial.println("AT+CSQ");
    delay(300);
    String resp = gsmSerial.readString();
    int idx = resp.indexOf("+CSQ: ");
    if (idx < 0) return -1;
    return (int8_t)resp.substring(idx + 6).toInt();
}

// ═══════════════════════════════════════════════════════════════
//  GSM
// ═══════════════════════════════════════════════════════════════
bool initGSM() {
    gsmSerial.setTimeout(2000);
    for (int i = 0; i < 5; i++) {
        gsmSerial.println("AT");
        String resp = gsmSerial.readString();
        if (resp.indexOf("OK") >= 0) {
            gsmSerial.println("ATE0"); gsmSerial.readString();
            gsmSerial.println("AT+CMGF=1"); gsmSerial.readString();
            return true;
        }
        delay(1000);
    }
    return false;
}

bool sendSMS() {
    gsmSerial.println("AT+CMGF=1");
    delay(500); gsmSerial.readString();
    gsmSerial.print("AT+CMGS=\""); gsmSerial.print(phoneNumber); gsmSerial.println("\"");
    delay(1000);
    String prompt = gsmSerial.readString();
    if (prompt.indexOf(">") < 0) return false;

    gsmSerial.print("ALERT #");
    gsmSerial.print(alertCount);
    gsmSerial.print(" ");
    gsmSerial.print(deviceId);
    if (pkt.gpsValid) {
        gsmSerial.print(" ");
        gsmSerial.print(pkt.latitude, 6);
        gsmSerial.print(",");
        gsmSerial.print(pkt.longitude, 6);
    }
    gsmSerial.write(26);

    String resp = gsmSerial.readString();
    return resp.indexOf("+CMGS") >= 0;
}

// ═══════════════════════════════════════════════════════════════
//  PIR WARMUP
// ═══════════════════════════════════════════════════════════════
void performPIRWarmup() {
    log("PIR","Warming up (15s)...");
    for (int i = 15; i > 0; i--) {
        if (i % 5 == 0) {
            Serial.print("  " + String(i) + "s... ");
            digitalWrite(LED_PIN, HIGH); delay(100); digitalWrite(LED_PIN, LOW);
        }
        while (gpsSerial.available()) gps.encode(gpsSerial.read());
        delay(1000);
    }
    log("PIR","✅ Ready");
}

// ═══════════════════════════════════════════════════════════════
//  STATUS PRINT
// ═══════════════════════════════════════════════════════════════
void printStatus() {
    Serial.println(F("\n╔════════════════════════════════════════╗"));
    Serial.println(F("║         SYSTEM STATUS                  ║"));
    Serial.println(F("╚════════════════════════════════════════╝"));
    Serial.printf("Device ID:      %s\n",       deviceId);
    Serial.printf("Firmware:       %s\n",       VERSION);
    Serial.printf("Phone:          %s\n",       phoneNumber);
    Serial.printf("Alerts:         %lu\n",      alertCount);
    Serial.printf("Cooldown:       %lu ms\n",   cooldown);
    Serial.printf("Free Heap:      %lu bytes\n",ESP.getFreeHeap());
    Serial.printf("Battery:        %.2f V\n",   readBattery());
    Serial.printf("GPS:            %s\n",       gpsReady ? "✅" : "⚠️ No fix");
    Serial.printf("GSM:            %s\n",       gsmReady ? "✅" : "⚠️");
    Serial.printf("ESP-NOW Ch:     %d  %s\n",   espnowChannel, espnowOK ? "✅" : "❌");
    Serial.printf("SensorPkt size: %d bytes\n", (int)sizeof(SensorPacket));
    Serial.print("CAM MAC target: ");
    for (int i = 0; i < 6; i++) { Serial.printf("%02X", camMAC[i]); if(i<5) Serial.print(":"); }
    Serial.println(F("\n════════════════════════════════════════\n"));
}

// ═══════════════════════════════════════════════════════════════
//  CONFIG
// ═══════════════════════════════════════════════════════════════
void loadConfig() {
    prefs.begin("cfg", false);
    prefs.getString("deviceId",   deviceId,     sizeof(deviceId));
    prefs.getString("phone",      phoneNumber,  sizeof(phoneNumber));
    alertCount = prefs.getUInt("alertCount", 0);
    cooldown   = prefs.getUInt("cooldown",   MOTION_COOLDOWN_MS);
    prefs.end();
    log("CFG","Loaded (alertCount=" + String(alertCount) + ")");
}

void saveConfig() {
    prefs.begin("cfg", false);
    prefs.putString("deviceId",  deviceId);
    prefs.putString("phone",     phoneNumber);
    prefs.putUInt("alertCount",  alertCount);
    prefs.putUInt("cooldown",    cooldown);
    prefs.end();
    log("CFG","Saved");
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
void handleCommand(String cmd) {
    cmd.trim(); cmd.toUpperCase();

    if (cmd == "STATUS")   printStatus();
    else if (cmd == "TEST")     testSensors();
    else if (cmd == "SEND")     { sendStatusPacket(); log("CMD","Status sent"); }
    else if (cmd == "RESET")    { alertCount = 0; saveConfig(); log("CMD","Alert count reset"); }
    else if (cmd == "CHANNEL")  { Serial.printf("[CMD] ESP-NOW Channel: %d\n", espnowChannel); }
    else if (cmd == "LEDON")    { digitalWrite(LED_PIN, HIGH); log("CMD","LED ON"); }
    else if (cmd == "LEDOFF")   { digitalWrite(LED_PIN, LOW);  log("CMD","LED OFF"); }
    else if (cmd.startsWith("SETID "))    { cmd.substring(6).toCharArray(deviceId, 12); saveConfig(); log("CMD","ID=" + String(deviceId)); }
    else if (cmd.startsWith("SETPHONE ")) { cmd.substring(9).toCharArray(phoneNumber, 20); saveConfig(); log("CMD","Phone=" + String(phoneNumber)); }
    else if (cmd.startsWith("COOLDOWN ")) { cooldown = cmd.substring(9).toInt(); saveConfig(); log("CMD","Cooldown=" + String(cooldown) + "ms"); }
    else {
        Serial.println("Commands: STATUS | TEST | SEND | RESET | CHANNEL | LEDON | LEDOFF | SETID x | SETPHONE x | COOLDOWN x");
    }
}

void testSensors() {
    log("TEST","Distance: " + String(getDistance(), 1) + " cm");
    log("TEST","Battery:  " + String(readBattery(), 2) + " V");
    log("TEST","PIR:      " + String(digitalRead(PIR_PIN) ? "HIGH" : "LOW"));
    log("TEST","GPS sats: " + String(gps.satellites.value()));
    log("TEST","Heap:     " + String(ESP.getFreeHeap()/1024) + " KB");
    log("TEST","Channel:  " + String(espnowChannel));
    if (gps.location.isValid())
        log("TEST","GPS pos:  " + String(gps.location.lat(),6) + ", " + String(gps.location.lng(),6));
}

void log(String m, String msg) {
    uint32_t t = millis()/1000;
    Serial.printf("[%02lu:%02lu:%02lu] %s: %s\n",(t/3600)%24,(t/60)%60,t%60,m.c_str(),msg.c_str());
}
