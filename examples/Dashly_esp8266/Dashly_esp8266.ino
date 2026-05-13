// #include <ESP8266WiFi.h>
// #include <OneWire.h>
// #include <DallasTemperature.h>
// #include "DashlyClient.h"

// // Device config
// const char* ssid = "Ucom7091_2.4G";
// const char* password = "vazasvazas001";
// const char* token = "9694501f-e055-4321-951f-738065beca6c";

// const int relayPin = 14;          // D1 R2 onboard wiring target (change if needed)
// const char* commandPin = "v1";    // Dashboard button command pin
// const char* statusPin = "v0";     // Device reports status here
// const char* temperaturePin = "v2"; // DS18B20 publishes here
// const int ds18b20Pin = 5;         // Change if your sensor data pin is different

// OneWire oneWire(ds18b20Pin);
// DallasTemperature sensors(&oneWire);
// unsigned long lastTempPublishMs = 0;
// const unsigned long tempPublishIntervalMs = 5000;

// DashlyClient dashly(token);
// bool wasNetworkReady = false;

// void onDashlyUpdate(String pin, String value) {
//   Serial.println("Pin: " + pin + " Value: " + value);

//   if (pin == commandPin) {
//     bool on = (value == "1");
//     digitalWrite(relayPin, on ? HIGH : LOW);
//     dashly.virtualWrite(statusPin, on ? "1" : "0");
//     Serial.println(on ? "LED ON - Status sent to v0" : "LED OFF - Status sent to v0");
//   }
// }

// void restoreStateFromPinsIfEnabled() {
//   if (!dashly.shouldRestoreOnReconnect()) return;
//   String last = dashly.virtualRead(commandPin);
//   if (last.length() > 0) {
//     onDashlyUpdate(commandPin, last);
//   }
// }

// void publishTemperatureIfDue() {
//   unsigned long now = millis();
//   if (now - lastTempPublishMs < tempPublishIntervalMs) return;
//   lastTempPublishMs = now;

//   sensors.requestTemperatures();
//   float tempC = sensors.getTempCByIndex(0);
//   if (tempC == DEVICE_DISCONNECTED_C) {
//     Serial.println("DS18B20 read failed");
//     return;
//   }

//   dashly.virtualWrite(temperaturePin, String(tempC, 2));
//   Serial.println("Temp C -> v2: " + String(tempC, 2));
// }

// void setup() {
//   Serial.begin(115200);
//   pinMode(relayPin, OUTPUT);
//   digitalWrite(relayPin, LOW);
//   sensors.begin();

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.println("\nWiFi Connected!");

//   dashly.setConnectionMode(DashlyClient::WIFI_MODE);
//   // ESP8266: use lightweight pin polling transport for maximum stability.
//   dashly.setTransportMode(DashlyClient::PIN_POLL_TRANSPORT);
//   // Optional: tune queue polling for low load / low latency tradeoff.
//   dashly.setQueuePollIntervalMs(5000);
//   dashly.setQueueFallbackPin(commandPin);
//   dashly.onWrite(onDashlyUpdate);

//   if (!dashly.beginRealtimeAuto("device")) {
//     Serial.println("Bootstrap failed. HTTP:" + String(dashly.lastHttpCode()) + " (GET /api/bootstrap).");
//   } else {
//     Serial.println("Bootstrap OK");
//     restoreStateFromPinsIfEnabled();
//     if (!dashly.shouldRestoreOnReconnect()) {
//       dashly.virtualWrite(statusPin, "0");
//     }
//   }
// }

// void loop() {
//   dashly.run();
//   bool nowReady = dashly.networkReady();
//   if (nowReady && !wasNetworkReady) {
//     restoreStateFromPinsIfEnabled();
//   }
//   if (nowReady) publishTemperatureIfDue();
//   wasNetworkReady = nowReady;
// }
