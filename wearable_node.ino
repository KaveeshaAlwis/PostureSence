#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ===================== 1) EDIT THESE WHEN RUN =====================
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* DB_URL    = "https://posturesense-monitor-default-rtdb.asia-southeast1.firebasedatabase.app";  // SAME database as monitor node; no slash at end

// ---- Telegram (ADDED ONLY; existing Firebase/sensor logic is unchanged) ----
const bool  ENABLE_TELEGRAM = true;
const char* TG_BOT_TOKEN    = "";
const char* TG_CHAT_ID      = "";
// =========================================================

// ===================== TEST SWITCHES =====================
//   USE_MPU  1 = MPU6050 is used          0 = MPU6050 part is switched off (no I2C at all)
//   USE_FLEX 1 = flex sensor is used      0 = flex sensor part is switched off
//
//   Flex sensor alone  :  USE_MPU 0 , USE_FLEX 1     
//   MPU6050 alone      :  USE_MPU 1 , USE_FLEX 0
//   Both (final device):  USE_MPU 1 , USE_FLEX 1
#define USE_MPU  1
#define USE_FLEX 1
// =========================================================

// ---- pins (unchanged) ----
const int PIN_SDA = 21, PIN_SCL = 22, PIN_FLEX = 34;
const int PIN_BUZZ = 25, PIN_VIBE = 26, PIN_BTN = 27, PIN_LED = 2;
const int MPU_ADDR = 0x68;

// ---- fine tuning ----
const uint32_t ALERT_REPEAT_MS    = 5000;   // repeat the 3-beep alert this often while still bad
const float    MIN_LEAN_DEG       = 10.0f;  // the guided lean must be at least this big
const float    FORWARD_SIGN_FALLBACK = 1.0f; // only used if the guided lean failed AND nothing was learned

// ---- user settings (overwritten by the app every 10 s) ----
// NOTE: these are only the starting values. As soon as the app has a /settings node in the
// database, those numbers REPLACE these ones every 10 s. Check what is stored there.
struct Settings {
  float neckThr = 15;        // degrees of forward lean allowed (raise to ~20 once it works)
  float spineThr = 12;       // % change of flex reading allowed
  int   holdSec = 7;         // bad posture must last this long before alert
  int   sessionLimitMin = 30;
  bool  useBuzzer = true;
  bool  useVibe = true;
};
Settings cfg;

// ---- values shared between the two cores ----
volatile float    g_neck = 0, g_spine = 0;
volatile bool     g_bad = false, g_session = false;
volatile uint32_t g_sessionSec = 0;
volatile uint16_t g_goodSec = 0, g_badSec = 0;  // seconds counted for the current 1-minute summary

struct Event { char type; char text[90]; };      // P = posture, S = session limit
QueueHandle_t evQ;

// ---- sensing state ----
float g0[3]    = {0, 0, 1};   // direction of gravity when you sit upright (unit vector, sensor axes)
float axisL[3] = {1, 0, 0};   // the sideways axis your neck bends around (learned)
bool  axisKnown = false;
float gF[3]    = {0, 0, 1};   // filtered direction of gravity right now
float flexBase = 1500, neckF = 0, spineF = 0;
float flexNow = 0;            // latest flex reading (ADC counts), printed so you can watch it
bool  filtInit = false;
uint8_t mpuFails = 0;
bool  mpuFault = false;

// ---- alert state ----
uint32_t badHold = 0, goodHold = 0, lastPostureAlertMs = 0, lastSessAlertSec = 0;
bool episodeSent = false;
bool neckBad = false, spineBad = false;
uint32_t settleUntilMs = 0;   // status is forced "good" until this time, right after calibration

// ---- non-blocking beep/vibrate pattern ----
struct Pattern { bool active = false; uint8_t pulses = 0; uint16_t onMs = 0, offMs = 0; uint32_t start = 0; } pat;

