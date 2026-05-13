#pragma once

#include <ArduinoJson.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
typedef BearSSL::WiFiClientSecure DashlySecureClient;
#define DASHLY_HAS_HTTPS 1
#elif defined(ESP32)
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
typedef WiFiClientSecure DashlySecureClient;
#define DASHLY_HAS_HTTPS 1
#else
#define DASHLY_HAS_HTTPS 0
#endif

#if __has_include(<WebSocketsClient.h>)
#include <WebSocketsClient.h>
#define DASHLY_HAS_REALTIME 1
#else
#define DASHLY_HAS_REALTIME 0
typedef int WStype_t;
#endif

class DashlyClient {
public:
  typedef void (*PinUpdateCallback)(String pin, String value);
  static constexpr const char* DEFAULT_BASE_URL = "https://dashlyiot.com";

  enum ConnectionMode { WIFI_MODE };
  enum TransportMode { AUTO_TRANSPORT, REALTIME_TRANSPORT, QUEUE_TRANSPORT, PIN_POLL_TRANSPORT };

  DashlyClient(const char* token, const char* appBaseUrl = DEFAULT_BASE_URL)
    : _token(token),
      _baseUrl(appBaseUrl),
      _callback(nullptr),
      _transportMode(AUTO_TRANSPORT),
      _realtimeEnabled(false),
      _presenceOnlineSent(false),
      _restoreOnReconnect(false),
      _presenceKey("device"),
      _presenceIntervalMs(30000),
      _lastPresenceMs(0),
      _lastPollMs(0),
      _pollIntervalMs(1500),
      _fallbackPin("v1"),
      _lastFallbackValue(""),
      _fallbackDisabledByPolicy(false),
      _lastHttpCode(0),
      _ref(1) {}

  /** Last HTTP status from the gateway (-1 = connect/begin failed). */
  int lastHttpCode() const { return _lastHttpCode; }

  void setConnectionMode(ConnectionMode mode) { (void)mode; }
  void onWrite(PinUpdateCallback callback) { _callback = callback; }
  void setTransportMode(TransportMode mode) { _transportMode = mode; }
  void setQueuePollIntervalMs(unsigned long intervalMs) { _pollIntervalMs = intervalMs < 500 ? 500 : intervalMs; }
  void setQueueFallbackPin(const char* pin) { if (pin && *pin) _fallbackPin = String(pin); }

  bool networkReady() { return isNetworkReady(); }
  bool shouldRestoreOnReconnect() const { return _restoreOnReconnect; }

