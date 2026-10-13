/*
  PostureSense - Monitor node v2 (ESP32 + HC-SR04)
  Measures distance, decides "too close", uploads to Firebase Realtime Database.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>

// ===================== 1. SETTINGS (change these) =====================
const bool  LOCAL_ALERT   = true;    // LED/buzzer on THIS board
const bool  ENABLE_CLOUD  = true;    // upload to the database
const bool  ENABLE_ESPNOW = false;   // later: send distance to the wearable

const char* WIFI_SSID = "Dunith";                         // 2.4 GHz network
const char* WIFI_PASS = "dun12126";
const char* DB_URL    = "https://posturesense-monitor-default-rtdb.asia-southeast1.firebasedatabase.app";  // SAME database as wearable; no slash at end

float         DIST_MIN_CM       = 50.0;   // closer than this = too close
float         HYSTERESIS_CM     = 5.0;    // must move 5 cm further to clear
float         MAX_PRESENT_CM    = 120.0;  // further than this = nobody there
unsigned long TOO_CLOSE_SECONDS = 10;     // how long before alerting
float         SOUND_CM_PER_US   = 0.0343; // speed of sound (tune in Step 4 of the node guide)

unsigned long LATEST_EVERY_MS   = 3000;   // how often to overwrite /monitor/latest
unsigned long HISTORY_EVERY_MS  = 10000;  // how often to add a /monitor/history record

// ===================== 2. PINS =====================
const int PIN_TRIG = 26;
const int PIN_ECHO = 27;
const int PIN_LED  = 2;
const int PIN_BUZZ = 25;

// ===================== 3. VARIABLES =====================
bool tooClose = false, alertPrev = false;
unsigned long closeSince = 0, lastLoopMs = 0;
unsigned long lastLatestMs = 0, lastHistoryMs = 0, lastWifiTryMs = 0;
unsigned long alertCount = 0;
float tooCloseSec = 0;

uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
typedef struct { float distanceCm; } DistMsg;

// ===================== 4. DISTANCE (same as before) =====================
float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000);
  if (us == 0) return -1;
  float cm = us * SOUND_CM_PER_US / 2.0;
  if (cm < 2 || cm > 400) return -1;
  return cm;
}

float readMedianCm() {
  float v[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = readDistanceCm();
    if (d > 0) v[n++] = d;
    delay(60);
  }
  if (n < 3) return -1;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
  return v[n / 2];
}

// ===================== 5. WI-FI =====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) { delay(400); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" connected, IP ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FAILED (will keep retrying)");
  }
}

// ===================== 6. ESP-NOW (for later, off by default) =====================
void setupEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendDistance(float cm) {
  DistMsg m;
  m.distanceCm = cm;
  esp_now_send(BROADCAST_ADDR, (uint8_t*)&m, sizeof(m));
}

// ===================== 7. FIREBASE (REST over HTTPS) =====================
// method: "PUT" or "POST".  path: e.g. "/monitor/latest".  json: the body text.
bool firebaseSend(const char* method, const char* path, const char* json) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();                 // learning only: encrypts but does not verify the server
  HTTPClient http;
  http.setTimeout(5000);
  String url = String(DB_URL) + path + ".json";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, String(json));
  http.end();
  Serial.printf("  Firebase %s %s -> %d\n", method, path, code);
  return code == 200;
}

void putLatest(float d, const char* status) {
  char body[260];
  snprintf(body, sizeof(body),
    "{\"distance\":%.1f,\"status\":\"%s\",\"tooClose\":%s,\"alerts\":%lu,"
    "\"tooCloseSec\":%lu,\"uptime\":%lu,\"rssi\":%d,\"ts\":{\".sv\":\"timestamp\"}}",
    d, status, tooClose ? "true" : "false", alertCount,
    (unsigned long)tooCloseSec, millis() / 1000, WiFi.RSSI());
  firebaseSend("PUT", "/monitor/latest", body);
}

void postHistory(float d, const char* status) {
  char body[120];
  snprintf(body, sizeof(body),
    "{\"d\":%.1f,\"s\":\"%s\",\"ts\":{\".sv\":\"timestamp\"}}", d, status);
  firebaseSend("POST", "/monitor/history", body);
}

void postEvent(float d) {
  char body[120];
  snprintf(body, sizeof(body),
    "{\"type\":\"ALERT\",\"d\":%.1f,\"ts\":{\".sv\":\"timestamp\"}}", d);
  firebaseSend("POST", "/monitor/events", body);
}

// ===================== 8. SETUP AND LOOP =====================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_BUZZ, LOW);

  if (ENABLE_CLOUD) connectWiFi();
  else if (ENABLE_ESPNOW) WiFi.mode(WIFI_STA);
  if (ENABLE_ESPNOW) setupEspNow();

  lastLoopMs = millis();
  Serial.println("Monitor node v2 ready.");
}

void loop() {
  float d = readMedianCm();                       // about 0.35 s per reading
  unsigned long now = millis();
  float dt = (now - lastLoopMs) / 1000.0;
  lastLoopMs = now;

  bool present = (d > 0 && d <= MAX_PRESENT_CM);

  // --- too close, with hysteresis ---
  if (!present)                                          tooClose = false;
  else if (!tooClose && d < DIST_MIN_CM)                 tooClose = true;
  else if (tooClose && d > DIST_MIN_CM + HYSTERESIS_CM)  tooClose = false;

  // --- time rule ---
  if (tooClose) { if (closeSince == 0) closeSince = millis(); }
  else          { closeSince = 0; }
  bool alert = tooClose && (millis() - closeSince >= TOO_CLOSE_SECONDS * 1000UL);

  // --- statistics kept on the device ---
  if (tooClose) tooCloseSec += dt;
  if (alert && !alertPrev) {                      // an alert has just STARTED
    alertCount++;
    if (ENABLE_CLOUD) postEvent(d);
  }
  alertPrev = alert;

  // --- local indication ---
  if (LOCAL_ALERT) {
    digitalWrite(PIN_LED, tooClose ? HIGH : LOW);
    if (alert) { digitalWrite(PIN_BUZZ, HIGH); delay(150); digitalWrite(PIN_BUZZ, LOW); }
  }

  const char* status = !present ? "NO ONE" : (alert ? "ALERT" : (tooClose ? "TOO CLOSE" : "OK"));

  if (ENABLE_ESPNOW) sendDistance(d);

  // --- cloud ---
  if (ENABLE_CLOUD) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWifiTryMs > 10000) { lastWifiTryMs = millis(); WiFi.reconnect(); }
    } else {
      if (millis() - lastLatestMs >= LATEST_EVERY_MS)   { lastLatestMs = millis();  putLatest(d, status); }
      if (millis() - lastHistoryMs >= HISTORY_EVERY_MS) { lastHistoryMs = millis(); postHistory(d, status); }
    }
  }

  Serial.printf("distance=%.1f cm   status=%s   alerts=%lu\n", d, status, alertCount);
}
