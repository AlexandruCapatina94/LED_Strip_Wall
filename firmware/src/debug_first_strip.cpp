#include <Arduino.h>
#include <ArduinoOTA.h>
#include <FastLED.h>
#include <WiFi.h>

#include "wifi_credentials.h"

#ifndef WIFI_SSID
#error "Define WIFI_SSID in wifi_credentials.h"
#endif

#ifndef WIFI_PASSWORD
#error "Define WIFI_PASSWORD in wifi_credentials.h"
#endif

namespace {
constexpr uint8_t DATA_PIN = 2;
// Configure how many physical LEDs exist on this strip and how many of them
// belong to each logical zone (used by rain/snake effects).
#ifndef DEBUG_STRIP_ZONE_COUNT
#define DEBUG_STRIP_ZONE_COUNT 5 // number of controllable zones on the debug strip
#endif

#ifndef DEBUG_LEDS_PER_ZONE
#define DEBUG_LEDS_PER_ZONE 1
#endif

#ifndef DEBUG_STRIP_REVERSED
#define DEBUG_STRIP_REVERSED true
#endif

#ifndef DEBUG_COLOR_ORDER
#define DEBUG_COLOR_ORDER GRB
#endif

constexpr bool COLOR_INPUT_IS_GRB = true;

constexpr uint16_t LEDS_PER_ZONE = DEBUG_LEDS_PER_ZONE;
constexpr uint8_t NUM_STRIPS = 1;
constexpr uint8_t DEFAULT_BRIGHTNESS = 200;
constexpr float DEFAULT_SPEED = 1.0f;
constexpr uint8_t RAIN_TRAIL = 6;
constexpr uint8_t RAIN_FADE = 48;
constexpr uint8_t SNAKE_LENGTH = 12;
constexpr uint8_t SNAKE_FADE = 32;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr char OTA_HOSTNAME[] = "led-strip-debug";
constexpr uint16_t TELNET_PORT = 2323;
constexpr uint8_t HEARTBEAT_PIN = 8;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 500;

#ifndef WIFI_DEBUG_ENABLED
#define WIFI_DEBUG_ENABLED 0
#endif

struct StripDescriptor {
  uint16_t startZone;
  uint16_t zoneCount;
  bool reversed;
};

constexpr StripDescriptor STRIPS[NUM_STRIPS] = {{0, DEBUG_STRIP_ZONE_COUNT, DEBUG_STRIP_REVERSED}};

constexpr uint16_t TOTAL_ZONES = [] {
  uint16_t sum = 0;
  for (const auto &strip : STRIPS) {
    sum += strip.zoneCount;
  }
  return sum;
}();

constexpr uint32_t TOTAL_LEDS = TOTAL_ZONES * LEDS_PER_ZONE;

CRGB zoneBuffer[TOTAL_ZONES];
CRGB leds[TOTAL_LEDS];

enum class EffectType {
  Solid,
  Rain,
  Snake,
  Breathing,
};

struct StripRuntime {
  float accumulator = 0.0f;
};

EffectType currentEffect = EffectType::Rain;
CRGB masterColor = CRGB(30, 120, 0);
uint8_t globalBrightness = DEFAULT_BRIGHTNESS;
float speedMultiplier = DEFAULT_SPEED;
StripRuntime stripState[NUM_STRIPS];
uint32_t lastFrameMillis = 0;
constexpr uint32_t FRAME_INTERVAL_MS = 1000 / 120; // target ~120 FPS
uint32_t lastWifiAttemptMillis = 0;
bool wifiConnected = false;
bool wifiReady = false;
bool otaReady = false;
uint8_t pendingBrightness = DEFAULT_BRIGHTNESS;

#if WIFI_DEBUG_ENABLED
WiFiServer telnetServer(TELNET_PORT);
WiFiClient telnetClient;
#endif
String serialBuffer;
String telnetBuffer;
uint32_t lastHeartbeat = 0;
bool heartbeatState = false;
float breathingPhase = 0.0f; // normalized 0-1 phase
constexpr uint32_t BREATH_PERIOD_MS = 3000;
constexpr float BREATH_PERIOD_SECONDS = BREATH_PERIOD_MS / 1000.0f;
constexpr uint8_t BREATH_MIN_LEVEL = 20; // avoid flicker near zero PWM

bool connectWiFi();
void configureOTA();
void maintainWiFiAndOTA(uint32_t now);

void broadcastLine(const __FlashStringHelper *msg) {
  Serial.println(msg);
#if WIFI_DEBUG_ENABLED
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(msg);
  }
#endif
}

