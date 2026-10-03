#pragma once

/**
 * DashlyClient — official ESP8266 / ESP32 transport for Dashly IoT.
 *
 * Versioning (semver):
 *   - MAJOR: breaking API or default transport behavior changes
 *   - MINOR: backward-compatible features (new setters, optional query params)
 *   - PATCH: bug fixes only
 *
 * v2.2.0: Fast, non-blocking transport (ESP32)
 *   - All network I/O runs in background FreeRTOS tasks. loop() never waits for the
 *     network: virtualWrite() only queues the value, run() only delivers received
 *     commands to the onWrite() callback (called from the task that calls run()).
 *   - The Realtime WebSocket has its own task, so commands are received instantly,
 *     even while telemetry is being uploaded.
 *   - HTTPS connection is kept alive and reused (no new TLS handshake per request).
 *   - Commands that were already delivered over WebSocket are NOT executed a second
 *     time when the command-queue poll returns them (they are only acknowledged).
 *   - Command backlog that is still pending when the device boots is discarded
 *     (acknowledged without executing), so old commands never replay after a reboot.
 *   - Command-queue poll is only a safety net: default interval 10 s, and an extra
 *     poll runs right after every WebSocket (re)connect.
 *   - Phoenix heartbeat every 20 s; reconnect interval 1 s.
 *   - beginRealtimeAuto() is non-blocking: it starts the tasks and returns true; the
 *     bootstrap is retried in the background until it succeeds.
 *   - ESP8266 keeps working in the old cooperative mode (everything runs inside run()).
 *   - Optional debug logging: define DASHLY_DEBUG before including this header.
 *
 * v2.1.2: Realtime stability fixes (Phoenix heartbeat, non-blocking disconnect).
 *
 * v2.1.1 (2026-05): Command-queue poll fallback for dashboard → device pin writes
 *   when Realtime WebSocket broadcast is missed. Device virtualWrite uses queue=0
 *   so telemetry/status does not echo back through the command queue.
 */
#define DASHLY_CLIENT_VERSION "2.2.0"

#include <ArduinoJson.h>
#include <string.h>

#ifdef DASHLY_DEBUG
#define DASHLY_LOG(msg) Serial.println(F(msg))
#else
#define DASHLY_LOG(msg) ((void)0)
#endif

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
typedef BearSSL::WiFiClientSecure DashlySecureClient;
#define DASHLY_HAS_HTTPS 1
#define DASHLY_USE_TASKS 0
#elif defined(ESP32)
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
typedef WiFiClientSecure DashlySecureClient;
#define DASHLY_HAS_HTTPS 1
#define DASHLY_USE_TASKS 1
#else
#define DASHLY_HAS_HTTPS 0
#define DASHLY_USE_TASKS 0
#endif

#if __has_include(<WebSocketsClient.h>)
#include <WebSocketsClient.h>
#define DASHLY_HAS_REALTIME 1
#else
#define DASHLY_HAS_REALTIME 0
typedef int WStype_t;
#endif

#if DASHLY_USE_TASKS
typedef SemaphoreHandle_t DashlyMutex;
#define DASHLY_TAKE(m) do { if (m) xSemaphoreTake((m), portMAX_DELAY); } while (0)
#define DASHLY_GIVE(m) do { if (m) xSemaphoreGive((m)); } while (0)
#else
typedef int DashlyMutex;
#define DASHLY_TAKE(m) ((void)0)
#define DASHLY_GIVE(m) ((void)0)
#endif

