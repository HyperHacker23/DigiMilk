/**
 * Project: ESP32 IoT Milk Dispenser (Final)
 * Hardware: ESP32, L298N Driver (Channel B), Pump
 * Wiring:
 * - ENB -> GPIO 27 (Speed/Enable)
 * - IN3 -> GPIO 26
 * - IN4 -> GPIO 25
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <L298N.h>

// ===== CONFIG =====
const char *AP_SSID = "MilkPoC";
const char *AP_PASS = "12345678";

// CHANGE THESE TO YOUR HOME WIFI
const char *HOME_SSID = "work";
const char *HOME_PASS = "abcdefgh";

const char *LOG_PATH = "/logs.csv";
const float PRICE_PER_LITER = 50.0f;
const char *DISPENSE_API_TOKEN = "DMTKN_4nFh92xQ7sY8wLf0BqZp3cR1vKdTg";

// ===== HARDWARE (Channel B) =====
#define ENB_PIN 27
#define IN3_PIN 26
#define IN4_PIN 25

// Constructor: (EnablePin, Input1, Input2)
L298N motor(ENB_PIN, IN3_PIN, IN4_PIN);

// ===== STATE =====
WebServer server(80);
Preferences prefs;
const char *PREF_NAMESPACE = "milk";

// Calibration default (approx 6.5s for 100mL)
double saved_ms_per_100ml = 6503.80;

// Storage for calibration samples
String samplesCSV = "";
#define MAX_SAMPLES 20
unsigned long samples[MAX_SAMPLES];
int sampleCount = 0;

// Flags
bool calibrating = false;
unsigned long calStartMillis = 0;
bool dispensing = false;
double target_ml = 0.0;
String current_mode = "ml";
unsigned long dispenseStartMillis = 0;
unsigned long dispenseEndMillis = 0;

// Safety Limits
const unsigned long MAX_DISPENSE_MS = 300000; // 5 mins
const unsigned long CAL_MAX_MS = 600000;      // 10 mins

// SSE Client
WiFiClient sseClient;
bool sseClientActive = false;

// ================= HELPERS =================
String getISTTimestamp()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &timeinfo);
  return String(buf);
}

void ensureLogHeader()
{
  if (!SPIFFS.exists(LOG_PATH))
  {
    File f = SPIFFS.open(LOG_PATH, FILE_WRITE);
    if (f)
    {
      f.println("Status,Mode,Amount_mL,Price,Timestamp");
      f.close();
    }
  }
}

void appendLog(const String &status, const String &mode, double amount_ml, double price)
{
  ensureLogHeader();
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (f)
  {
    f.println(status + "," + mode + "," + String(amount_ml, 2) + "," + String(price, 2) + "," + getISTTimestamp());
    f.close();
  }
}

// ===== PREFERENCES =====
void loadCalibrationFromPrefs()
{
  prefs.begin(PREF_NAMESPACE, false);
  saved_ms_per_100ml = prefs.getDouble("cal100", saved_ms_per_100ml);
  samplesCSV = prefs.getString("samples", "");
  prefs.end();

  // Parse CSV
  sampleCount = 0;
  if (samplesCSV.length())
  {
    int idx = 0;
    String cur = "";
    while (idx < (int)samplesCSV.length())
    {
      char c = samplesCSV[idx++];
      if (c == ',')
      {
        if (cur.length() && sampleCount < MAX_SAMPLES)
        {
          samples[sampleCount++] = (unsigned long)cur.toInt();
          cur = "";
        }
      }
      else
        cur += c;
    }
    if (cur.length() && sampleCount < MAX_SAMPLES)
      samples[sampleCount++] = (unsigned long)cur.toInt();
  }
}

void saveSamplesCSVToPrefs()
{
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString("samples", samplesCSV);
  prefs.end();
}

void appendSample(unsigned long ms)
{
  if (sampleCount < MAX_SAMPLES)
  {
    samples[sampleCount++] = ms;
    if (samplesCSV.length())
      samplesCSV += ",";
    samplesCSV += String(ms);
  }
  else
  {
    // FIFO Rotate
    for (int i = 1; i < MAX_SAMPLES; ++i)
      samples[i - 1] = samples[i];
    samples[MAX_SAMPLES - 1] = ms;
    // Rebuild CSV
    samplesCSV = "";
    for (int i = 0; i < MAX_SAMPLES; ++i)
    {
      if (i)
        samplesCSV += ",";
      samplesCSV += String(samples[i]);
    }
  }
  saveSamplesCSVToPrefs();
}

double computeSamplesAverage()
{
  if (sampleCount == 0)
    return 0.0;
  unsigned long sum = 0;
  for (int i = 0; i < sampleCount; ++i)
    sum += samples[i];
  return ((double)sum) / (double)sampleCount;
}

// ===== JSON & SSE =====
String makeStateJSON()
{
  StaticJsonDocument<1024> doc;
  doc["dispensing"] = dispensing;
  doc["calibrating"] = calibrating;
  doc["saved_ms_per_100ml"] = saved_ms_per_100ml;
  doc["target_ml"] = target_ml;
  doc["mode"] = current_mode;
  doc["now_ms"] = millis();

  // [BUG FIX] Division by Zero Check
  double safe_cal = (saved_ms_per_100ml > 0.1) ? saved_ms_per_100ml : 6000.0;

  if (dispensing)
  {
    unsigned long elapsed = millis() - dispenseStartMillis;
    double current_ml = ((double)elapsed / safe_cal) * 100.0;
    doc["dispensed_ml"] = current_ml;
  }
  else
  {
    doc["dispensed_ml"] = 0.0;
  }

  JsonArray arr = doc.createNestedArray("samples");
  for (int i = 0; i < sampleCount; ++i)
    arr.add(samples[i]);

  String out;
  serializeJson(doc, out);
  return out;
}

void sseSend(const String &data)
{
  if (sseClientActive && sseClient.connected())
  {
    sseClient.print("data: ");
    sseClient.print(data);
    sseClient.print("\n\n");
  }
  else
  {
    sseClientActive = false;
  }
}

// ===== HANDLERS =====
void handleDispenseStart()
{
  // 1. Security Check
  if (!server.hasHeader("X-DISPENSE-TOKEN") || server.header("X-DISPENSE-TOKEN") != DISPENSE_API_TOKEN)
  {
    server.send(403, "text/plain", "Invalid token");
    return;
  }

  // [BUG FIX] Busy Check (Prevents double dispense)
  if (dispensing || calibrating)
  {
    server.send(409, "text/plain", "Busy: Already dispensing");
    return;
  }

  float val = server.arg("value").toFloat();
  String mode = server.arg("mode");

  if (val <= 0)
  {
    server.send(400, "text/plain", "Invalid value");
    return;
  }

  // 2. Logic
  if (mode == "ml")
    target_ml = val;
  else if (mode == "litre")
    target_ml = val * 1000.0;
  else if (mode == "cost")
    target_ml = (val / PRICE_PER_LITER) * 1000.0;
  else
  {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }

  unsigned long runMs = (unsigned long)round((target_ml / 100.0) * saved_ms_per_100ml);
  if (runMs == 0)
    runMs = 1;

  if (runMs > MAX_DISPENSE_MS)
  {
    server.send(400, "text/plain", "Requested dispense too long");
    return;
  }

  // 3. Action
  dispensing = true;
  dispenseStartMillis = millis();
  dispenseEndMillis = dispenseStartMillis + runMs;
  current_mode = mode;
  motor.forward();

  server.send(200, "text/plain", "Dispense started");
  appendLog("DISPENSE_START", current_mode, target_ml, (target_ml / 1000.0) * PRICE_PER_LITER);
  sseSend(makeStateJSON());
}

void handleDispenseStop()
{
  if (!dispensing)
  {
    server.send(200, "text/plain", "Not dispensing");
    return;
  }
  motor.stop();
  dispensing = false;
  unsigned long actualRunMs = millis() - dispenseStartMillis;
  double dispensed_ml = ((double)actualRunMs / saved_ms_per_100ml) * 100.0;
  double price = (dispensed_ml / 1000.0) * PRICE_PER_LITER;

  appendLog("MANUAL_STOP", current_mode, dispensed_ml, price);
  server.send(200, "text/plain", "Dispense stopped");
  sseSend(makeStateJSON());
}

// Calibration Handlers
void handleCalStart()
{
  if (calibrating || dispensing)
  {
    server.send(400, "text/plain", "Busy");
    return;
  }
  calibrating = true;
  calStartMillis = millis();
  motor.forward();
  server.send(200, "text/plain", "Cal start");
  sseSend(makeStateJSON());
}

void handleCalStop()
{
  if (!calibrating)
    return;
  motor.stop();
  calibrating = false;
  unsigned long delta = millis() - calStartMillis;
  if (delta > CAL_MAX_MS)
  {
    server.send(400, "text/plain", "Timeout");
    return;
  }

  appendSample(delta);
  StaticJsonDocument<256> doc;
  doc["last_sample_ms"] = delta;
  doc["average_ms"] = computeSamplesAverage();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
  sseSend(makeStateJSON());
}

void handleCalSave()
{
  if (sampleCount == 0)
  {
    server.send(400, "text/plain", "No samples");
    return;
  }
  double avg = computeSamplesAverage();
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putDouble("cal100", avg);
  prefs.end();
  saved_ms_per_100ml = avg;
  server.send(200, "application/json", "{\"status\":\"saved\"}");
  sseSend(makeStateJSON());
}

void handleCalReset()
{
  sampleCount = 0;
  samplesCSV = "";
  saveSamplesCSVToPrefs();
  server.send(200, "text/plain", "Reset OK");
  sseSend(makeStateJSON());
}

void handleRoot()
{
  if (SPIFFS.exists("/index.html"))
  {
    File f = SPIFFS.open("/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  }
  else
    server.send(200, "text/plain", "Missing index.html");
}

void handleEvents()
{
  WiFiClient client = server.client();
  if (client)
  {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println();
    sseClient = client;
    sseClientActive = true;
    sseSend(makeStateJSON());
  }
}

// ===== SETUP =====
void setup()
{
  Serial.begin(115200);

  // MOTOR: Software Enable for Channel B
  motor.setSpeed(255);
  motor.stop();

  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS Fail");
  ensureLogHeader();
  loadCalibrationFromPrefs();

  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);

  long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
  {
    delay(100);
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "Wifi Connected" : "Wifi Failed");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP()); // <--- READ THIS FOR .ENV FILE

  configTime(19800, 0, "pool.ntp.org");

  // Routes
  server.on("/", handleRoot);
  server.on("/events", handleEvents);
  server.on("/start", HTTP_POST, handleDispenseStart);
  server.on("/stop", HTTP_POST, handleDispenseStop);
  server.on("/calibrate/start", HTTP_POST, handleCalStart);
  server.on("/calibrate/stop", HTTP_POST, handleCalStop);
  server.on("/calibrate/save", HTTP_POST, handleCalSave);
  server.on("/calibrate/reset", HTTP_POST, handleCalReset);

  const char *headerKeys[] = {"X-DISPENSE-TOKEN"};
  server.collectHeaders(headerKeys, 1);
  server.begin();
}

// ===== LOOP =====
void loop()
{
  server.handleClient();

  // 1. Auto Reconnect
  if (WiFi.status() != WL_CONNECTED)
  {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 10000)
    {
      lastCheck = millis();
      WiFi.reconnect();
    }
  }

  // 2. Calibration Logic
  if (calibrating && millis() - calStartMillis > CAL_MAX_MS)
  {
    motor.stop();
    calibrating = false;
    sseSend(makeStateJSON());
  }

  // 3. Dispense Logic
  if (dispensing)
  {
    unsigned long now = millis();
    if (now >= dispenseEndMillis)
    {
      motor.stop();
      dispensing = false;
      sseSend(makeStateJSON());
    }
    else if (now - dispenseStartMillis > MAX_DISPENSE_MS)
    {
      motor.stop();
      dispensing = false;
      sseSend(makeStateJSON());
    }
    else
    {
      static unsigned long lastPush = 0;
      if (now - lastPush > 250)
      {
        sseSend(makeStateJSON());
        lastPush = now;
      }
    }
  }
}