void broadcastLine(const String &msg) {
  Serial.println(msg);
#if WIFI_DEBUG_ENABLED
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(msg);
  }
#endif
}

void broadcastRaw(const __FlashStringHelper *msg) {
  Serial.print(msg);
#if WIFI_DEBUG_ENABLED
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
  }
#endif
}

void broadcastRaw(const String &msg) {
  Serial.print(msg);
#if WIFI_DEBUG_ENABLED
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
  }
#endif
}

uint16_t logicalToZoneIndex(uint8_t stripIndex, uint16_t logicalIndex) {
  const StripDescriptor &strip = STRIPS[stripIndex];
  if (logicalIndex >= strip.zoneCount) {
    return strip.startZone;
  }
  return strip.reversed ? (strip.startZone + strip.zoneCount - 1 - logicalIndex)
                        : (strip.startZone + logicalIndex);
}

void setZoneColor(uint8_t stripIndex, uint16_t logicalIndex, const CRGB &color) {
  zoneBuffer[logicalToZoneIndex(stripIndex, logicalIndex)] = color;
}

void clearZones() {
  for (uint16_t i = 0; i < TOTAL_ZONES; ++i) {
    zoneBuffer[i] = CRGB::Black;
  }
}

void fadeZones(uint8_t amount) {
  for (uint16_t i = 0; i < TOTAL_ZONES; ++i) {
    zoneBuffer[i].fadeToBlackBy(amount);
  }
}

void flushZonesToPhysical() {
  for (const StripDescriptor &strip : STRIPS) {
    const uint32_t stripBaseLED = static_cast<uint32_t>(strip.startZone) * LEDS_PER_ZONE;
    for (uint16_t zone = 0; zone < strip.zoneCount; ++zone) {
      const uint16_t zoneIndex = strip.startZone + zone;
      const CRGB color = zoneBuffer[zoneIndex];
      const uint32_t ledBase = stripBaseLED + static_cast<uint32_t>(zone) * LEDS_PER_ZONE;
      for (uint8_t led = 0; led < LEDS_PER_ZONE; ++led) {
        leds[ledBase + led] = color;
      }
    }
  }
}

void updateSolid() {
  for (uint16_t i = 0; i < TOTAL_ZONES; ++i) {
    zoneBuffer[i] = masterColor;
  }
}

void updateRain(float deltaSeconds) {
  fadeZones(RAIN_FADE);
  const float step = speedMultiplier * deltaSeconds;
  for (uint8_t stripIndex = 0; stripIndex < NUM_STRIPS; ++stripIndex) {
    StripRuntime &state = stripState[stripIndex];
    const uint16_t length = STRIPS[stripIndex].zoneCount;
    state.accumulator += step * length;
    while (state.accumulator >= static_cast<float>(length + RAIN_TRAIL)) {
      state.accumulator -= static_cast<float>(length + RAIN_TRAIL);
    }
    int32_t head = static_cast<int32_t>(state.accumulator);
    for (uint8_t trail = 0; trail < RAIN_TRAIL; ++trail) {
      int32_t position = head - static_cast<int32_t>(trail);
      if (position >= 0 && position < length) {
        CRGB color = masterColor;
        color.fadeToBlackBy(trail * (255 / RAIN_TRAIL));
        setZoneColor(stripIndex, static_cast<uint16_t>(position), color);
      }
    }
  }
}