void startPattern(uint8_t pulses, uint16_t onMs, uint16_t offMs) {
  pat.pulses = pulses; pat.onMs = onMs; pat.offMs = offMs; pat.start = millis(); pat.active = true;
}
void setOutputs(bool on) {
  digitalWrite(PIN_BUZZ, (on && cfg.useBuzzer) ? HIGH : LOW);
  digitalWrite(PIN_VIBE, (on && cfg.useVibe)   ? HIGH : LOW);
}
void updatePattern() {
  if (!pat.active) return;
  uint32_t t = millis() - pat.start;
  uint32_t period = pat.onMs + pat.offMs;
  if (period == 0 || t / period >= pat.pulses) { setOutputs(false); pat.active = false; return; }
  setOutputs((t % period) < pat.onMs);
}
// blocking beeps - only used while calibrating (the loop is paused then anyway)
void cue(uint8_t n, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < n; i++) { setOutputs(true); delay(onMs); setOutputs(false); delay(offMs); }
}

// ---------------- small vector helpers (3 numbers = a direction) ----------------
static inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void cross3(const float a[3], const float b[3], float o[3]) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}
static bool norm3(float v[3]) {
  float m = sqrtf(dot3(v, v));
  if (m < 1e-6f) return false;
  v[0] /= m; v[1] /= m; v[2] /= m;
  return true;
}
static float angleDeg(const float a[3], const float b[3]) {
  float c = dot3(a, b);
  if (c > 1) c = 1;
  if (c < -1) c = -1;
  return acosf(c) * 57.29578f;
}

// ---------------- MPU6050 ----------------
bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}
bool mpuInit() {
  Wire.end();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);                       // never hang on a stuck bus
  if (!mpuWrite(0x6B, 0x01)) return false;   // wake up, use the gyro clock (steadier than the internal one)
  delay(50);
  mpuWrite(0x1A, 3);                         // low-pass filter ~44 Hz
  mpuWrite(0x1C, 0);                         // +-2 g
  return true;
}
// Reads the accelerometer and returns the direction of gravity as a UNIT vector in g[].
//   return  0 : good reading, board fairly still
//   return  1 : readable, but you are moving/jolted (|g| not close to 1) -> do not trust it
//   return -1 : I2C error
int mpuGravity(float g[3]) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6) != 6) return -1;
  uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
  uint8_t yh = Wire.read(); uint8_t yl = Wire.read();
  uint8_t zh = Wire.read(); uint8_t zl = Wire.read();
  g[0] = (int16_t)((xh << 8) | xl) / 16384.0f;
  g[1] = (int16_t)((yh << 8) | yl) / 16384.0f;
  g[2] = (int16_t)((zh << 8) | zl) / 16384.0f;
  float m = sqrtf(dot3(g, g));
  if (m < 0.5f) return -1;                   // all zeros = sensor asleep or bus fault
  g[0] /= m; g[1] /= m; g[2] /= m;
  return (m > 0.85f && m < 1.15f) ? 0 : 1;
}

