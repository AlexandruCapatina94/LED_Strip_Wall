#include <Arduino.h>
#include <ArduinoOTA.h>
#include <FastLED.h>
#include <WiFi.h>
#include <stdlib.h>

#ifndef WIFI_MAIN_ENABLED
#define WIFI_MAIN_ENABLED 0
#endif

#if WIFI_MAIN_ENABLED
#include "wifi_credentials.h"

#ifndef WIFI_SSID
#error "Define WIFI_SSID in wifi_credentials.h"
#endif

#ifndef WIFI_PASSWORD
#error "Define WIFI_PASSWORD in wifi_credentials.h"
#endif
#endif

namespace {
constexpr uint8_t DATA_PIN = 2;
constexpr uint16_t LEDS_PER_ZONE = 1;
constexpr uint8_t NUM_STRIPS = 18;
constexpr uint8_t DEFAULT_BRIGHTNESS = 128;
constexpr float DEFAULT_SPEED = 3.0f;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr char OTA_HOSTNAME[] = "led-strip-wall";
constexpr uint32_t FRAME_INTERVAL_MS = 1000 / 120; // target ~120 FPS
constexpr bool COLOR_INPUT_IS_GRB = true;

struct StripDescriptor {
  uint16_t startZone;    // Zone index in data order
  uint16_t zoneCount;    // Number of controllable 14-LED groups
  bool reversed;         // True if physical orientation is reversed
};

constexpr StripDescriptor STRIPS[NUM_STRIPS] = {
    {0, 5, false},    // Strip 0 - 0.5 m
    {5, 5, true},     // Strip 1 - 0.5 m
    {10, 5, false},   // Strip 2 - 0.5 m
    {15, 5, true},    // Strip 3 - 0.5 m
    {20, 5, false},   // Strip 4 - 0.5 m
    {25, 5, true},    // Strip 5 - 0.5 m
    {30, 5, false},   // Strip 6 - 0.5 m
    {35, 5, true},    // Strip 7 - 0.5 m
    {40, 12, false},  // Strip 8 - 1.2 m
    {52, 12, true},   // Strip 9 - 1.2 m
    {64, 12, false},  // Strip 10 - 1.2 m
    {76, 15, true},   // Strip 11 - 1.5 m
    {91, 15, false},  // Strip 12 - 1.5 m
    {106, 15, true},  // Strip 13 - 1.5 m
    {121, 15, false}, // Strip 14 - 1.5 m
    {136, 15, true},  // Strip 15 - 1.5 m
    {151, 15, false}, // Strip 16 - 1.5 m
    {166, 15, true},  // Strip 17 - 1.5 m
};

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
  Rainbow,
  RainbowGlitter,
  Confetti,
  Sinelon,
  BPM,
  Juggle,
};

EffectType currentEffect = EffectType::Rainbow;
CRGB masterColor = CRGB::White;
uint8_t globalBrightness = DEFAULT_BRIGHTNESS;
float speedMultiplier = DEFAULT_SPEED;
uint8_t gHue = 0;
float hueAccumulator = 0.0f;
uint32_t lastFrameMillis = 0;
uint32_t frameAccumulator = 0;
String serialBuffer;
bool wifiConnected = false;
bool otaReady = false;
uint32_t lastWifiAttemptMillis = 0;

void configureOTA();
bool attemptWiFiConnection();
void maintainWiFiAndOTA(uint32_t now);

// Verbose Wi-Fi event logging for diagnostics
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.print(F("[WiFi-event] "));
  Serial.println(static_cast<int>(event));
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("STA start"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("STA connected to AP"));
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print(F("Got IP: "));
    Serial.println(WiFi.localIP());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.print(F("STA disconnected, reason: "));
    Serial.println(info.wifi_sta_disconnected.reason);
    break;
  default:
    break;
  }
}

uint16_t logicalToZoneIndex(uint8_t stripIndex, uint16_t logicalIndex) {
  const StripDescriptor &strip = STRIPS[stripIndex];
  if (logicalIndex >= strip.zoneCount) {
    return strip.startZone;
  }
  return strip.reversed ? (strip.startZone + strip.zoneCount - 1 - logicalIndex)
                        : (strip.startZone + logicalIndex);
}

void clearZones() {
  for (uint16_t i = 0; i < TOTAL_ZONES; ++i) {
    zoneBuffer[i] = CRGB::Black;
  }
}