static inline void dashlyCopy(char* dst, const char* src, size_t n) {
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

class DashlyClient {
public:
  typedef void (*PinUpdateCallback)(String pin, String value);
  static constexpr const char* DEFAULT_BASE_URL = "https://dashlyiot.com";
  static constexpr const char* LIBRARY_VERSION = DASHLY_CLIENT_VERSION;

  enum ConnectionMode { WIFI_MODE };
  enum TransportMode { AUTO_TRANSPORT, REALTIME_TRANSPORT, QUEUE_TRANSPORT, PIN_POLL_TRANSPORT };

  DashlyClient(const char* token, const char* appBaseUrl = DEFAULT_BASE_URL)
    : _token(token), _baseUrl(appBaseUrl) {}

  /** Last HTTP status from the gateway (-1 = connect/begin failed). */
  int lastHttpCode() const { return _lastHttpCode; }

  /** True while the Realtime WebSocket is connected. */
  bool isConnected() const { return _wsConnected; }

  static const char* libraryVersion() { return LIBRARY_VERSION; }

  /**
   * Parse `#RRGGBB`, `RRGGBB`, or `#RGB` for WS2812B / dashboard pin values.
   * Returns false for off/empty/invalid (use before treating value as a color).
   */
  static bool parseHexColor(const String& in, uint8_t& r, uint8_t& g, uint8_t& b) {
    String s = in;
    s.trim();
    if (s.length() == 0 || s == "0") return false;
    if (s.startsWith("%23")) s = s.substring(3);
    while (s.startsWith("#")) s = s.substring(1);
    if (s.length() == 3) {
      s = String(s[0]) + String(s[0]) + String(s[1]) + String(s[1]) + String(s[2]) + String(s[2]);
    }
    if (s.length() != 6) return false;
    char buf[8];
    s.toCharArray(buf, sizeof(buf));
    char* end = nullptr;
    unsigned long n = strtoul(buf, &end, 16);
    if (end == nullptr || *end != '\0') return false;
    r = static_cast<uint8_t>((n >> 16) & 0xFF);
    g = static_cast<uint8_t>((n >> 8) & 0xFF);
    b = static_cast<uint8_t>(n & 0xFF);
    return true;
  }

  void setConnectionMode(ConnectionMode mode) { (void)mode; }
  void onWrite(PinUpdateCallback callback) { _callback = callback; }
  void setTransportMode(TransportMode mode) { _transportMode = mode; }
  void setQueuePollIntervalMs(unsigned long intervalMs) { _pollIntervalMs = intervalMs < 500 ? 500 : intervalMs; }
  /** Safety-net poll interval for the dashboard command queue (default 10000 ms). Minimum 500 ms. */
  void setCommandPollIntervalMs(unsigned long intervalMs) { _commandPollIntervalMs = intervalMs < 500 ? 500 : intervalMs; }
  /** Enable/disable HTTP command-queue polling (default on). Disable only if you rely solely on WS. */
  void setCommandPollEnabled(bool enabled) { _commandPollEnabled = enabled; }
  void setQueueFallbackPin(const char* pin) { if (pin && *pin) _fallbackPin = String(pin); }

  bool networkReady() { return isNetworkReady(); }
  bool shouldRestoreOnReconnect() const { return _restoreOnReconnect; }

  /**
   * Starts the background transport. Non-blocking: returns true right away; the
   * bootstrap/WebSocket connection is established (and retried) in the background.
   */
  bool beginRealtimeAuto(const char* presenceKey = "device") {
#if !DASHLY_HAS_HTTPS
    return false;
#else
    if (_started) return true;
    _presenceKey = String(presenceKey ? presenceKey : "device");
#if DASHLY_USE_TASKS
    _qLock = xSemaphoreCreateMutex();
    _httpLock = xSemaphoreCreateMutex();
    _started = true;
    BaseType_t a = xTaskCreate(wsTaskEntry, "dashly_ws", 10240, this, 2, &_wsTask);
    BaseType_t b = xTaskCreate(httpTaskEntry, "dashly_http", 10240, this, 1, &_httpTask);
    if (a != pdPASS || b != pdPASS) {
      DASHLY_LOG(">>> Dashly: task start failed");
      return false;
    }
#else
    _started = true;
#endif
    return true;
#endif
  }

  /** Call from loop(). Delivers received commands to the onWrite() callback. Never blocks on the network (ESP32). */
  void run() {
    if (!_started) beginRealtimeAuto(_presenceKey.c_str());
#if !DASHLY_USE_TASKS
    netStep();  // ESP8266: cooperative mode
#endif
    dispatchIncoming();
  }

  /** Synchronous read (blocks until the server answers). Avoid in time-critical code. */
  String virtualRead(const char* pin) {
    if (!isNetworkReady()) return "";
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/pins?pin=" + String(pin)).c_str(), body);
    return code == 200 ? body : "";
  }

  /** Queues the value for sending and returns immediately (ESP32). Latest value per pin wins. */
  bool virtualWrite(const char* pin, const String& value) {
    if (!pin || !*pin) return false;
    if (!_started) return sendUpdate(pin, value.c_str());  // legacy use without begin: send synchronously
    DASHLY_TAKE(_qLock);
    bool ok = _out.push(pin, value.c_str(), true);
    DASHLY_GIVE(_qLock);
    return ok;
  }
  bool virtualWrite(const char* pin, int value) { return virtualWrite(pin, String(value)); }
  bool virtualWrite(const char* pin, float value) { return virtualWrite(pin, String(value, 2)); }

private:
  static constexpr unsigned long WS_DEDUPE_MS = 400;
  static constexpr unsigned long SEEN_TTL_MS = 600000UL;  // WS-delivered commands are remembered for 10 min

  // ---------- small fixed-size message queue ----------
  struct Msg {
    char pin[16];
    char value[48];
  };
  struct MsgQueue {
    static const uint8_t CAP = 16;
    Msg items[CAP];
    uint8_t head = 0;
    uint8_t count = 0;
    bool push(const char* pin, const char* value, bool coalesce) {
      if (coalesce) {
        char p[16];
        dashlyCopy(p, pin, sizeof(p));
        for (uint8_t i = 0; i < count; i++) {
          Msg& m = items[(head + i) % CAP];
          if (strcmp(m.pin, p) == 0) {
            dashlyCopy(m.value, value, sizeof(m.value));
            return true;
          }
        }
      }
      if (count >= CAP) return false;
      Msg& m = items[(head + count) % CAP];
      dashlyCopy(m.pin, pin, sizeof(m.pin));
      dashlyCopy(m.value, value, sizeof(m.value));
      count++;
      return true;
    }
    bool pop(Msg& out) {
      if (count == 0) return false;
      out = items[head];
      head = (head + 1) % CAP;
      count--;
      return true;
    }
  };

  struct Seen {
    char pin[16];
    char value[48];
    unsigned long at;
    bool used;
  };

  const char* _token;
  const char* _baseUrl;
  PinUpdateCallback _callback = nullptr;
  TransportMode _transportMode = AUTO_TRANSPORT;

  volatile bool _started = false;
  volatile bool _bootstrapped = false;
  volatile bool _realtimeMode = false;
  volatile bool _wsConnected = false;
  volatile bool _wsJoined = false;
  volatile bool _pollNow = false;
  volatile bool _needPresence = false;
  bool _wsBegun = false;
  bool _restoreOnReconnect = false;
  bool _commandPollEnabled = true;
  bool _presenceOnlineSent = false;
  bool _syncDone = false;  // false until the boot-time command backlog has been discarded
  bool _lastPollOk = true;

  String _projectId, _sbUrl, _sbAnon, _topic, _presenceKey = "device";
  String _wsHost, _wsPath;
  unsigned long _presenceIntervalMs = 30000, _lastPresenceMs = 0, _presenceRetryAt = 0;
  unsigned long _pollIntervalMs = 900, _lastPollMs = 0;
  unsigned long _commandPollIntervalMs = 10000, _lastCommandPollMs = 0;
  unsigned long _lastBootstrapTry = 0, _sendRetryAt = 0, _lastHeartbeatMs = 0;
  uint32_t _lastCommandId = 0;
  unsigned long _ref = 1;
  String _fallbackPin = "v1", _lastFallbackValue;
  bool _fallbackDisabledByPolicy = false;
  volatile int _lastHttpCode = 0;

  MsgQueue _out;
  MsgQueue _in;
  Seen _seen[32] = {};
  uint8_t _seenNext = 0;
  char _lastWsPin[16] = "";
  char _lastWsValue[48] = "";
  unsigned long _lastWsAt = 0;

  DashlyMutex _qLock = 0;
  DashlyMutex _httpLock = 0;

#if DASHLY_USE_TASKS
  TaskHandle_t _wsTask = nullptr;
  TaskHandle_t _httpTask = nullptr;
  DashlySecureClient _secure;
  HTTPClient _http;
  bool _secureInit = false;
#endif

#if DASHLY_HAS_REALTIME
  WebSocketsClient _ws;
#endif

  // ---------- tasks ----------
#if DASHLY_USE_TASKS
  static TickType_t msToTicks(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
  }
  static void wsTaskEntry(void* p) {
    DashlyClient* self = static_cast<DashlyClient*>(p);
    for (;;) {
      self->wsStep();
      vTaskDelay(msToTicks(5));
    }
  }
  static void httpTaskEntry(void* p) {
    DashlyClient* self = static_cast<DashlyClient*>(p);
    for (;;) {
      self->httpStep();
      vTaskDelay(msToTicks(20));
    }
  }
#endif

  void netStep() {
    httpStep();
    wsStep();
  }

  bool isNetworkReady() {
#if defined(ESP8266) || defined(ESP32)
    return WiFi.status() == WL_CONNECTED;
#else
    return false;
#endif
  }

  // ---------- delivering commands to the sketch ----------
  void dispatchIncoming() {
    for (uint8_t n = 0; n < 8; n++) {
      Msg m;
      DASHLY_TAKE(_qLock);
      bool ok = _in.pop(m);
      DASHLY_GIVE(_qLock);
      if (!ok) break;
      if (_callback) _callback(String(m.pin), String(m.value));
    }
  }

  // Command that arrived over the WebSocket: remember it, then queue it for the sketch.
  void acceptWsCommand(const String& pin, const String& value) {
    if (pin.length() == 0) return;
    unsigned long now = millis();
    DASHLY_TAKE(_qLock);
    bool dup = (strcmp(_lastWsPin, pin.c_str()) == 0 && strcmp(_lastWsValue, value.c_str()) == 0 &&
                (now - _lastWsAt) < WS_DEDUPE_MS);
    if (!dup) {
      dashlyCopy(_lastWsPin, pin.c_str(), sizeof(_lastWsPin));
      dashlyCopy(_lastWsValue, value.c_str(), sizeof(_lastWsValue));
      _lastWsAt = now;
      Seen& s = _seen[_seenNext];
      _seenNext = (_seenNext + 1) % 32;
      dashlyCopy(s.pin, pin.c_str(), sizeof(s.pin));
      dashlyCopy(s.value, value.c_str(), sizeof(s.value));
      s.at = now;
      s.used = true;
      _in.push(pin.c_str(), value.c_str(), false);
    }
    DASHLY_GIVE(_qLock);
  }

  // True (and forgets the entry) if this command was already delivered over the WebSocket.
  bool consumeSeen(const String& pin, const String& value, unsigned long now) {
    bool found = false;
    DASHLY_TAKE(_qLock);
    for (uint8_t i = 0; i < 32; i++) {
      Seen& s = _seen[i];
      if (!s.used) continue;
      if ((now - s.at) >= SEEN_TTL_MS) {
        s.used = false;
        continue;
      }
      if (strcmp(s.pin, pin.c_str()) == 0 && strcmp(s.value, value.c_str()) == 0) {
        s.used = false;
        found = true;
        break;
      }
    }
    DASHLY_GIVE(_qLock);
    return found;
  }

  // Command found only in the HTTP queue (the WebSocket missed it): deliver it.
  void pushPolledCommand(const String& pin, const String& value) {
    DASHLY_TAKE(_qLock);
    _in.push(pin.c_str(), value.c_str(), false);
    DASHLY_GIVE(_qLock);
  }

  // ---------- WebSocket side ----------
  void sendPhoenixHeartbeat() {
#if DASHLY_HAS_REALTIME
    String out = String("{\"topic\":\"phoenix\",\"event\":\"heartbeat\",\"payload\":{},\"ref\":\"") +
                 String(_ref++) + "\"}";
    _ws.sendTXT(out);
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
    cfg["postgres_changes"].to<JsonArray>();

    String out;
    serializeJson(doc, out);
    _ws.sendTXT(out);
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
    String out;
    serializeJson(doc, out);
    _ws.sendTXT(out);
#endif
  }

  void wsStep() {
#if DASHLY_HAS_REALTIME
    if (!_bootstrapped || !_realtimeMode) return;
    if (!_wsBegun) {
      _ws.beginSSL(_wsHost.c_str(), 443, _wsPath.c_str());
      _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) { this->handleWs(type, payload, length); });
      _ws.enableHeartbeat(15000, 3000, 2);
      _ws.setReconnectInterval(1000);
      _wsBegun = true;
    }
    _ws.loop();
    unsigned long now = millis();
    if (now - _lastHeartbeatMs >= 20000) {
      _lastHeartbeatMs = now;
      if (_wsConnected) sendPhoenixHeartbeat();
    }
#endif
  }

  void handleWs(WStype_t type, uint8_t* payload, size_t length) {
#if DASHLY_HAS_REALTIME
    (void)length;
    if (type == WStype_CONNECTED) {
      DASHLY_LOG(">>> WS connected");
      _wsConnected = true;
      _wsJoined = false;
      joinTopic();
      return;
    }
    if (type == WStype_DISCONNECTED || type == WStype_ERROR) {
      if (_wsConnected) DASHLY_LOG(">>> WS disconnected");
      _wsConnected = false;
      _wsJoined = false;
      return;
    }
    if (type != WStype_TEXT) return;

    JsonDocument doc;
    if (deserializeJson(doc, (char*)payload) != DeserializationError::Ok) return;
    String event = doc["event"] | "";

    if (event == "phx_reply") {
      String topic = doc["topic"] | "";
      if (topic != _topic) return;  // ignore heartbeat replies
      String status = doc["payload"]["status"] | "";
      if (status.length() == 0) status = doc["payload"]["response"]["status"] | "";
      if (status == "ok" && !_wsJoined) {
        _wsJoined = true;
        trackPresence();
        _needPresence = true;  // HTTP task sends presence
        _pollNow = true;       // HTTP task re-syncs the command queue after (re)connect
      }
      return;
    }

    if (event == "broadcast") {
      String sub = doc["payload"]["event"] | "";
      if (sub == "pin_update") {
        String pin = doc["payload"]["payload"]["pin"] | "";
        String value = doc["payload"]["payload"]["value"] | "";
        if (pin.length() == 0) {
          pin = doc["payload"]["pin"] | "";
          value = doc["payload"]["value"] | "";
        }
        acceptWsCommand(pin, value);
      }
      return;
    }

    if (event == "postgres_changes") {
      String pin = doc["payload"]["data"]["new"]["pin_label"] | "";
      String value = doc["payload"]["data"]["new"]["value"] | "";
      if (pin.length() == 0) pin = doc["payload"]["data"]["record"]["pin_label"] | "";
      if (value.length() == 0) value = doc["payload"]["data"]["record"]["value"] | "";
      acceptWsCommand(pin, value);
    }
#else
    (void)type;
    (void)payload;
    (void)length;
#endif
  }

  // ---------- HTTP side ----------
  void httpStep() {
    if (!isNetworkReady()) {
      _presenceOnlineSent = false;
      return;
    }
    unsigned long now = millis();

    if (!_bootstrapped) {
      if (_lastBootstrapTry != 0 && (now - _lastBootstrapTry) < 3000) return;
      _lastBootstrapTry = now;
      doBootstrap();
      return;
    }

    // 1. Outgoing values (highest priority)
    if (now >= _sendRetryAt) {
      Msg m;
      DASHLY_TAKE(_qLock);
      bool have = _out.pop(m);
      DASHLY_GIVE(_qLock);
      if (have) {
        if (!sendUpdate(m.pin, m.value)) {
          DASHLY_TAKE(_qLock);
          _out.push(m.pin, m.value, true);  // keep it, newer value for the same pin wins
          DASHLY_GIVE(_qLock);
          _sendRetryAt = now + 1000;
        }
        return;
      }
    }

    // 2. Presence
    bool presenceDue = !_presenceOnlineSent || _needPresence || (now - _lastPresenceMs >= _presenceIntervalMs);
    if (presenceDue && now >= _presenceRetryAt) {
      if (sendPresence(true)) {
        _presenceOnlineSent = true;
        _needPresence = false;
        _lastPresenceMs = now;
      } else {
        _presenceRetryAt = now + 3000;
      }
      return;
    }

    // 3. Fallback pin polling (only when the Realtime transport is not in use)
    if (!_realtimeMode) runHttpFallback();

    // 4. Command-queue poll: safety net + re-sync after every WebSocket (re)connect
    if (_commandPollEnabled && _presenceOnlineSent) {
      unsigned long interval = _realtimeMode ? _commandPollIntervalMs
                                             : (_commandPollIntervalMs < 1200 ? _commandPollIntervalMs : 1200);
      bool fast = (_pollNow || !_syncDone) && _lastPollOk;  // re-sync / backlog drain: quick, but never a hot loop
      bool due = (now - _lastCommandPollMs >= interval) || (fast && (now - _lastCommandPollMs >= 300));
      if (due) {
        _pollNow = false;
        pollDeviceCommands(now);
      }
    }
  }

  void doBootstrap() {
#if !DASHLY_HAS_HTTPS
    return;
#else
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/bootstrap").c_str(), body);
    if (code != 200 || body.length() == 0) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;

    String projectId = doc["project_id"] | "";
    String sbUrl = doc["supabase_url"] | "";
    String sbAnon = doc["supabase_anon_key"] | "";
    _restoreOnReconnect = doc["restore_on_reconnect"] | false;
    if (projectId.length() == 0 || sbUrl.length() == 0 || sbAnon.length() == 0) return;

    _projectId = projectId;
    _sbUrl = sbUrl;
    _sbAnon = sbAnon;
    _topic = "project:" + _projectId;

    String host = _sbUrl;
    host.replace("https://", "");
    host.replace("http://", "");
    int slash = host.indexOf("/");
    if (slash > 0) host = host.substring(0, slash);
    _wsHost = host;
    _wsPath = "/realtime/v1/websocket?apikey=" + _sbAnon + "&vsn=1.0.0";

    bool wantRealtime = (_transportMode == AUTO_TRANSPORT || _transportMode == REALTIME_TRANSPORT);
    _realtimeMode = wantRealtime && (DASHLY_HAS_REALTIME == 1);
    _bootstrapped = true;  // set last: the WebSocket task starts using the values above
    DASHLY_LOG(">>> Dashly bootstrap OK");
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
    if (code == 403) {
      _fallbackDisabledByPolicy = true;
      return;
    }
    if (code != 200 || body.length() == 0) return;
    if (_lastFallbackValue.length() == 0) {
      _lastFallbackValue = body;
      return;
    }
    if (body != _lastFallbackValue) {
      _lastFallbackValue = body;
      pushPolledCommand(_fallbackPin, body);
    }
  }

  /**
   * Poll pending dashboard/automation commands (GET /api/device/commands).
   *  - boot-time backlog: acknowledged and discarded (never executed)
   *  - command already received over WebSocket: acknowledged only (no second execution)
   *  - command the WebSocket missed: delivered to the sketch, then acknowledged
   */
  void pollDeviceCommands(unsigned long now) {
    _lastCommandPollMs = now;

    String body;
    String url = normalizeBase(_baseUrl) + "/api/device/commands?pending=1&limit=5";
    int code = doGet(url.c_str(), body);
    _lastPollOk = (code == 200);
    if (code != 200 || body.length() == 0) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;
    JsonArray arr = doc["commands"].as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) {
      _syncDone = true;
      return;
    }

    for (JsonObject cmd : arr) {
      uint32_t id = cmd["id"] | 0;
      String pin = cmd["pin_label"] | "";
      String value = cmd["value"] | "";
      if (id == 0) continue;
      if (id > _lastCommandId) _lastCommandId = id;
      if (pin.length() > 0 && _syncDone) {
        bool alreadyDelivered = consumeSeen(pin, value, millis());
        if (!alreadyDelivered) pushPolledCommand(pin, value);
      }
      ackDeviceCommand(id);
    }
    if (!_syncDone) _pollNow = true;  // keep draining the boot-time backlog quickly
  }

  bool ackDeviceCommand(uint32_t id) {
    String payload = String("{\"id\":") + String(id) + "}";
    String resp;
    String url = normalizeBase(_baseUrl) + "/api/device/ack";
    return httpRequest("POST", url.c_str(), payload.c_str(), resp) == 200;
  }

  bool sendUpdate(const char* pin, const char* value) {
    if (!isNetworkReady()) return false;
    String body;
    // queue=0: device telemetry/status must not re-enter the dashboard command queue.
    String url = normalizeBase(_baseUrl) + "/api/update?pin=" + urlEncode(String(pin)) + "&value=" +
                 urlEncode(String(value)) + "&queue=0";
    return doGet(url.c_str(), body) == 200;
  }

  bool sendPresence(bool online) {
    if (!isNetworkReady()) return false;
    String body;
    int code = doGet((normalizeBase(_baseUrl) + "/api/presence?online=" + String(online ? 1 : 0)).c_str(), body);
    return code == 200;
  }

  int doGet(const char* fullUrl, String& outBody) { return httpRequest("GET", fullUrl, nullptr, outBody); }