// ---------------- flex sensor ----------------
float readFlexRaw() {
#if USE_FLEX
  // MEDIAN of 15 readings: a single wild sample (loose wire, radio burst) cannot move it.
  int v[15];
  for (int i = 0; i < 15; i++) { v[i] = analogRead(PIN_FLEX); delayMicroseconds(100); }
  for (int i = 1; i < 15; i++) {             
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return (float)v[7];
#else
  return 0;                                 
#endif
}

// ---------------- events / cloud queue ----------------
void sendEvent(char type, const char* text) {
  Event e; e.type = type;
  strncpy(e.text, text, sizeof(e.text) - 1); e.text[sizeof(e.text) - 1] = 0;
  xQueueSend(evQ, &e, 0);
}

// ---------------- forward lean angle ----------------
// Rotation of "gravity" away from the upright direction, measured around the sideways axis.
// Positive = leaning forward (the direction you showed during calibration), negative = leaning back.
float forwardTiltDeg(const float g[3]) {
  float c[3];
  cross3(g0, g, c);
  float s = dot3(c, axisL);      // sin(angle) around the sideways axis
  float k = dot3(g0, g);         // cos(angle)
  return atan2f(s, k) * 57.29578f;
}

// ---------------- calibration ----------------
// Averages ~2 s of gravity direction and flex. Returns false if the sensor failed or you moved.
bool captureUpright(float outG[3], float &outFlex) {
#if USE_MPU
  static float buf[100][3];
  int n = 0; float fsum = 0;
  for (int i = 0; i < 100; i++) {
    float g[3];
    if (mpuGravity(g) == 0 && n < 100) { memcpy(buf[n], g, sizeof(g)); n++; }
    fsum += readFlexRaw();
    delay(20);
  }
  if (n < 60) return false;
  float m[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) { m[0] += buf[i][0]; m[1] += buf[i][1]; m[2] += buf[i][2]; }
  if (!norm3(m)) return false;
  float worst = 0;
  for (int i = 0; i < n; i++) { float a = angleDeg(buf[i], m); if (a > worst) worst = a; }
  if (worst > 8.0f) return false;            // you moved while it was measuring
  memcpy(outG, m, sizeof(m));
  outFlex = fsum / 100.0f;
  return true;
#else
  // MPU6050 switched off: measure only the flex sensor for ~2 s
  float fsum = 0, fmin = 1e9f, fmax = -1e9f;
  for (int i = 0; i < 100; i++) {
    float f = readFlexRaw();
    fsum += f;
    if (f < fmin) fmin = f;
    if (f > fmax) fmax = f;
    delay(20);
  }
  outG[0] = 0; outG[1] = 0; outG[2] = 1;
  outFlex = fsum / 100.0f;
  Serial.printf("  Flex readings while holding still: min=%.0f max=%.0f (spread %.0f counts)\n", fmin, fmax, fmax - fmin);
  return true;
#endif
}

// You lean forward for 5 s. The direction gravity moves tells us which sensor axis the neck bends around.
bool learnForward() {
  float gl[3]   = {g0[0], g0[1], g0[2]};
  float gMax[3] = {g0[0], g0[1], g0[2]};
  float maxAng = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    float g[3];
    if (mpuGravity(g) == 0) {
      for (int i = 0; i < 3; i++) gl[i] += 0.2f * (g[i] - gl[i]);
      float tmp[3] = {gl[0], gl[1], gl[2]};
      if (norm3(tmp)) {
        float a = angleDeg(g0, tmp);
        if (a > maxAng) { maxAng = a; memcpy(gMax, tmp, sizeof(tmp)); }
      }
    }
    delay(20);
  }
  Serial.printf("  Lean detected: %.0f deg (needs at least %.0f)\n", maxAng, MIN_LEAN_DEG);
  if (maxAng < MIN_LEAN_DEG) return false;
  float L[3];
  cross3(g0, gMax, L);                       // the rotation axis
  if (!norm3(L)) return false;
  memcpy(axisL, L, sizeof(L));               // by construction, this lean now measures POSITIVE
  axisKnown = true;
  return true;
}

void calibrate(bool full) {
  g_session = false;
  Serial.println("\n=== CALIBRATION ===");
  Serial.println("Step 1: sit UPRIGHT in your good posture and hold still (2 s)...");
  cue(1, 300, 600);                          // one long beep = hold still

  float gU[3], fU = 0; bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    ok = captureUpright(gU, fU);
    if (!ok) { Serial.println("  Moved too much or no sensor data - hold still, trying again"); cue(3, 60, 60); delay(500); }
  }
  if (!ok) { Serial.println("Calibration FAILED. Check the sensor wiring, then press the button again."); cue(5, 80, 80); return; }
  memcpy(g0, gU, sizeof(gU));
  flexBase = fU < 50 ? 50 : fU;              // avoid divide-by-zero if wiring is wrong

#if USE_MPU
  if (full || !axisKnown) {
    cue(2, 150, 150);                        // two beeps = "now lean forward"
    Serial.println("Step 2: now LEAN FORWARD as if reading small text on the screen (5 s), then sit back up");
    bool learned = learnForward();
    if (!learned && !axisKnown) {            // last resort: guess the axis with the least gravity
      int k = 0;
      for (int i = 1; i < 3; i++) if (fabsf(g0[i]) < fabsf(g0[k])) k = i;
      axisL[0] = axisL[1] = axisL[2] = 0;
      axisL[k] = FORWARD_SIGN_FALLBACK;
      Serial.println("  Lean too small - the forward direction is a GUESS. Hold the button 4 s to repeat.");
    } else if (!learned) {
      Serial.println("  Lean too small - kept the previously learned direction.");
    }
    cue(1, 300, 0);                          // one beep = sit back up
  }

  const char* an[3] = {"X", "Y", "Z"};
  int up = 0, side = 0;
  for (int i = 1; i < 3; i++) { if (fabsf(g0[i]) > fabsf(g0[up])) up = i; if (fabsf(axisL[i]) > fabsf(axisL[side])) side = i; }
  Serial.printf("Mounting detected: gravity mostly on the %s axis, neck bends around the %s axis\n", an[up], an[side]);