void flushZonesToPhysical() {
  uint16_t logicalBase = 0;
  for (uint8_t stripIndex = 0; stripIndex < NUM_STRIPS; ++stripIndex) {
    const StripDescriptor &strip = STRIPS[stripIndex];
    for (uint16_t logical = 0; logical < strip.zoneCount; ++logical) {
      const uint16_t logicalIndex = logicalBase + logical;
      if (logicalIndex >= TOTAL_ZONES) {
        continue;
      }
      const uint16_t physicalZone = logicalToZoneIndex(stripIndex, logical);
      const CRGB color = zoneBuffer[logicalIndex];
      const uint32_t ledBase =
          static_cast<uint32_t>(physicalZone) * LEDS_PER_ZONE;
      for (uint8_t led = 0; led < LEDS_PER_ZONE; ++led) {
        leds[ledBase + led] = color;
      }
    }
    logicalBase += strip.zoneCount;
  }
}

void runSolid() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  fill_solid(zoneBuffer, TOTAL_ZONES, masterColor);
}

void addGlitter(fract8 chanceOfGlitter) {
  if (TOTAL_ZONES == 0) {
    return;
  }
  if (random8() < chanceOfGlitter) {
    const uint16_t index = random16(TOTAL_ZONES);
    zoneBuffer[index] += CRGB::White;
  }
}

void runRainbowCycle() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  fill_rainbow(zoneBuffer, TOTAL_ZONES, gHue, 7);
}

void runRainbowWithGlitter() {
  runRainbowCycle();
  addGlitter(80);
}

void runConfetti() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  fadeToBlackBy(zoneBuffer, TOTAL_ZONES, 10);
  const uint16_t pos = random16(TOTAL_ZONES);
  zoneBuffer[pos] += CHSV(gHue + random8(64), 200, 255);
}

uint8_t scaledBpm(uint8_t baseBpm) {
  const float scaled = baseBpm * speedMultiplier;
  if (scaled < 1.0f) {
    return 1;
  }
  if (scaled > 255.0f) {
    return 255;
  }
  return static_cast<uint8_t>(scaled);
}

void runSinelon() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  fadeToBlackBy(zoneBuffer, TOTAL_ZONES, 20);
  const uint16_t pos = beatsin16(scaledBpm(13), 0, TOTAL_ZONES - 1);
  zoneBuffer[pos] += CHSV(gHue, 255, 192);
}