void updateSnake(float deltaSeconds) {
  fadeZones(SNAKE_FADE);
  const float step = speedMultiplier * deltaSeconds;
  for (uint8_t stripIndex = 0; stripIndex < NUM_STRIPS; ++stripIndex) {
    StripRuntime &state = stripState[stripIndex];
    const uint16_t length = STRIPS[stripIndex].zoneCount;
    state.accumulator += step * length;
    while (state.accumulator >= static_cast<float>(length)) {
      state.accumulator -= static_cast<float>(length);
    }
    const int32_t head = static_cast<int32_t>(state.accumulator);
    for (uint8_t segment = 0; segment < SNAKE_LENGTH; ++segment) {
      int32_t position = head - static_cast<int32_t>(segment);
      if (position < 0) {
        position += length;
      }
      if (position >= 0 && position < length) {
        CRGB color = masterColor;
        color.fadeLightBy(segment * (255 / SNAKE_LENGTH));
        setZoneColor(stripIndex, static_cast<uint16_t>(position), color);
      }
    }
  }
}

void updateBreathing(float deltaSeconds) {
  if (BREATH_PERIOD_SECONDS <= 0.0f) {
    return;
  }
  const float normalizedIncrement = (deltaSeconds * speedMultiplier) / BREATH_PERIOD_SECONDS;
  breathingPhase += normalizedIncrement;
  if (breathingPhase >= 1.0f) {
    breathingPhase -= static_cast<uint32_t>(breathingPhase);
  }
  const uint8_t phaseByte = static_cast<uint8_t>(breathingPhase * 255.0f);
  const uint8_t wave = sin8(phaseByte);
  uint8_t level = scale8(wave, globalBrightness);
  if (level > 0 && level < BREATH_MIN_LEVEL) {
    level = BREATH_MIN_LEVEL;
  }
  pendingBrightness = level;
  for (uint16_t zone = 0; zone < TOTAL_ZONES; ++zone) {
    zoneBuffer[zone] = masterColor;
  }
}

void updateEffect(float deltaSeconds) {
  pendingBrightness = globalBrightness;
  switch (currentEffect) {
  case EffectType::Solid:
    updateSolid();
    break;
  case EffectType::Rain:
    updateRain(deltaSeconds);
    break;
  case EffectType::Snake:
    updateSnake(deltaSeconds);
    break;
  case EffectType::Breathing:
    updateBreathing(deltaSeconds);
    break;
  }
}

void resetRuntimeState() {
  for (auto &state : stripState) {
    state.accumulator = 0.0f;
  }
  breathingPhase = 0.0f;
}

bool parseUInt8(const String &token, uint8_t &value) {
  if (token.length() == 0) {
    return false;
  }
  char *end = nullptr;
  const long parsed = strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0' || parsed < 0 || parsed > 255) {
    return false;
  }
  value = static_cast<uint8_t>(parsed);
  return true;
}

bool parseFloat(const String &token, float &value) {
  if (token.length() == 0) {
    return false;
  }
  char *end = nullptr;
  value = strtof(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0') {
    return false;
  }
  return true;
}

void printStatus(Print &out) {
  out.println(F("--- Debug LED Strip Status ---"));
  out.print(F("Effect: "));
  switch (currentEffect) {
  case EffectType::Solid:
    out.println(F("solid"));
    break;
  case EffectType::Rain:
    out.println(F("rain"));
    break;
  case EffectType::Snake:
    out.println(F("snake"));
    break;
  case EffectType::Breathing:
    out.println(F("breathing"));
    break;
  }
  out.print(F("Color (R,G,B): "));
  out.print(masterColor.r);
  out.print(F(","));
  out.print(masterColor.g);
  out.print(F(","));
  out.println(masterColor.b);
  out.print(F("Brightness: "));
  out.println(globalBrightness);
  out.print(F("Speed multiplier: "));
  out.println(speedMultiplier, 3);
  out.print(F("WiFi: "));
  if (WiFi.status() == WL_CONNECTED) {
    out.print(F("connected ("));
    out.print(WiFi.localIP());
    out.println(F(")"));
  } else {
    out.println(F("disconnected"));
  }
  out.print(F("Telnet: "));
  #if WIFI_DEBUG_ENABLED
  if (telnetClient && telnetClient.connected()) {
    out.println(F("client connected"));
  } else {
    out.println(F("waiting for client"));
  }
  #else
  out.println(F("disabled"));
  #endif
  out.println(F("Commands: effect <solid|rain|snake|breathing>, color <r> <g> <b>, brightness <0-255>, speed <multiplier>, reconnect, status, help"));
}