#endif
#if USE_MPU
  Serial.printf("Calibrated MPU6050: g0=[%.2f %.2f %.2f]\n", g0[0], g0[1], g0[2]);
#endif
#if USE_FLEX
  Serial.printf("Calibrated flex sensor: flexBase=%.0f counts (this is your good-posture value)\n", flexBase);
#endif

  filtInit = false; neckF = 0; spineF = 0; neckBad = spineBad = false;
  badHold = goodHold = 0; lastPostureAlertMs = 0; lastSessAlertSec = 0; episodeSent = false;
  g_sessionSec = 0; g_session = true;
  settleUntilMs = millis() + 4000;           // ignore bad-posture checks while you sit back and filters settle
}

// ---------------- sensing (20 times per second) ----------------
void sampleSensors() {
#if USE_MPU
  float g[3];
  int r = mpuGravity(g);
  if (r < 0) {
    if (++mpuFails >= 20) {                  // about 1 s of failures in a row
      mpuFault = true;
      Serial.println("MPU6050 not answering - restarting I2C");
      mpuInit();
      mpuFails = 0;
    }
  } else {
    mpuFails = 0; mpuFault = false;
    if (r == 0) {                            // only trust readings taken while you are fairly still
      if (!filtInit) { memcpy(gF, g, sizeof(gF)); filtInit = true; }
      for (int i = 0; i < 3; i++) gF[i] += 0.1f * (g[i] - gF[i]);
    }
  }
  if (filtInit) {
    float n[3] = {gF[0], gF[1], gF[2]};
    if (norm3(n)) neckF = forwardTiltDeg(n);
  }
#else
  neckF = 0;                                 // MPU6050 switched off (USE_MPU = 0)
#endif
#if USE_FLEX
  flexNow = readFlexRaw();
  float pct = fabsf(flexNow - flexBase) / flexBase * 100.0f;
  spineF = 0.8f * spineF + 0.2f * pct;
#else
  spineF = 0;                                // flex sensor switched off (USE_FLEX = 0)
#endif
  g_neck = neckF; g_spine = spineF;
}

// ---------------- decisions (once per second) ----------------
void secondTick() {
  digitalWrite(PIN_LED, g_session ? !digitalRead(PIN_LED) : LOW);
  if (!g_session) { g_bad = false; return; }
  g_sessionSec = g_sessionSec + 1;

  // hysteresis: becomes bad above the limit, becomes good again only below 80 % of the limit
  if (!neckBad && neckF > cfg.neckThr) neckBad = true;
  else if (neckBad && neckF < cfg.neckThr * 0.8f) neckBad = false;
  if (!spineBad && spineF > cfg.spineThr) spineBad = true;
  else if (spineBad && spineF < cfg.spineThr * 0.8f) spineBad = false;

  bool bad = (neckBad && !mpuFault) || spineBad;
  if (millis() < settleUntilMs) bad = false;   // just calibrated - trust it was good, ignore noise
  g_bad = bad;
  if (bad) {
    badHold++;
    goodHold = 0;
    g_badSec = g_badSec + 1;
  } else {
    goodHold++;
    badHold = 0;
    g_goodSec = g_goodSec + 1;
    if (goodHold >= 10) episodeSent = false;
  }

  uint32_t nowMs = millis();
  if (bad && badHold >= (uint32_t)cfg.holdSec &&
      (lastPostureAlertMs == 0 || nowMs - lastPostureAlertMs >= ALERT_REPEAT_MS)) {
    lastPostureAlertMs = nowMs;
    startPattern(3, 150, 120);                       // three quick beeps + buzzes
    if (!episodeSent) { episodeSent = true; sendEvent('P', "Bad posture - please sit upright"); }
  }

  uint32_t limitSec = (uint32_t)cfg.sessionLimitMin * 60UL;
  if (limitSec > 0 && g_sessionSec >= limitSec &&
      (lastSessAlertSec == 0 || g_sessionSec - lastSessAlertSec >= 300)) {
    lastSessAlertSec = g_sessionSec;
    startPattern(2, 700, 300);                       // two long beeps
    sendEvent('S', "Session limit reached - take a break and remove the device");
  }
  Serial.printf("sess=%lus bad=%d", (unsigned long)g_sessionSec, (int)g_bad);
#if USE_MPU
  Serial.printf("   neck=%.1f deg  g=[%.2f %.2f %.2f]%s", (float)g_neck, gF[0], gF[1], gF[2], mpuFault ? "  MPU FAULT" : "");
#endif
#if USE_FLEX
  Serial.printf("   flex=%.0f (base %.0f)  spine=%.1f%% (limit %.0f%%)", flexNow, flexBase, (float)g_spine, cfg.spineThr);
#endif
  Serial.println("");
}

