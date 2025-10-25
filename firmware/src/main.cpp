#include <Arduino.h>
#include <FastLED.h>
#include <stdlib.h>

namespace {
constexpr uint8_t DATA_PIN = 2;
constexpr uint16_t LEDS_PER_ZONE = 14;
constexpr uint8_t NUM_STRIPS = 18;
constexpr uint8_t DEFAULT_BRIGHTNESS = 128;
constexpr float DEFAULT_SPEED = 1.0f;
constexpr uint8_t RAIN_TRAIL = 6;
constexpr uint8_t RAIN_FADE = 48;
constexpr uint8_t SNAKE_LENGTH = 12;
constexpr uint8_t SNAKE_FADE = 32;

struct StripDescriptor {
  uint16_t startZone;    // Zone index in data order
  uint16_t zoneCount;    // Number of controllable 14-LED groups
  bool reversed;         // True if physical orientation is reversed
};

constexpr StripDescriptor STRIPS[NUM_STRIPS] = {
    {0, 15, false},   // Strip 0 - 0.5 m
    {15, 15, true},   // Strip 1 - 0.5 m
    {30, 15, false},  // Strip 2 - 0.5 m
    {45, 15, true},   // Strip 3 - 0.5 m
    {60, 15, false},  // Strip 4 - 0.5 m
    {75, 15, true},   // Strip 5 - 0.5 m
    {90, 15, false},  // Strip 6 - 0.5 m
    {105, 15, true},  // Strip 7 - 0.5 m
    {120, 36, false}, // Strip 8 - 1.2 m
    {156, 36, true},  // Strip 9 - 1.2 m
    {192, 36, false}, // Strip 10 - 1.2 m
    {228, 45, true},  // Strip 11 - 1.5 m
    {273, 45, false}, // Strip 12 - 1.5 m
    {318, 45, true},  // Strip 13 - 1.5 m
    {363, 45, false}, // Strip 14 - 1.5 m
    {408, 45, true},  // Strip 15 - 1.5 m
    {453, 45, false}, // Strip 16 - 1.5 m
    {498, 45, true},  // Strip 17 - 1.5 m
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
  Rain,
  Snake,
};

struct StripRuntime {
  float accumulator = 0.0f;
};

EffectType currentEffect = EffectType::Solid;
CRGB masterColor = CRGB::White;
uint8_t globalBrightness = DEFAULT_BRIGHTNESS;
float speedMultiplier = DEFAULT_SPEED;
StripRuntime stripState[NUM_STRIPS];
uint32_t lastFrameMillis = 0;
String serialBuffer;

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

void updateEffect(float deltaSeconds) {
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
  }
}

void resetRuntimeState() {
  for (auto &state : stripState) {
    state.accumulator = 0.0f;
  }
}

void printStatus() {
  Serial.println(F("--- LED Wall Status ---"));
  Serial.print(F("Effect: "));
  switch (currentEffect) {
  case EffectType::Solid:
    Serial.println(F("solid"));
    break;
  case EffectType::Rain:
    Serial.println(F("rain"));
    break;
  case EffectType::Snake:
    Serial.println(F("snake"));
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
  } else if (token.equalsIgnoreCase(F("rain"))) {
    currentEffect = EffectType::Rain;
  } else if (token.equalsIgnoreCase(F("snake"))) {
    currentEffect = EffectType::Snake;
  } else {
    Serial.println(F("Unknown effect. Options: solid, rain, snake"));
    return;
  }
  resetRuntimeState();
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
  masterColor = CRGB(r, g, b);
  Serial.print(F("Color updated to "));
  Serial.print(r);
  Serial.print(F(","));
  Serial.print(g);
  Serial.print(F(","));
  Serial.println(b);
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
      Serial.println(F("Usage: effect <solid|rain|snake>"));
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
        resetRuntimeState();
        Serial.print(F("Speed multiplier set to "));
        Serial.println(speedMultiplier, 3);
      }
    }
  } else if (command.equalsIgnoreCase(F("status"))) {
    printStatus();
  } else if (command.equalsIgnoreCase(F("help"))) {
    Serial.println(F("Commands:"));
    Serial.println(F("  effect <solid|rain|snake>"));
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

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  FastLED.addLeds<WS2811, DATA_PIN, GRB>(leds, TOTAL_LEDS);
  FastLED.setBrightness(globalBrightness);
  clearZones();
  flushZonesToPhysical();
  FastLED.show();
  printStatus();
  lastFrameMillis = millis();
}

void loop() {
  handleSerialInput();
  const uint32_t now = millis();
  const uint32_t delta = now - lastFrameMillis;
  lastFrameMillis = now;
  const float deltaSeconds = static_cast<float>(delta) / 1000.0f;
  updateEffect(deltaSeconds);
  flushZonesToPhysical();
  FastLED.show();
}