void setEffectFromToken(const String &token, Print &out) {
  EffectType newEffect;
  if (token.equalsIgnoreCase(F("solid"))) {
    newEffect = EffectType::Solid;
  } else if (token.equalsIgnoreCase(F("rain"))) {
    newEffect = EffectType::Rain;
  } else if (token.equalsIgnoreCase(F("snake"))) {
    newEffect = EffectType::Snake;
  } else if (token.equalsIgnoreCase(F("breathing"))) {
    newEffect = EffectType::Breathing;
  } else {
    out.println(F("Unknown effect. Options: solid, rain, snake, breathing"));
    return;
  }
  currentEffect = newEffect;
  resetRuntimeState();
  out.print(F("Effect set to "));
  out.println(token);
}

void setColorFromTokens(const String tokens[], uint8_t count, Print &out) {
  if (count < 3) {
    out.println(F("Usage: color <r> <g> <b>"));
    return;
  }
  uint8_t r, g, b;
  if (!parseUInt8(tokens[0], r) || !parseUInt8(tokens[1], g) || !parseUInt8(tokens[2], b)) {
    out.println(F("Color values must be 0-255"));
    return;
  }
  if constexpr (COLOR_INPUT_IS_GRB) {
    masterColor = CRGB(g, r, b);
  } else {
    masterColor = CRGB(r, g, b);
  }
  out.print(F("Color updated to "));
  out.print(r);
  out.print(F(","));
  out.print(g);
  out.print(F(","));
  out.println(b);
}

void handleConsoleCommand(const String &line, Print &out) {
  if (line.length() == 0) {
    return;
  }
  constexpr uint8_t MAX_TOKENS = 4;
  String tokens[MAX_TOKENS];
  uint8_t tokenCount = 0;
  int start = 0;
  while (start < line.length() && tokenCount < MAX_TOKENS) {
    int end = line.indexOf(' ', start);
    if (end == -1) {
      end = line.length();
    }
    String token = line.substring(start, end);
    token.trim();
    if (token.length() > 0) {
      tokens[tokenCount++] = token;
    }
    start = end + 1;
  }
  if (tokenCount == 0) {
    return;
  }
  const String &command = tokens[0];
  if (command.equalsIgnoreCase(F("effect"))) {
    if (tokenCount < 2) {
      out.println(F("Usage: effect <solid|rain|snake>"));
    } else {
      setEffectFromToken(tokens[1], out);
    }
  } else if (command.equalsIgnoreCase(F("color"))) {
    setColorFromTokens(&tokens[1], tokenCount - 1, out);
  } else if (command.equalsIgnoreCase(F("brightness"))) {
    if (tokenCount < 2) {
      out.println(F("Usage: brightness <0-255>"));
    } else {
      uint8_t value;
      if (!parseUInt8(tokens[1], value)) {
        out.println(F("Brightness must be 0-255"));
      } else {
        globalBrightness = value;
        pendingBrightness = globalBrightness;
        FastLED.setBrightness(globalBrightness);
        out.print(F("Brightness set to "));
        out.println(globalBrightness);
      }
    }
  } else if (command.equalsIgnoreCase(F("speed"))) {
    if (tokenCount < 2) {
      out.println(F("Usage: speed <multiplier>"));
    } else {
      float value;
      if (!parseFloat(tokens[1], value) || value <= 0.0f) {
        out.println(F("Speed must be a positive number"));
      } else {
        speedMultiplier = value;
        resetRuntimeState();
        out.print(F("Speed multiplier set to "));
        out.println(speedMultiplier, 3);
      }
    }
  } else if (command.equalsIgnoreCase(F("status"))) {
    printStatus(out);
  } else if (command.equalsIgnoreCase(F("reconnect"))) {
    out.println(F("Reconnecting WiFi..."));
#if WIFI_DEBUG_ENABLED
    telnetClient.stop();
    telnetServer.close();
    WiFi.disconnect(true, true);
    wifiConnected = false;
    wifiReady = false;
    otaReady = false;
    if (connectWiFi()) {
      configureOTA();
    }
#else
    out.println(F("WiFi is disabled in this build."));
#endif
  } else if (command.equalsIgnoreCase(F("help"))) {
    out.println(F("Commands:"));
    out.println(F("  effect <solid|rain|snake|breathing>"));
    out.println(F("  color <r> <g> <b>"));
    out.println(F("  brightness <0-255>"));
    out.println(F("  speed <multiplier>"));
    out.println(F("  reconnect"));
    out.println(F("  status"));
  } else {
    out.print(F("Unknown command: "));
    out.println(command);
  }
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      String line = serialBuffer;
      serialBuffer = "";
      line.trim();
      handleConsoleCommand(line, Serial);
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 120) {
        serialBuffer.remove(0, serialBuffer.length() - 120);
      }
    }
  }
}