void handleButton() {
  static bool down = false; static uint32_t t0 = 0;
  bool pressed = digitalRead(PIN_BTN) == LOW;
  uint32_t now = millis();
  if (pressed && !down) { down = true; t0 = now; }
  if (!pressed && down) {
    down = false;
    uint32_t dur = now - t0;
    if (dur < 50) return;
    if (dur < 1500)      calibrate(!axisKnown);      // short: re-zero (also learns the lean the first time)
    else if (dur < 4000) { g_session = false; startPattern(2, 100, 100); Serial.println("Session stopped"); }
    else                 calibrate(true);            // very long: full calibration again
  }
}

// ---------------- network / Firebase (runs on core 0) ----------------
// Firebase Realtime Database layout used by THIS wearable node:
//   /wearable/latest        PUT every 2 s
//   /wearable/history       POST one good/bad summary every 60 s
//   /wearable/events        POST when a posture/session alert starts
//   /settings/wearable      GET every 10 s; created with defaults if missing
//
// The monitor ESP32 writes separately to /monitor/... in the SAME database.

int fbSend(const char* method, const char* path, const char* body, String* resp = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  WiFiClientSecure client;
  client.setInsecure();  
  HTTPClient http;
  http.setTimeout(6000);
  String url = String(DB_URL) + path + ".json";
  if (!http.begin(client, url)) return -1;

  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, (uint8_t*)body, strlen(body));
  }

  if (resp && code > 0) *resp = http.getString();
  http.end();

  if (code != 200) Serial.printf("Firebase %s %s -> %d\n", method, path, code);
  return code;
}

bool createDefaultSettingsIfMissing() {
  String resp;
  int code = fbSend("GET", "/settings/wearable", "", &resp);
  if (code != 200) return false;

  resp.trim();
  if (resp != "null" && resp.length() > 0) return true;

  char body[180];
  snprintf(body, sizeof(body),
    "{\"neckThr\":%.1f,\"spineThr\":%.1f,\"holdSec\":%d,\"sessionLimitMin\":%d,"
    "\"useBuzzer\":%s,\"useVibe\":%s}",
    cfg.neckThr, cfg.spineThr, cfg.holdSec, cfg.sessionLimitMin,
    cfg.useBuzzer ? "true" : "false", cfg.useVibe ? "true" : "false");

  code = fbSend("PUT", "/settings/wearable", body);
  if (code == 200) Serial.println("Created /settings/wearable with default values.");
  return code == 200;
}

// ---------------- Telegram (ADDED) ----------------
// Telegram is called from the existing network task, not from the sensor loop.
// Therefore sensor sampling/calibration/posture decisions remain unchanged.
String urlEncode(const String& value) {
  String out;
  const char* hex = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); i++) {
    unsigned char c = (unsigned char)value[i];

    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }

  return out;
}