void runBpm() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  const uint8_t beatsPerMinute = scaledBpm(62);
  const CRGBPalette16 palette = PartyColors_p;
  const uint8_t beat = beatsin8(beatsPerMinute, 64, 255);
  for (uint16_t i = 0; i < TOTAL_ZONES; ++i) {
    zoneBuffer[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

void runJuggle() {
  if (TOTAL_ZONES == 0) {
    return;
  }
  fadeToBlackBy(zoneBuffer, TOTAL_ZONES, 20);
  uint8_t dothue = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    const uint16_t pos = beatsin16(scaledBpm(i + 7), 0, TOTAL_ZONES - 1);
    zoneBuffer[pos] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

void advanceHue(float deltaSeconds) {
  constexpr float HUE_STEPS_PER_SECOND = 40.0f;
  float increment = speedMultiplier * deltaSeconds * HUE_STEPS_PER_SECOND;
  if (increment < 0.0f) {
    increment = 0.0f;
  }
  hueAccumulator += increment;
  while (hueAccumulator >= 1.0f) {
    gHue += 1;
    hueAccumulator -= 1.0f;
  }
}

void updateEffect(float deltaSeconds) {
  switch (currentEffect) {
  case EffectType::Solid:
    runSolid();
    break;
  case EffectType::Rainbow:
    runRainbowCycle();
    break;
  case EffectType::RainbowGlitter:
    runRainbowWithGlitter();
    break;
  case EffectType::Confetti:
    runConfetti();
    break;
  case EffectType::Sinelon:
    runSinelon();
    break;
  case EffectType::BPM:
    runBpm();
    break;
  case EffectType::Juggle:
    runJuggle();
    break;
  }
  advanceHue(deltaSeconds);
}

void resetEffectState() {
  gHue = 0;
  hueAccumulator = 0.0f;
  clearZones();
}

void printStatus() {
  Serial.println(F("--- LED Wall Status ---"));
  Serial.print(F("Effect: "));
  switch (currentEffect) {
  case EffectType::Solid:
    Serial.println(F("solid"));
    break;
  case EffectType::Rainbow:
    Serial.println(F("rainbow"));
    break;
  case EffectType::RainbowGlitter:
    Serial.println(F("rainbow_glitter"));
    break;
  case EffectType::Confetti:
    Serial.println(F("confetti"));
    break;
  case EffectType::Sinelon:
    Serial.println(F("sinelon"));
    break;
  case EffectType::BPM:
    Serial.println(F("bpm"));
    break;
  case EffectType::Juggle:
    Serial.println(F("juggle"));
    break;
  }
  Serial.print(F("Color (R,G,B): "));
  Serial.print(masterColor.r);
  Serial.print(F(","));
  Serial.print(masterColor.g);
  Serial.print(F(","));
  Serial.println(masterColor.b);
  Serial.print(F("Brightness: "));
  Serial.println(globalBrightness);
  Serial.print(F("Speed multiplier: "));
  Serial.println(speedMultiplier, 3);
  Serial.print(F("Total zones: "));
  Serial.println(TOTAL_ZONES);
  Serial.print(F("Total LEDs: "));
  Serial.println(TOTAL_LEDS);
  Serial.println();
}

void setEffectFromToken(const String &token) {
  if (token.equalsIgnoreCase(F("solid"))) {
    currentEffect = EffectType::Solid;
  } else if (token.equalsIgnoreCase(F("rainbow"))) {
    currentEffect = EffectType::Rainbow;
  } else if (token.equalsIgnoreCase(F("rainbow_glitter"))) {
    currentEffect = EffectType::RainbowGlitter;
  } else if (token.equalsIgnoreCase(F("confetti"))) {
    currentEffect = EffectType::Confetti;
  } else if (token.equalsIgnoreCase(F("sinelon"))) {
    currentEffect = EffectType::Sinelon;
  } else if (token.equalsIgnoreCase(F("bpm"))) {
    currentEffect = EffectType::BPM;
  } else if (token.equalsIgnoreCase(F("juggle"))) {
    currentEffect = EffectType::Juggle;
  } else {
    Serial.println(F("Unknown effect. Options: solid, rainbow, rainbow_glitter, confetti, sinelon, bpm, juggle"));
    return;
  }
  resetEffectState();
  Serial.print(F("Effect set to "));
  Serial.println(token);
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

void setColorFromTokens(const String tokens[], uint8_t count) {
  if (count < 3) {
    Serial.println(F("Usage: color <r> <g> <b>"));
    return;
  }
  uint8_t r, g, b;
  if (!parseUInt8(tokens[0], r) || !parseUInt8(tokens[1], g) || !parseUInt8(tokens[2], b)) {
    Serial.println(F("Color values must be 0-255"));
    return;
  }
  if constexpr (COLOR_INPUT_IS_GRB) {
    masterColor = CRGB(g, r, b);
  } else {
    masterColor = CRGB(r, g, b);
  }
  Serial.print(F("Color updated to "));
  Serial.print(r);
  Serial.print(F(","));
  Serial.print(g);
  Serial.print(F(","));
  Serial.println(b);
  if (currentEffect == EffectType::Solid) {
    runSolid();
    flushZonesToPhysical();
    FastLED.show();
  }
}

void handleCommand(const String &line) {
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
      Serial.println(F("Usage: effect <solid|rainbow|rainbow_glitter|confetti|sinelon|bpm|juggle>"));
    } else {
      setEffectFromToken(tokens[1]);
    }
  } else if (command.equalsIgnoreCase(F("color"))) {
    setColorFromTokens(&tokens[1], tokenCount - 1);
  } else if (command.equalsIgnoreCase(F("brightness"))) {
    if (tokenCount < 2) {
      Serial.println(F("Usage: brightness <0-255>"));
    } else {
      uint8_t value;
      if (parseUInt8(tokens[1], value)) {
        globalBrightness = value;
        FastLED.setBrightness(globalBrightness);
        Serial.print(F("Brightness set to "));
        Serial.println(globalBrightness);
      } else {
        Serial.println(F("Brightness must be 0-255"));
      }
    }
  } else if (command.equalsIgnoreCase(F("speed"))) {
    if (tokenCount < 2) {
      Serial.println(F("Usage: speed <multiplier>"));
    } else {
      float value;
      if (!parseFloat(tokens[1], value)) {
        Serial.println(F("Speed must be a number"));
      } else if (value <= 0.0f) {
        Serial.println(F("Speed must be positive"));
      } else {
        speedMultiplier = value;
        resetEffectState();
        Serial.print(F("Speed multiplier set to "));
        Serial.println(speedMultiplier, 3);
      }
    }
  } else if (command.equalsIgnoreCase(F("status"))) {
    printStatus();
  } else if (command.equalsIgnoreCase(F("help"))) {
    Serial.println(F("Commands:"));
    Serial.println(F("  effect <solid|rainbow|rainbow_glitter|confetti|sinelon|bpm|juggle>"));
    Serial.println(F("  color <r> <g> <b>"));
    Serial.println(F("  brightness <0-255>"));
    Serial.println(F("  speed <multiplier>"));
    Serial.println(F("  status"));
  } else {
    Serial.print(F("Unknown command: "));
    Serial.println(command);
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
      handleCommand(line);
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 80) {
        serialBuffer.remove(0, serialBuffer.length() - 80);
      }
    }
  }
}