void handleTelnet() {
#if WIFI_DEBUG_ENABLED
  if (!wifiReady || WiFi.status() != WL_CONNECTED) {
    if (telnetClient && telnetClient.connected()) {
      telnetClient.stop();
    }
    return;
  }
  if (!telnetClient || !telnetClient.connected()) {
    if (telnetServer.hasClient()) {
      WiFiClient newClient = telnetServer.available();
      if (telnetClient) {
        telnetClient.stop();
      }
      telnetClient = newClient;
      telnetClient.println(F("LED strip debug console"));
      telnetClient.println(F("Type 'help' for commands."));
    }
    return;
  }
  while (telnetClient.available() > 0) {
    char c = static_cast<char>(telnetClient.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      String line = telnetBuffer;
      telnetBuffer = "";
      line.trim();
      handleConsoleCommand(line, telnetClient);
    } else {
      telnetBuffer += c;
      if (telnetBuffer.length() > 120) {
        telnetBuffer.remove(0, telnetBuffer.length() - 120);
      }
    }
  }
#else
  // Telnet disabled when Wi-Fi is off.
#endif
}

void updateHeartbeat(uint32_t now) {
  if (now - lastHeartbeat < HEARTBEAT_INTERVAL_MS) {
    return;
  }
  lastHeartbeat = now;
  heartbeatState = !heartbeatState;
  digitalWrite(HEARTBEAT_PIN, heartbeatState ? HIGH : LOW);
}

// Wi-Fi and OTA helpers -----------------------------------------------------

void stopTelnet() {
#if WIFI_DEBUG_ENABLED
  if (telnetClient) {
    telnetClient.stop();
  }
  telnetServer.close();
#endif
}

void startTelnet() {
#if WIFI_DEBUG_ENABLED
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  broadcastRaw(F("Telnet console on port "));
  broadcastLine(String(TELNET_PORT));
#endif
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
#if WIFI_DEBUG_ENABLED
  Serial.print(F("[WiFi-event] "));
  Serial.println(static_cast<int>(event));
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    broadcastLine(F("STA start"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    broadcastLine(F("Connected to AP"));
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    broadcastRaw(F("Got IP: "));
    broadcastLine(WiFi.localIP().toString());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    broadcastRaw(F("Disconnected, reason: "));
    broadcastLine(String(info.wifi_sta_disconnected.reason));
    wifiReady = false;
    break;
  default:
    break;
  }
#else
  (void)event;
  (void)info;
#endif
}

bool connectWiFi() {
#if WIFI_DEBUG_ENABLED
  broadcastRaw(F("Connecting to WiFi SSID '"));
  broadcastRaw(String(WIFI_SSID));
  broadcastLine(F("'"));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    broadcastRaw(F("."));
  }
  broadcastLine("");
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    broadcastLine(F("WiFi connected."));
    broadcastRaw(F("IP address: "));
    broadcastLine(WiFi.localIP().toString());
    wifiReady = true;
    startTelnet();
    lastWifiAttemptMillis = millis();
    return true;
  }
  broadcastLine(F("Failed to connect to WiFi."));
  wifiReady = false;
  stopTelnet();
  WiFi.disconnect(true);
  lastWifiAttemptMillis = millis();
  return false;