  bool beginRealtimeAuto(const char* presenceKey = "device") {
    if (!isNetworkReady()) return false;
#if !DASHLY_HAS_HTTPS
    return false;
#else
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/bootstrap").c_str(), body);
    if (code != 200 || body.length() == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

    String projectId = doc["project_id"] | "";
    String sbUrl = doc["supabase_url"] | "";
    String sbAnon = doc["supabase_anon_key"] | "";
    _restoreOnReconnect = doc["restore_on_reconnect"] | false;
    if (projectId.length() == 0 || sbUrl.length() == 0 || sbAnon.length() == 0) return false;

    _presenceKey = String(presenceKey);
    bool wantRealtime = (_transportMode == AUTO_TRANSPORT || _transportMode == REALTIME_TRANSPORT);
    if (wantRealtime && beginRealtime(projectId, sbUrl, sbAnon)) return true;
    if (_transportMode == REALTIME_TRANSPORT) return false;

    _realtimeEnabled = false;
    unsigned long now = millis();
    if (sendPresence(true)) {
      _presenceOnlineSent = true;
      _lastPresenceMs = now;
    }
    return true;
#endif
  }

  void run() {
    if (!isNetworkReady()) {
      _presenceOnlineSent = false;
      return;
    }

    if (_realtimeEnabled) {
#if DASHLY_HAS_REALTIME
      _ws.loop();
#endif
    } else {
      runHttpFallback();
    }

    unsigned long now = millis();
    // Re-establish online heartbeat after reconnect (especially important for ESP8266 pin-poll mode).
    if (!_presenceOnlineSent) {
      if (sendPresence(true)) {
        _presenceOnlineSent = true;
        _lastPresenceMs = now;
      }
      return;
    }
    if (_presenceOnlineSent && (now - _lastPresenceMs >= _presenceIntervalMs)) {
      if (sendPresence(true)) _lastPresenceMs = now;
    }
  }

  String virtualRead(const char* pin) {
    if (!isNetworkReady()) return "";
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/pins?pin=" + String(pin)).c_str(), body);
    return code == 200 ? body : "";
  }

  bool virtualWrite(const char* pin, const String& value) {
    if (!isNetworkReady()) return false;
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/update?pin=" + String(pin) + "&value=" + value).c_str(), body);
    return code == 200;
  }
  bool virtualWrite(const char* pin, int value) { return virtualWrite(pin, String(value)); }
  bool virtualWrite(const char* pin, float value) { return virtualWrite(pin, String(value, 2)); }

private:
  const char* _token;
  const char* _baseUrl;
  PinUpdateCallback _callback;
  TransportMode _transportMode;
  bool _realtimeEnabled;
  bool _presenceOnlineSent;
  bool _restoreOnReconnect;

  String _projectId, _sbUrl, _sbAnon, _topic, _presenceKey;
  String _wsHost, _wsPath;
  unsigned long _presenceIntervalMs, _lastPresenceMs, _lastPollMs, _pollIntervalMs, _ref;
  String _fallbackPin, _lastFallbackValue;
  bool _fallbackDisabledByPolicy;
  int _lastHttpCode;

#if DASHLY_HAS_REALTIME
  WebSocketsClient _ws;
#endif

  bool isNetworkReady() {
#if defined(ESP8266) || defined(ESP32)
    return WiFi.status() == WL_CONNECTED;
#else
    return false;
#endif
  }

  bool beginRealtime(const String& projectId, const String& sbUrl, const String& sbAnon) {
    _projectId = projectId;
    _sbUrl = sbUrl;
    _sbAnon = sbAnon;
    // Must match the browser channel name (`project:<uuid>`). Anon JWT cannot subscribe to
    // postgres_changes on `pins` (RLS), so joinTopic() must not request postgres_changes — only
    // broadcast (same as dashboard's base channel config before .on(postgres_changes)).
    _topic = "project:" + _projectId;
    _realtimeEnabled = false;
#if !DASHLY_HAS_REALTIME
    return false;
#else
    String host = _sbUrl;
    host.replace("https://", "");
    host.replace("http://", "");
    int slash = host.indexOf("/");
    if (slash > 0) host = host.substring(0, slash);
    _wsHost = host;
    _wsPath = "/realtime/v1/websocket?apikey=" + _sbAnon + "&vsn=1.0.0";

    _ws.beginSSL(_wsHost.c_str(), 443, _wsPath.c_str());
    _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) { this->handleWs(type, payload, length); });
    _ws.enableHeartbeat(30000, 5000, 2);
    _ws.setReconnectInterval(3000);
    _realtimeEnabled = true;
    return true;
#endif
  }

  void joinTopic() {
#if DASHLY_HAS_REALTIME
    JsonDocument doc;
    doc["topic"] = _topic;
    doc["event"] = "phx_join";
    doc["ref"] = String(_ref++);

    JsonObject cfg = doc["payload"]["config"].to<JsonObject>();
    cfg["broadcast"]["self"] = true;
    cfg["presence"]["key"] = _presenceKey;
    cfg["postgres_changes"].to<JsonArray>(); // empty — anon cannot use pins replication (RLS)

    String out; serializeJson(doc, out); _ws.sendTXT(out);
#endif
  }

  void trackPresence() {
#if DASHLY_HAS_REALTIME
    JsonDocument doc;
    doc["topic"] = _topic;
    doc["event"] = "track";
    doc["ref"] = String(_ref++);
    doc["payload"]["online_at"] = millis();
    doc["payload"]["type"] = "device";
    doc["payload"]["user_type"] = "device";
    doc["payload"]["source"] = "arduino";
    String out; serializeJson(doc, out); _ws.sendTXT(out);
#endif
  }

  void handleWs(WStype_t type, uint8_t* payload, size_t length) {
#if DASHLY_HAS_REALTIME
    (void)length;
    if (type == WStype_CONNECTED) { joinTopic(); return; }
    if (type == WStype_DISCONNECTED || type == WStype_ERROR) {
      sendPresence(false);
      _presenceOnlineSent = false;
      return;
    }
    if (type != WStype_TEXT) return;

    JsonDocument doc;
    if (deserializeJson(doc, (char*)payload) != DeserializationError::Ok) return;
    String event = doc["event"] | "";

    if (event == "phx_reply") {
      String status = doc["payload"]["status"] | "";
      if (status.length() == 0) status = doc["payload"]["response"]["status"] | "";
      if (status == "ok" && !_presenceOnlineSent) {
        trackPresence();
        unsigned long now = millis();
        if (sendPresence(true)) {
          _presenceOnlineSent = true;
          _lastPresenceMs = now;
        }
      }
      return;
    }

    if (event == "broadcast" && _callback) {
      String sub = doc["payload"]["event"] | "";
      if (sub == "pin_update") {
        String pin = doc["payload"]["payload"]["pin"] | "";
        String value = doc["payload"]["payload"]["value"] | "";
        if (pin.length() > 0) _callback(pin, value);
      }
      return;
    }

    if (event == "postgres_changes" && _callback) {
      String pin = doc["payload"]["data"]["new"]["pin_label"] | "";
      String value = doc["payload"]["data"]["new"]["value"] | "";
      if (pin.length() == 0) pin = doc["payload"]["data"]["record"]["pin_label"] | "";
      if (value.length() == 0) value = doc["payload"]["data"]["record"]["value"] | "";
      if (pin.length() > 0) _callback(pin, value);
    }
#else
    (void)type; (void)payload; (void)length;
#endif
  }

  void runHttpFallback() {
    unsigned long now = millis();
    if (now - _lastPollMs < _pollIntervalMs) return;
    _lastPollMs = now;
    pollPin();
  }

  void pollPin() {
    if (_fallbackDisabledByPolicy || _fallbackPin.length() == 0) return;
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/pins?pin=" + _fallbackPin + "&live=1").c_str(), body);
    if (code == 403) { _fallbackDisabledByPolicy = true; return; }
    if (code != 200 || body.length() == 0) return;
    if (_lastFallbackValue.length() == 0) { _lastFallbackValue = body; return; }
    if (body != _lastFallbackValue) {
      _lastFallbackValue = body;
      if (_callback) _callback(_fallbackPin, body);
    }
  }

  bool sendPresence(bool online) {
    if (!isNetworkReady()) return false;
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/presence?online=" + String(online ? 1 : 0)).c_str(), body);
    return code == 200;
  }

  int doGet(const char* fullUrl, String& outBody) { return httpRequest("GET", fullUrl, nullptr, outBody); }

  int httpRequest(const char* method, const char* fullUrl, const char* payload, String& outBody) {
    outBody = "";
#if !DASHLY_HAS_HTTPS
    (void)method; (void)fullUrl; (void)payload;
    _lastHttpCode = -1;
    return -1;
#else
    int code = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
      DashlySecureClient client;
#if defined(ESP8266)
      client.setBufferSizes(512, 512);
#endif
      client.setInsecure();
      HTTPClient http;
      bool started = false;
#if defined(ESP8266)
      started = http.begin(client, String(fullUrl));
      http.useHTTP10(true);
#else
      started = http.begin(client, fullUrl);
#endif
      if (!started) {
        _lastHttpCode = -1;
        code = -1;
        continue;
      }
      http.setTimeout(15000);
      http.setReuse(false);
      http.addHeader("Authorization", String("Bearer ") + String(_token));
      if (String(method) == "POST") {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(payload ? payload : "{}");
      } else {
        code = http.GET();
      }
      if (code > 0) outBody = http.getString();
      http.end();
      if (code > 0) {
        _lastHttpCode = code;
        return code;
      }
#if defined(ESP8266)
      delay(250);
#endif
    }
    _lastHttpCode = code;
    return code;
#endif
  }

  String normalizeBase(const char* in) {
    String s = String(in ? in : DEFAULT_BASE_URL);
    if (s.endsWith("/")) s.remove(s.length() - 1, 1);
    return s;
  }
};
