// firmware/src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------- CONFIG ----------
const char *AP_SSID = "MilkPoC";
const char *AP_PASS = "12345678";

const char *HOME_SSID = "HomeWifi";
const char *HOME_PASS = "f1nallyw1f1c0nnect10n1nmyh0me";

const char *DISPENSE_API_TOKEN = "DMTKN_4nFh92xQ7sY8wLf0BqZp3cR1vKdTg"; // set same in backend .env

const char *LOG_PATH = "/logs.csv";
const float PRICE_PER_LITER = 50.0f; // change as needed

WebServer server(80);

// ---------- State ----------
bool dispensing = false;
float dispensed_ml = 0.0f;
float target_ml = 0.0f;
String current_mode = "ml";
unsigned long lastSimTick = 0;
float simulated_flow_per_sec = 30.0f;

// SSE
WiFiClient sseClient;
bool sseClientActive = false;
unsigned long lastSSEPing = 0;

// ---------- Time & Logs ----------
String getISTTimestamp()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (!localtime_r(&now, &timeinfo))
    return String("Time N/A");
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

void appendLog(const String &status, const String &mode, float amount_ml, float price)
{
  ensureLogHeader();
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (!f)
    return;
  String line = status + "," + mode + "," + String(amount_ml, 2) + "," + String(price, 2) + "," + getISTTimestamp();
  f.println(line);
  f.close();
  Serial.println(line);
}

// ---------- SSE ----------
void sseSend(const String &jsonData)
{
  if (!sseClientActive || !sseClient || !sseClient.connected())
  {
    sseClientActive = false;
    return;
  }
  String d = jsonData;
  d.replace("\n", "\\n");
  sseClient.print("data: ");
  sseClient.print(d);
  sseClient.print("\n\n");
}

String currentStateJSON()
{
  StaticJsonDocument<256> doc;
  doc["dispensing"] = dispensing;
  doc["dispensed_ml"] = dispensed_ml;
  doc["target_ml"] = target_ml;
  doc["mode"] = current_mode;
  doc["price_per_l"] = PRICE_PER_LITER;
  String out;
  serializeJson(doc, out);
  return out;
}

// ---------- HTTP Handlers ----------
void handleRoot()
{
  File f = SPIFFS.open("/index.html", FILE_READ);
  if (!f)
  {
    server.send(500, "text/plain", "index.html missing");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

void handleStatus() { server.send(200, "application/json; charset=utf-8", currentStateJSON()); }

void handleLogsCSV()
{
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!f)
  {
    server.send(200, "text/plain", "No logs yet.");
    return;
  }
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClearLogs()
{
  if (SPIFFS.exists(LOG_PATH))
    SPIFFS.remove(LOG_PATH);
  ensureLogHeader();
  server.send(200, "text/plain", "Logs cleared");
}

void handleStart()
{
  if (!server.hasArg("value") || !server.hasArg("mode"))
  {
    server.send(400, "text/plain", "Missing value or mode");
    return;
  }

  // Optional token check (backend must send this header)
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
  float tml = 0.0f;
  if (smode == "ml")
    tml = inputVal;
  else if (smode == "litre")
    tml = inputVal * 1000.0f;
  else if (smode == "cost")
  {
    float litres = inputVal / PRICE_PER_LITER;
    tml = litres * 1000.0f;
  }
  else
  {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }

  if (tml <= 0.0f)
  {
    server.send(400, "text/plain", "Invalid target");
    return;
  }

  dispensing = true;
  dispensed_ml = 0.0f;
  target_ml = tml;
  current_mode = smode;
  lastSimTick = millis();
  simulated_flow_per_sec = 20.0f + random(0, 21); // 20..40 mL/s

  server.send(200, "text/plain", "Started");
  sseSend(currentStateJSON());
  Serial.printf("Started: mode=%s target=%.2f mL flow=%.2f mL/s\n", current_mode.c_str(), target_ml, simulated_flow_per_sec);
}

void handleStop()
{
  if (dispensing)
  {
    dispensing = false;
    float finalAmount = dispensed_ml;
    float price = (finalAmount / 1000.0f) * PRICE_PER_LITER;
    appendLog("SUCCESS", current_mode, finalAmount, price);
    sseSend(currentStateJSON());
    Serial.printf("Stopped: dispensed=%.2f mL price=%.2f\n", finalAmount, price);
  }
  server.send(200, "text/plain", "Stopped");
}

void handleEvents()
{
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();
  client.println(": connected\n");

  if (sseClient && sseClient.connected())
    sseClient.stop();
  sseClient = client;
  sseClientActive = true;
  lastSSEPing = millis();
  sseSend(currentStateJSON());
}

void handleExtras() { server.send(204); }

// ---------- Setup ----------
void setup()
{
  Serial.begin(115200);
  delay(200);

  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS mount failed");
  ensureLogHeader();

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 6000)
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.print("STA IP: "), Serial.println(WiFi.localIP());
  else
    Serial.println("STA connect failed");

  configTime(19800, 0, "pool.ntp.org"); // IST offset

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/logs", HTTP_GET, handleLogsCSV);
  server.on("/clearlogs", HTTP_GET, handleClearLogs);
  server.on("/events", HTTP_GET, handleEvents);
  server.onNotFound(handleExtras);

  server.begin();
  randomSeed(analogRead(0));
  Serial.println("Web server started");
}

// ---------- Loop ----------
void simulationTick()
{
  if (!dispensing)
    return;
  unsigned long now = millis();
  if (now - lastSimTick >= 250)
  {
    float dt = (now - lastSimTick) / 1000.0f;
    lastSimTick = now;
    float factor = 0.9f + (random(0, 21) / 100.0f);
    float delta = simulated_flow_per_sec * factor * dt;
    dispensed_ml += delta;
    if (dispensed_ml > target_ml)
      dispensed_ml = target_ml;
    sseSend(currentStateJSON());
    if (dispensed_ml >= target_ml - 0.001f)
    {
      dispensing = false;
      float price = (dispensed_ml / 1000.0f) * PRICE_PER_LITER;
      appendLog("SUCCESS", current_mode, dispensed_ml, price);
      sseSend(currentStateJSON());
      Serial.printf("Completed: %.2f mL price=%.2f\n", dispensed_ml, price);
    }
  }
}

void sseKeepalive()
{
  if (!sseClientActive || !sseClient || !sseClient.connected())
  {
    sseClientActive = false;
    return;
  }
  if (millis() - lastSSEPing >= 15000)
  {
    sseClient.print(": ping\n\n");
    lastSSEPing = millis();
  }
}

void loop()
{
  server.handleClient();
  simulationTick();
  sseKeepalive();
}
