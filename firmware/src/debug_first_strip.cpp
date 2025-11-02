#include <Arduino.h>
#include <FastLED.h>

namespace {

constexpr uint8_t DATA_PIN = 2;
constexpr uint8_t NUM_DEBUG_STRIPS = 9; // Mirror the first four production strips
constexpr uint8_t DEFAULT_BRIGHTNESS = 100;
constexpr uint16_t LEDS_PER_ZONE = 1;
constexpr uint32_t FRAME_DELAY_MS = 1000 / 120; // target ~120 FPS

struct StripDescriptor {
  uint16_t startZone;
  uint16_t zoneCount;
  bool reversed;
};

// Copy of the first four strip descriptors from the main firmware.
constexpr StripDescriptor STRIPS[NUM_DEBUG_STRIPS] = {
    {0, 5, false},
    {5, 5, true},
    {10, 5, false},
    {15, 5, true},
    {20, 5, false},
    {25, 5, true},
    {30, 5, false},
    {35, 5, true},
    {40, 12, false},
};

constexpr uint16_t TOTAL_ZONES = [] {
  uint16_t sum = 0;
  for (const auto &strip : STRIPS) {
    sum += strip.zoneCount;
  }
  return sum;
}();

constexpr uint16_t TOTAL_LEDS = TOTAL_ZONES * LEDS_PER_ZONE;

CRGB leds[TOTAL_LEDS];
uint8_t gCurrentPattern = 0;
uint8_t gHue = 0;

using PatternFunction = void (*)();

void addGlitter(fract8 chanceOfGlitter) {
  if (random8() < chanceOfGlitter) {
    leds[random16(TOTAL_LEDS)] += CRGB::White;
  }
}

void rainbow() {
  fill_rainbow(leds, TOTAL_LEDS, gHue, 7);
}

void rainbowWithGlitter() {
  rainbow();
  addGlitter(80);
}

void confetti() {
  fadeToBlackBy(leds, TOTAL_LEDS, 10);
  const uint16_t pos = random16(TOTAL_LEDS);
  leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

void sinelon() {
  fadeToBlackBy(leds, TOTAL_LEDS, 20);
  const uint16_t pos = beatsin16(13, 0, TOTAL_LEDS - 1);
  leds[pos] += CHSV(gHue, 255, 192);
}

void bpm() {
  const uint8_t beatsPerMinute = 62;
  const CRGBPalette16 palette = PartyColors_p;
  const uint8_t beat = beatsin8(beatsPerMinute, 64, 255);
  for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
    leds[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

void juggle() {
  fadeToBlackBy(leds, TOTAL_LEDS, 20);
  uint8_t dothue = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    const uint16_t pos = beatsin16(i + 7, 0, TOTAL_LEDS - 1);
    leds[pos] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

PatternFunction gPatterns[] = {
    rainbow,
    rainbowWithGlitter,
    confetti,
    sinelon,
    bpm,
    juggle,
};

constexpr uint8_t PATTERN_COUNT = sizeof(gPatterns) / sizeof(gPatterns[0]);

void nextPattern() {
  gCurrentPattern = (gCurrentPattern + 1) % PATTERN_COUNT;
}

void previousPattern() {
  gCurrentPattern = (gCurrentPattern == 0) ? (PATTERN_COUNT - 1) : (gCurrentPattern - 1);
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(tolower(Serial.read()));
    switch (c) {
    case 'n':
      nextPattern();
      Serial.println(F("Next pattern"));
      break;
    case 'p':
      previousPattern();
      Serial.println(F("Previous pattern"));
      break;
    case '+':
      FastLED.setBrightness(qadd8(FastLED.getBrightness(), 16));
      Serial.print(F("Brightness: "));
      Serial.println(FastLED.getBrightness());
      break;
    case '-':
      FastLED.setBrightness(qsub8(FastLED.getBrightness(), 16));
      Serial.print(F("Brightness: "));
      Serial.println(FastLED.getBrightness());
      break;
    case 'h':
    case '?':
      Serial.println(F("Commands: n=next, p=previous, +=brighter, -=dimmer"));
      break;
    default:
      Serial.println(F("Unknown command. Type 'h' for help."));
      break;
    }
  }
}

void printLayout() {
  Serial.println(F("--- Debug Layout (first four production strips) ---"));
  for (uint8_t i = 0; i < NUM_DEBUG_STRIPS; ++i) {
    const StripDescriptor &strip = STRIPS[i];
    Serial.print(F("Strip "));
    Serial.print(i);
    Serial.print(F(": zones "));
    Serial.print(strip.startZone);
    Serial.print(F(".."));
    Serial.print(strip.startZone + strip.zoneCount - 1);
    Serial.print(F(", reversed="));
    Serial.println(strip.reversed ? F("true") : F("false"));
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, TOTAL_LEDS);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  FastLED.clear();
  FastLED.show();
  printLayout();
  Serial.println(F("DemoReel controls: n=next, p=previous, +/- brightness, h=help"));
}

void loop() {
  handleSerial();
  gPatterns[gCurrentPattern]();
  FastLED.show();
  FastLED.delay(FRAME_DELAY_MS);

  EVERY_N_MILLISECONDS(50) { ++gHue; }
 // EVERY_N_SECONDS(10) { nextPattern(); }
}