#if defined(ESP32)
  // ESP32: one persistent TLS connection, reused by every request (no handshake per request).
  int httpRequest(const char* method, const char* fullUrl, const char* payload, String& outBody) {
    outBody = "";
    DASHLY_TAKE(_httpLock);
    int code = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!_secureInit) {
        _secure.setInsecure();
        _secureInit = true;
      }
      if (!_http.begin(_secure, String(fullUrl))) {
        code = -1;
        _secure.stop();
        continue;
      }
      _http.setReuse(true);
      _http.setTimeout(8000);
      _http.addHeader("Authorization", String("Bearer ") + String(_token));
      if (String(method) == "POST") {
        _http.addHeader("Content-Type", "application/json");
        code = _http.POST(payload ? payload : "{}");
      } else {
        code = _http.GET();
      }
      if (code > 0) outBody = _http.getString();
      _http.end();
      if (code > 0) {
        _lastHttpCode = code;
        DASHLY_GIVE(_httpLock);
        return code;
      }
      _secure.stop();  // stale/broken connection: the next attempt reconnects
    }
    _lastHttpCode = code;
    DASHLY_GIVE(_httpLock);
    return code;
  }
#else
  // ESP8266 / other: one connection per request (low memory use).
  int httpRequest(const char* method, const char* fullUrl, const char* payload, String& outBody) {
    outBody = "";
#if !DASHLY_HAS_HTTPS
    (void)method;
    (void)fullUrl;
    (void)payload;
    _lastHttpCode = -1;
    return -1;
#else
    int code = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
      DashlySecureClient client;
      client.setBufferSizes(512, 512);
      client.setInsecure();
      HTTPClient http;
      bool started = http.begin(client, String(fullUrl));
      http.useHTTP10(true);
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
      delay(250);
    }
    _lastHttpCode = code;
    return code;
#endif
  }
#endif

  String normalizeBase(const char* in) {
    String s = String(in ? in : DEFAULT_BASE_URL);
    if (s.endsWith("/")) s.remove(s.length() - 1, 1);
    return s;
  }

  static String urlEncode(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < in.length(); ++i) {
      const char c = in[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
          c == '.' || c == '~') {
        out += c;
      } else if (c == ' ') {
        out += '+';
      } else {
        out += '%';
        out += hex[(c >> 4) & 0xF];
        out += hex[c & 0xF];
      }
    }
    return out;
  }
};
