#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ===== CONFIG =====
const char *AP_SSID = "MilkPoC";
const char *AP_PASS = "12345678";

const char *HOME_SSID = "HomeWifi";
const char *HOME_PASS = "f1nallyw1f1c0nnect10n1nmyh0me";

const char *LOG_PATH = "/logs.csv";
const float PRICE_PER_LITER = 50.0f;

// THIS MUST MATCH BACKEND
const char *DISPENSE_API_TOKEN = "DMTKN_4nFh92xQ7sY8wLf0BqZp3cR1vKdTg";

// ===== HARDWARE PINS =====
#define FLOW_PIN 27
#define MOTOR_IN1 14
#define MOTOR_IN2 12

// ===== GLOBAL STATE =====
volatile unsigned long pulseCount = 0;

float mlPerPulse = 0.03580f;    // fixed, no calibration
float calibrationFactor = 0.0f; // derived from mlPerPulse

bool dispensing = false;
float dispensed_ml = 0.0f;
float target_ml = 0.0f;
String current_mode = "ml";

// Safety/timeouts
unsigned long dispenseStartMillis = 0;
unsigned long lastPulseMillis = 0;
const unsigned long MAX_DISPENSE_MS = 180000UL;
const unsigned long NO_PULSE_TIMEOUT_MS = 5000UL;

// SSE
WebServer server(80);
WiFiClient sseClient;
bool sseClientActive = false;
unsigned long lastSSEPing = 0;

// ===== ISR =====
void IRAM_ATTR flowISR()
{
  pulseCount++;
  lastPulseMillis = millis();
}

// ===== LOGGING =====
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
    f.println("Status,Mode,Amount_mL,Price,Timestamp");
    f.close();
  }
}

void appendLog(const String &status, const String &mode, float amount_ml, float price)
{
  ensureLogHeader();
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  f.println(status + "," + mode + "," + String(amount_ml, 2) + "," + String(price, 2) + "," + getISTTimestamp());
  f.close();
}

// ===== FLOW FORMULA =====
void computeCalibrationFactor()
{
  float pulsesPerLiter = 1000.0f / mlPerPulse;
  calibrationFactor = pulsesPerLiter / 60.0f; // pulses per L/min
}

// ===== SSE JSON =====
String makeStateJSON()
{
  JsonDocument doc;
  doc["dispensing"] = dispensing;
  doc["dispensed_ml"] = dispensed_ml;
  doc["target_ml"] = target_ml;
  doc["mode"] = current_mode;

  String out;
  serializeJson(doc, out);
  return out;
}

void sseSend(const String &data)
{
  if (!sseClientActive || !sseClient.connected())
    return;
  sseClient.print("data: ");
  sseClient.print(data);
  sseClient.print("\n\n");
}

// ===== MOTOR =====
void pumpStart()
{
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  Serial.println("PUMP START");
}

void pumpStop()
{
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  Serial.println("PUMP STOP");
}

// ===== HTTP HANDLERS =====
void handleRoot()
{
  File f = SPIFFS.open("/index.html", FILE_READ);
  server.streamFile(f, "text/html");
  f.close();
}

void handleStatus()
{
  server.send(200, "application/json", makeStateJSON());
}

void handleLogsCSV()
{
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClearLogs()
{
  SPIFFS.remove(LOG_PATH);
  ensureLogHeader();
  server.send(200, "text/plain", "Logs cleared");
}

void handleEvents()
{
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();
  client.println(": connected");

  sseClient = client;
  sseClientActive = true;
  sseSend(makeStateJSON());
}

void handleStart()
{
  // Validate security token (now headers are collected)
  if (!server.hasHeader("X-DISPENSE-TOKEN") ||
      server.header("X-DISPENSE-TOKEN") != DISPENSE_API_TOKEN)
  {
    server.send(403, "text/plain", "Invalid token");
    return;
  }

  if (!server.hasArg("value") || !server.hasArg("mode"))
  {
    server.send(400, "text/plain", "Missing args");
    return;
  }

  float val = server.arg("value").toFloat();
  String m = server.arg("mode");

  if (m == "ml")
    target_ml = val;
  else if (m == "litre")
    target_ml = val * 1000;
  else if (m == "cost")
    target_ml = (val / PRICE_PER_LITER) * 1000;
  else
  {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }

  pulseCount = 0;
  dispensed_ml = 0;
  current_mode = m;
  dispensing = true;
  dispenseStartMillis = millis();
  lastPulseMillis = millis();

  pumpStart();
  server.send(200, "text/plain", "Started");
  sseSend(makeStateJSON());
}

void handleStop()
{
  dispensing = false;
  pumpStop();

  float price = (dispensed_ml / 1000.0f) * PRICE_PER_LITER;
  appendLog("SUCCESS", current_mode, dispensed_ml, price);

  server.send(200, "text/plain", "Stopped");
  sseSend(makeStateJSON());
}

// ===== SETUP =====
void setup()
{
  Serial.begin(115200);
  SPIFFS.begin(true);
  ensureLogHeader();
  computeCalibrationFactor();

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, RISING);

  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pumpStop();

  // WiFi
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 7000)
  {
    Serial.print(".");
    delay(300);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("STA CONNECTED: ");
    Serial.println(WiFi.localIP());
  }

  configTime(19800, 0, "pool.ntp.org");

  // HTTP routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogsCSV);
  server.on("/clearlogs", handleClearLogs);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/events", handleEvents);

  // ⭐ REQUIRED FIX FOR TOKEN CHECK ⭐
  const char *headerKeys[] = {"X-DISPENSE-TOKEN"};
  server.collectHeaders(headerKeys, 1);

  server.begin();
}

// ===== LOOP =====
void loop()
{
  server.handleClient();

  if (dispensing)
  {
    static unsigned long lastCalc = 0;
    static unsigned long lastPulseSnap = 0;

    if (millis() - lastCalc >= 1000)
    {
      // 1 second flow calc
      detachInterrupt(digitalPinToInterrupt(FLOW_PIN));

      unsigned long pulseDelta = pulseCount - lastPulseSnap;
      lastPulseSnap = pulseCount;
      lastCalc = millis();

      attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, RISING);

      // Compute mL from pulses
      float flowLmin = pulseDelta / calibrationFactor;
      float mlThisSecond = (flowLmin / 60.0f) * 1000.0f;

      dispensed_ml += mlThisSecond;

      // No flow fail-safe
      if (pulseDelta == 0 && (millis() - lastPulseMillis > NO_PULSE_TIMEOUT_MS))
      {
        dispensing = false;
        pumpStop();
        appendLog("FAIL_NO_FLOW", current_mode, dispensed_ml, 0);
        sseSend(makeStateJSON());
        return;
      }

      // Timeout fail-safe
      if (millis() - dispenseStartMillis > MAX_DISPENSE_MS)
      {
        dispensing = false;
        pumpStop();
        float price = (dispensed_ml / 1000.0f) * PRICE_PER_LITER;
        appendLog("FAIL_TIMEOUT", current_mode, dispensed_ml, price);
        sseSend(makeStateJSON());
        return;
      }

      // Target reached
      if (dispensed_ml >= target_ml)
      {
        dispensing = false;
        pumpStop();
        float price = (dispensed_ml / 1000.0f) * PRICE_PER_LITER;
        appendLog("SUCCESS", current_mode, dispensed_ml, price);
        sseSend(makeStateJSON());
        return;
      }

      sseSend(makeStateJSON());
    }
  }

  // SSE keepalive
  if (sseClientActive && millis() - lastSSEPing > 15000)
  {
    sseClient.print(": ping\n\n");
    lastSSEPing = millis();
  }
}