bool attemptWiFiConnection() {
#if WIFI_MAIN_ENABLED
  Serial.print(F("Connecting to WiFi SSID '"));
  Serial.print(WIFI_SSID);
  Serial.println(F("'"));
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.println();
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    Serial.println(F("WiFi connected."));
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());
    lastWifiAttemptMillis = millis();
    return true;
  }
  Serial.println(F("Failed to connect to WiFi. OTA updates disabled until retry."));
  WiFi.disconnect(true);
  lastWifiAttemptMillis = millis();
  return false;
#else
  Serial.println(F("WiFi disabled in main build. OTA updates unavailable."));
  wifiConnected = false;
  otaReady = false;
  return false;
#endif
}

void configureOTA() {
#if WIFI_MAIN_ENABLED
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([]() { Serial.println(F("OTA update started.")); });
  ArduinoOTA.onEnd([]() { Serial.println(F("OTA update finished.")); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const unsigned int percent = (total == 0) ? 0U : (progress * 100U) / total;
    Serial.print(F("OTA progress: "));
    Serial.print(percent);
    Serial.println(F("%"));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.print(F("OTA error: "));
    Serial.println(static_cast<uint8_t>(error));
  });
  ArduinoOTA.begin();
  Serial.print(F("OTA ready. Hostname: "));
  Serial.println(OTA_HOSTNAME);
  otaReady = true;
#else
  otaReady = false;
#endif
}

void maintainWiFiAndOTA(uint32_t now) {
#if WIFI_MAIN_ENABLED
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println(F("WiFi reconnected."));
      Serial.print(F("IP address: "));
      Serial.println(WiFi.localIP());
    }
    if (!otaReady) {
      configureOTA();
    }
  } else {
    if (wifiConnected || otaReady) {
      wifiConnected = false;
      otaReady = false;
      Serial.println(F("WiFi connection lost. OTA paused."));
    }
    if (now - lastWifiAttemptMillis >= WIFI_RETRY_INTERVAL_MS) {
      if (attemptWiFiConnection()) {
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
  // Attach Wi-Fi event logger early
#if WIFI_MAIN_ENABLED
  WiFi.onEvent(onWiFiEvent);
#endif
  FastLED.addLeds<WS2811, DATA_PIN, GRB>(leds, TOTAL_LEDS);
  FastLED.setBrightness(globalBrightness);
  clearZones();
  flushZonesToPhysical();
  FastLED.show();
  printStatus();
  lastFrameMillis = millis();
#if WIFI_MAIN_ENABLED
  if (attemptWiFiConnection()) {
    configureOTA();
  }
#else
  Serial.println(F("WiFi disabled; OTA features are offline."));
#endif
}

void loop() {
  handleSerialInput();
  const uint32_t now = millis();
  const uint32_t delta = now - lastFrameMillis;
  lastFrameMillis = now;
  maintainWiFiAndOTA(now);
  frameAccumulator += delta;
  if (frameAccumulator >= FRAME_INTERVAL_MS) {
    const float deltaSeconds = static_cast<float>(frameAccumulator) / 1000.0f;
    frameAccumulator = 0;
    updateEffect(deltaSeconds);
    flushZonesToPhysical();
    FastLED.show();
  }
}
