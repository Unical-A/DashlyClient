#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DashlyClient.h"

// Կարգավորումներ
const char* ssid = "Ucom7091_2.4G";
const char* password = "vazasvazas001";
const char* token = "4d28af02-7411-4aab-adf7-b165cf10fda0";

const int relayPin = 5; // Physical LED/relay pin
const char* commandPin = "v1"; // Dashboard button writes here
const char* statusPin = "v0";  // Device reports LED state here
const char* temperaturePin = "v2"; // DS18B20 publishes here
const int ds18b20Pin = 4; // Change if your sensor data pin is different

OneWire oneWire(ds18b20Pin);
DallasTemperature sensors(&oneWire);
unsigned long lastTempPublishMs = 0;
const unsigned long tempPublishIntervalMs = 5000;

DashlyClient dashly(token);
bool wasNetworkReady = false;

void onDashlyUpdate(String pin, String value) {
  Serial.println("Pin: " + pin + " Value: " + value);

  // Default contract: command from v1, status reported on v0.
  if (pin == commandPin) {
    if (value == "1") {
      digitalWrite(relayPin, HIGH);         // Միացնում ենք ֆիզիկական Լեդը
      dashly.virtualWrite(statusPin, "1");  // Push status back to dashboard LED
      Serial.println("LED ON - Status sent to v0");
    } else {
      digitalWrite(relayPin, LOW);          // Անջատում ենք ֆիզիկական Լեդը
      dashly.virtualWrite(statusPin, "0");  // Push status back to dashboard LED
      Serial.println("LED OFF - Status sent to v0");
    }
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

  bool sent = dashly.virtualWrite(temperaturePin, String(tempC, 2));
  Serial.println("Temp C -> v2: " + String(tempC, 2) + "  api HTTP:" + String(dashly.lastHttpCode()) +
                 (sent ? " OK" : " FAIL"));
}

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  sensors.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  dashly.setConnectionMode(DashlyClient::WIFI_MODE);
  dashly.setTransportMode(DashlyClient::AUTO_TRANSPORT);
  dashly.onWrite(onDashlyUpdate);

  // Single-token mode: backend-ից ավտոմատ վերցնում է realtime config-ը
  if (!dashly.beginRealtimeAuto("device")) {
    Serial.println("Realtime bootstrap failed. HTTP:" + String(dashly.lastHttpCode()) +
                   " (GET /api/bootstrap — token or server config).");
  } else {
    Serial.println("Realtime bootstrap OK");
    restoreStateFromPinsIfEnabled();
    if (!dashly.shouldRestoreOnReconnect()) {
      dashly.virtualWrite(statusPin, "0"); // Keep current behavior when toggle is off
    }
  }
}

void loop() {
  dashly.run();
  bool nowReady = dashly.networkReady();
  if (nowReady && !wasNetworkReady) {
    // Internet just came back: pull latest command once from pins table.
    restoreStateFromPinsIfEnabled();
  }
  if (nowReady) publishTemperatureIfDue();
  wasNetworkReady = nowReady;
}
