#include <ESP8266WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DashlyClient.h>

// WiFi and Dashly credentials — set these before upload (Dashboard → device token).
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* token = "YOUR_DEVICE_AUTH_TOKEN";

const int relayPin = D1;          // D1 R2 onboard wiring target (change if needed)
const char* commandPin = "v1";    // Dashboard button command pin
const char* statusPin = "v0";     // Device reports status here
const char* temperaturePin = "v2"; // DS18B20 publishes here
const int ds18b20Pin = D2;         // Change if your sensor data pin is different

OneWire oneWire(ds18b20Pin);
DallasTemperature sensors(&oneWire);
unsigned long lastTempPublishMs = 0;
const unsigned long tempPublishIntervalMs = 5000;
unsigned long lastRecoverAttemptMs = 0;
unsigned long firstHttpFailMs = 0;
int consecutiveHttpFailCount = 0;
const int reconnectFailThreshold = 3;   // Try WiFi reconnect after ~15s of fails (3*5s)
const int rebootFailThreshold = 8;      // Reboot after prolonged stuck state (~40s)
const unsigned long recoverCooldownMs = 4000;
const unsigned long maxFailWindowMs = 120000;

DashlyClient dashly(token);
bool wasNetworkReady = false;

void handleHttpHealthAfterWrite() {
  const int code = dashly.lastHttpCode();
  const unsigned long now = millis();

  if (code == 200) {
    consecutiveHttpFailCount = 0;
    firstHttpFailMs = 0;
    return;
  }
  // Only run aggressive recovery for transport-level failures (-1 begin/connect).
  if (code != -1) return;

  if (firstHttpFailMs == 0) firstHttpFailMs = now;
  if (now - firstHttpFailMs > maxFailWindowMs) {
    // Reset fail window so sporadic issues do not accumulate forever.
    firstHttpFailMs = now;
    consecutiveHttpFailCount = 0;
  }
  consecutiveHttpFailCount++;

  if (consecutiveHttpFailCount >= rebootFailThreshold) {
    Serial.println("HTTP stuck (-1). Auto rebooting...");
    delay(200);
    ESP.restart();
    return;
  }

  if (consecutiveHttpFailCount >= reconnectFailThreshold && now - lastRecoverAttemptMs >= recoverCooldownMs) {
    lastRecoverAttemptMs = now;
    Serial.println("HTTP -1 repeated. Reconnecting WiFi...");
    WiFi.disconnect(true);
    delay(250);
    WiFi.begin(ssid, password);
  }
}

void onDashlyUpdate(String pin, String value) {
  Serial.println("Pin: " + pin + " Value: " + value);

  // Default contract: command from v1, status reported on v0.
  if (pin == commandPin) {
    bool on = (value == "1");
    digitalWrite(relayPin, on ? HIGH : LOW);
    dashly.virtualWrite(statusPin, on ? "1" : "0");
    Serial.println(on ? "LED ON - Status sent to v0" : "LED OFF - Status sent to v0");
  }
}

void restoreStateFromPinsIfEnabled() {
  if (!dashly.shouldRestoreOnReconnect()) return;
  String last = dashly.virtualRead(commandPin);
  if (last.length() > 0) {
    onDashlyUpdate(commandPin, last);
  }
}

void publishTemperatureIfDue() {
  unsigned long now = millis();
  if (now - lastTempPublishMs < tempPublishIntervalMs) return;
  lastTempPublishMs = now;

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 read failed");
    return;
  }

  dashly.virtualWrite(temperaturePin, String(tempC, 2));
  Serial.println("Temp C -> v2: " + String(tempC, 2) + "  api HTTP:" + String(dashly.lastHttpCode()) +
                 (dashly.lastHttpCode() == 200 ? " OK" : " FAIL"));
  handleHttpHealthAfterWrite();
}

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  sensors.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    ESP.wdtFeed();
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  dashly.setConnectionMode(DashlyClient::WIFI_MODE);
  // ESP8266: use lightweight pin polling transport for maximum stability.
  dashly.setTransportMode(DashlyClient::PIN_POLL_TRANSPORT);
  // Poll command pin at least every 500 ms (DashlyClient minimum). Slower = noticeable button lag.
  dashly.setQueuePollIntervalMs(500);
  dashly.setQueueFallbackPin(commandPin);
  dashly.onWrite(onDashlyUpdate);

  // Single-token mode: pulls realtime config from the backend automatically.
  if (!dashly.beginRealtimeAuto("device")) {
    Serial.println("Bootstrap failed. HTTP:" + String(dashly.lastHttpCode()) + " (GET /api/bootstrap).");
  } else {
    Serial.println("Bootstrap OK");
    restoreStateFromPinsIfEnabled();
    if (!dashly.shouldRestoreOnReconnect()) {
      dashly.virtualWrite(statusPin, "0"); // Keep current behavior when restore toggle is off
    }
  }
}

void loop() {
  ESP.wdtFeed();
  dashly.run();
  bool nowReady = dashly.networkReady();
  if (nowReady && !wasNetworkReady) {
    // Internet just came back: pull latest command once from pins table.
    restoreStateFromPinsIfEnabled();
  }
  if (nowReady) publishTemperatureIfDue();
  wasNetworkReady = nowReady;
}
