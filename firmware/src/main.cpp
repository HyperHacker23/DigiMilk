#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>

const char *AP_SSID = "MilkPoC";
const char *AP_PASS = "12345678";

const char *HOME_SSID = "HomeWifi";
const char *HOME_PASS = "f1nallyw1f1c0nnect10n1nmyh0me";

const char *LOG_PATH = "/logs.csv";
const float PRICE_PER_LITER = 50.0f;

const char *DISPENSE_API_TOKEN = "DMTKN_4nFh92xQ7sY8wLf0BqZp3cR1vKdTg";

// ------------------- REAL HARDWARE PINS -------------------
#define FLOW_PIN 27  // YF-S401 Yellow wire
#define MOTOR_IN1 14 // L298N IN1
#define MOTOR_IN2 12 // L298N IN2

// ------------------- FLOW SENSOR VARIABLES -------------------
volatile uint32_t pulseCount = 0;
float mlPerPulse = 0.45f; // YF-S401 typical calibration (tune later)

// ------------------- DISPENSING STATE -------------------
bool dispensing = false;
float dispensed_ml = 0.0f;
float target_ml = 0.0f;
String current_mode = "ml";

// ------------------- SERVER -------------------
WebServer server(80);
WiFiClient sseClient;
bool sseClientActive = false;
unsigned long lastSSEPing = 0;

// ------------------- ISR -------------------
void IRAM_ATTR flowISR()
{
  pulseCount++;
}

// ------------------- LOGGING -------------------
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
  String line = status + "," + mode + "," + String(amount_ml, 2) + "," + String(price, 2) + "," + getISTTimestamp();
  f.println(line);
  f.close();
}

// ------------------- SSE -------------------
void sseSend(const String &jsonData)
{
  if (!sseClientActive || !sseClient.connected())
    return;

  sseClient.print("data: ");
  sseClient.print(jsonData);
  sseClient.print("\n\n");
}

String makeStateJSON()
{
  StaticJsonDocument<128> doc;
  doc["dispensing"] = dispensing;
  doc["dispensed_ml"] = dispensed_ml;
  doc["target_ml"] = target_ml;
  doc["mode"] = current_mode;

  String out;
  serializeJson(doc, out);
  return out;
}

// ------------------- MOTOR CONTROL -------------------
void pumpStart()
{
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
}

void pumpStop()
{
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
}

// ------------------- HTTP HANDLERS -------------------
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
  if (!server.hasArg("value") || !server.hasArg("mode"))
  {
    server.send(400, "text/plain", "Missing value or mode");
    return;
  }

  if (server.hasHeader("X-DISPENSE-TOKEN"))
  {
    if (server.header("X-DISPENSE-TOKEN") != DISPENSE_API_TOKEN)
    {
      server.send(403, "text/plain", "Forbidden");
      return;
    }
  }

  float inputVal = server.arg("value").toFloat();
  String smode = server.arg("mode");

  if (smode == "ml")
    target_ml = inputVal;
  else if (smode == "litre")
    target_ml = inputVal * 1000.0f;
  else if (smode == "cost")
    target_ml = (inputVal / PRICE_PER_LITER) * 1000.0f;

  if (target_ml <= 0)
  {
    server.send(400, "text/plain", "Invalid target");
    return;
  }

  pulseCount = 0;
  dispensed_ml = 0.0f;
  current_mode = smode;
  dispensing = true;

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

// ------------------- SETUP -------------------
void setup()
{
  Serial.begin(115200);
  SPIFFS.begin(true);

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(FLOW_PIN, flowISR, RISING);

  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pumpStop();

  WiFi.softAP(AP_SSID, AP_PASS);

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);

  configTime(19800, 0, "pool.ntp.org");

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogsCSV);
  server.on("/clearlogs", handleClearLogs);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/events", handleEvents);

  server.begin();
}

// ------------------- LOOP -------------------
void loop()
{
  server.handleClient();

  if (dispensing)
  {
    dispensed_ml = pulseCount * mlPerPulse;

    if (dispensed_ml >= target_ml)
    {
      dispensing = false;
      pumpStop();

      float price = (dispensed_ml / 1000.0f) * PRICE_PER_LITER;
      appendLog("SUCCESS", current_mode, dispensed_ml, price);

      sseSend(makeStateJSON());
    }

    static unsigned long lastSSE = 0;
    if (millis() - lastSSE > 200)
    {
      sseSend(makeStateJSON());
      lastSSE = millis();
    }
  }

  if (sseClientActive && millis() - lastSSEPing > 15000)
  {
    sseClient.print(": ping\n\n");
    lastSSEPing = millis();
  }
}