bool sendTelegramMessage(const String& message) {
  if (!ENABLE_TELEGRAM) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  // Keep placeholders from accidentally making useless network requests.
  if (String(TG_BOT_TOKEN).startsWith("YOUR_") ||
      String(TG_CHAT_ID).startsWith("YOUR_")) {
    Serial.println("Telegram not configured: set TG_BOT_TOKEN and TG_CHAT_ID.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // same prototype TLS approach already used for Firebase

  HTTPClient http;
  http.setTimeout(6000);

  String url = String("https://api.telegram.org/bot") +
               TG_BOT_TOKEN +
               "/sendMessage";

  if (!http.begin(client, url)) {
    Serial.println("Telegram: HTTP begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postBody =
    String("chat_id=") + urlEncode(String(TG_CHAT_ID)) +
    "&text=" + urlEncode(message);

  int code = http.POST(postBody);
  String response = http.getString();
  http.end();

  Serial.printf("Telegram -> %d\n", code);

  if (code != 200) {
    Serial.println(response);
    return false;
  }

  return true;
}

void sendTelegramForEvent(const Event& e) {
  String msg = "PostureSense";

  if (e.type == 'P') {
    msg += "\nPOSTURE ALERT";

    msg += "\nNeck: ";
    if (mpuFault) {
      msg += "SENSOR FAULT";
    } else {
      msg += (neckBad ? "BAD" : "GOOD");
      msg += " (";
      msg += String((float)g_neck, 1);
      msg += " deg)";
    }

    msg += "\nSpine: ";
    msg += (spineBad ? "BAD" : "GOOD");
    msg += " (";
    msg += String((float)g_spine, 1);
    msg += "%)";

    if (neckBad && spineBad) {
      msg += "\nProblem: neck + spine posture";
    } else if (neckBad) {
      msg += "\nProblem: neck posture";
    } else if (spineBad) {
      msg += "\nProblem: spine posture";
    }

    msg += "\nPlease correct your posture.";
  }
  else if (e.type == 'S') {
    msg += "\nSESSION LIMIT";
    msg += "\nSession time: ";
    msg += String((unsigned long)(g_sessionSec / 60));
    msg += " min";
    msg += "\nTake a break and remove the device.";
  }
  else {
    msg += "\n";
    msg += e.text;
  }

  sendTelegramMessage(msg);
}

void handleEvent(const Event& e) {
  const char* type = (e.type == 'P') ? "POSTURE" : "SESSION_LIMIT";
  char body[240];
  snprintf(body, sizeof(body),
    "{\"type\":\"%s\",\"neck\":%.1f,\"spine\":%.1f,\"sessionSec\":%lu,"
    "\"ts\":{\".sv\":\"timestamp\"}}",
    type, (float)g_neck, (float)g_spine, (unsigned long)g_sessionSec);

  // Existing Firebase event write — unchanged.
  fbSend("POST", "/wearable/events", body);

  // Added notification only.
  sendTelegramForEvent(e);
}

void pushLatest() {
  char body[320];
  snprintf(body, sizeof(body),
    "{\"neck\":%.1f,\"spine\":%.1f,\"bad\":%s,\"neckBad\":%s,\"spineBad\":%s,"
    "\"session\":%s,\"sessionSec\":%lu,\"mpuFault\":%s,\"rssi\":%d,\"uptime\":%lu,"
    "\"ts\":{\".sv\":\"timestamp\"}}",
    (float)g_neck, (float)g_spine,
    g_bad ? "true" : "false",
    neckBad ? "true" : "false",
    spineBad ? "true" : "false",
    g_session ? "true" : "false",
    (unsigned long)g_sessionSec,
    mpuFault ? "true" : "false",
    WiFi.RSSI(), (unsigned long)(millis() / 1000));
  fbSend("PUT", "/wearable/latest", body);
}

// Store ONE summary for the previous ~60 seconds.
// g = number of good-posture seconds, b = number of bad-posture seconds.
// n/s are the latest neck/spine values at the end of that minute, useful for charts.
void pushMinuteSummary() {
  uint16_t good = g_goodSec;
  uint16_t bad  = g_badSec;

  // No active-session seconds were counted during this minute.
  if (good + bad == 0) return;

  // Reset for the next minute only after copying the values.
  g_goodSec = 0;
  g_badSec = 0;

  char body[260];
  snprintf(body, sizeof(body),
    "{\"g\":%u,\"b\":%u,\"n\":%.1f,\"s\":%.1f,\"on\":%s,"
    "\"ts\":{\".sv\":\"timestamp\"}}",
    good, bad, (float)g_neck, (float)g_spine,
    g_session ? "true" : "false");

  int code = fbSend("POST", "/wearable/history", body);
  if (code == 200) {
    Serial.printf("Wearable minute summary uploaded: good=%u s, bad=%u s\n", good, bad);
  } else {
    // Restore the counts if the upload failed so the data is not silently discarded.
    g_goodSec = g_goodSec + good;
    g_badSec  = g_badSec + bad;
  }
}

void pullSettings() {
  String resp;
  if (fbSend("GET", "/settings/wearable", "", &resp) != 200) return;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  if (!doc.is<JsonObject>()) return;

  cfg.neckThr         = doc["neckThr"]         | cfg.neckThr;
  cfg.spineThr        = doc["spineThr"]        | cfg.spineThr;
  cfg.holdSec         = doc["holdSec"]         | cfg.holdSec;
  cfg.sessionLimitMin = doc["sessionLimitMin"] | cfg.sessionLimitMin;
  cfg.useBuzzer       = doc["useBuzzer"]       | cfg.useBuzzer;
  cfg.useVibe         = doc["useVibe"]         | cfg.useVibe;

  cfg.neckThr         = constrain(cfg.neckThr, 5.0f, 60.0f);
  cfg.spineThr        = constrain(cfg.spineThr, 3.0f, 80.0f);
  cfg.holdSec         = constrain(cfg.holdSec, 1, 120);
  cfg.sessionLimitMin = constrain(cfg.sessionLimitMin, 0, 600);
}

void netTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t lastLatest = 0, lastMinuteSummary = 0, lastCfg = 0, lastWifiTry = 0;
  bool settingsChecked = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      settingsChecked = false;
      if (millis() - lastWifiTry > 10000) {
        lastWifiTry = millis();
        WiFi.reconnect();
      }
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    if (!settingsChecked) {
      settingsChecked = createDefaultSettingsIfMissing();
      if (settingsChecked) pullSettings();
    }

    Event ev;
    while (xQueueReceive(evQ, &ev, 0) == pdTRUE) handleEvent(ev);

    uint32_t now = millis();
    if (now - lastLatest >= 2000) {
      lastLatest = now;
      pushLatest();
    }
    if (now - lastMinuteSummary >= 60000) {
      lastMinuteSummary = now;
      pushMinuteSummary();
    }
    if (now - lastCfg >= 10000) {
      lastCfg = now;
      pullSettings();
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZ, OUTPUT); pinMode(PIN_VIBE, OUTPUT); pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  digitalWrite(PIN_BUZZ, LOW); digitalWrite(PIN_VIBE, LOW);
#if USE_FLEX
  analogSetPinAttenuation(PIN_FLEX, ADC_11db);
#endif

#if USE_MPU
  if (!mpuInit()) {
    Serial.println("MPU6050 NOT FOUND - check wiring");
    for (int i = 0; i < 5; i++) { digitalWrite(PIN_BUZZ, HIGH); delay(100); digitalWrite(PIN_BUZZ, LOW); delay(100); }
  }
#endif
  evQ = xQueueCreate(6, sizeof(Event));
  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);

  Serial.println("Put the strap on and sit upright. PRESS THE BUTTON to start the guided calibration (waiting 30 s).");
  uint32_t t0 = millis();
  while (digitalRead(PIN_BTN) == HIGH && millis() - t0 < 30000) {
    digitalWrite(PIN_LED, (millis() / 250) % 2);      // fast blink = waiting 
    delay(10);
  }
  digitalWrite(PIN_LED, LOW);
  if (digitalRead(PIN_BTN) == LOW) {
    delay(60);
    while (digitalRead(PIN_BTN) == LOW) delay(10);    // wait for release
    calibrate(true);
  } else {
    Serial.println("No button press - idle. Press the button whenever you are ready.");
  }
}

void loop() {
  static uint32_t lastSample = 0, lastTick = 0;
  uint32_t now = millis();
  handleButton();
  updatePattern();
  if (now - lastSample >= 50)   { lastSample = now; sampleSensors(); }
  if (now - lastTick   >= 1000) { lastTick   = now; secondTick(); }
}