#else
  broadcastLine(F("WiFi disabled in debug build. Using USB serial only."));
  wifiConnected = false;
  wifiReady = false;
  stopTelnet();
  return false;
#endif
}

void configureOTA() {
#if WIFI_DEBUG_ENABLED
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([] { broadcastLine(F("OTA update started")); });
  ArduinoOTA.onEnd([] { broadcastLine(F("OTA update completed")); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const unsigned int percent = (total == 0) ? 0U : (progress * 100U) / total;
    broadcastLine(String(F("OTA progress: ")) + percent + F("%"));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    broadcastRaw(F("OTA error: "));
    broadcastLine(String(static_cast<uint8_t>(error)));
  });
  ArduinoOTA.begin();
  broadcastRaw(F("OTA ready. Hostname: "));
  broadcastLine(OTA_HOSTNAME);
  otaReady = true;
#endif
}

void maintainWiFiAndOTA(uint32_t now) {
#if WIFI_DEBUG_ENABLED
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      broadcastLine(F("WiFi reconnected."));
      broadcastRaw(F("IP address: "));
      broadcastLine(WiFi.localIP().toString());
      startTelnet();
    }
    if (!otaReady) {
      configureOTA();
    }
  } else {
    if (wifiConnected || otaReady) {
      wifiConnected = false;
      wifiReady = false;
      otaReady = false;
      broadcastLine(F("WiFi connection lost. OTA paused."));
      stopTelnet();
    }
    if (now - lastWifiAttemptMillis >= WIFI_RETRY_INTERVAL_MS) {
      if (connectWiFi()) {
        configureOTA();
      }
    }
  }
  if (otaReady) {
    ArduinoOTA.handle();
  }
#else
  (void)now;
#endif
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if constexpr (WIFI_DEBUG_ENABLED) {
    WiFi.onEvent(onWiFiEvent);
  }
  pinMode(HEARTBEAT_PIN, OUTPUT);
  digitalWrite(HEARTBEAT_PIN, LOW);
  FastLED.addLeds<WS2811, DATA_PIN, DEBUG_COLOR_ORDER>(leds, TOTAL_LEDS);
  FastLED.setBrightness(globalBrightness);
  clearZones();
  flushZonesToPhysical();
  FastLED.show();
  lastFrameMillis = millis();
  if (WIFI_DEBUG_ENABLED) {
    connectWiFi();
    if (wifiConnected) {
      configureOTA();
    }
  } else {
    broadcastLine(F("WiFi disabled; running in USB-only mode."));
  }
  printStatus(Serial);
}

void loop() {
  handleSerialInput();
  handleTelnet();
  const uint32_t now = millis();
  static uint32_t accumulatedDelta = 0;
  const uint32_t delta = now - lastFrameMillis;
  lastFrameMillis = now;
  accumulatedDelta += delta;

  maintainWiFiAndOTA(now);

  if (accumulatedDelta >= FRAME_INTERVAL_MS) {
    const float deltaSeconds = static_cast<float>(accumulatedDelta) / 1000.0f;
    accumulatedDelta = 0;
    updateEffect(deltaSeconds);
    flushZonesToPhysical();
    FastLED.setBrightness(pendingBrightness);
    FastLED.show();
  }
  updateHeartbeat(now);
}
