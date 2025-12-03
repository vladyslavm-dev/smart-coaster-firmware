#include "HX711.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoBLE.h>

// ──────────────────────────────────────────────────────────────────────────
// BLE SETUP
// Service + characteristics must match the Android app (BleUuids).
// TX: scale  -> phone (ASCII messages like "I 45.23 a")
// RX: phone  -> scale (single-byte commands, e.g. 1 = reminder LED flash)
// ──────────────────────────────────────────────────────────────────────────
#define SERVICE_UUID "6d12c00c-d907-4af8-b4d5-42680cdbbe04"
#define TX_CHAR_UUID "c663891c-6163-43cc-9ad6-0771785fde9d"  // Notify → phone
#define RX_CHAR_UUID "ab36ebe1-b1a5-4c46-b4e6-d54f3fb53247"  // Write → scale

BLEService scaleService(SERVICE_UUID);

// Up to ~30 chars, e.g. "I 123.45 a"
BLEStringCharacteristic txCharacteristic(
  TX_CHAR_UUID,
  BLERead | BLENotify,
  30
);

BLEByteCharacteristic rxCharacteristic(
  RX_CHAR_UUID,
  BLEWrite
);

// ──────────────────────────────────────────────────────────────────────────
// LED RING (NeoPixel)
// Visual feedback on drink / refill / reminder.
// ──────────────────────────────────────────────────────────────────────────
#define LED_PIN 3
#define NUM_LEDS 23
#define LED_BRIGHTNESS 15

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ──────────────────────────────────────────────────────────────────────────
// BLE STATE
// ──────────────────────────────────────────────────────────────────────────
bool bleConnected = false;
bool notificationSubscribed = false;  // true once the phone subscribes to TX

// ──────────────────────────────────────────────────────────────────────────
// RING BUFFER FOR UNSENT BLE MESSAGES
// If the phone is not connected/subscribed, we buffer events and flush later.
// ──────────────────────────────────────────────────────────────────────────
#define BUFFER_CAPACITY 30

String bufferData[BUFFER_CAPACITY];
int bufferHead = 0;
int bufferTail = 0;

// BLE & buffer helpers
void flushBufferedData();
void onTxSubscribe(BLEDevice central, BLECharacteristic characteristic);
void onTxUnsubscribe(BLEDevice central, BLECharacteristic characteristic);
void storeInBuffer(const char* data);

// LED helpers
void setAllLEDs(uint32_t color);
void playBlueAnimation();
void playGreenAnimation();
void setFirst3LEDsDimWhite();
void flashMultiColor5Times();

// ──────────────────────────────────────────────────────────────────────────
// WEIGHT MEASUREMENT: HX711 + THRESHOLDS
// ──────────────────────────────────────────────────────────────────────────
HX711 scale;

uint8_t dataPin  = 7;   // HX711 DOUT
uint8_t clockPin = 11;  // HX711 SCK

// Calibration from lab setup
float storedOffset      = 71306.00;
float storedScaleFactor = -1095.25;

// Thresholds & sensitivity (grams)
const float MIN_CUP_WEIGHT       = 200.0;  // minimal empty cup weight to accept tare
const float STABILITY_THRESHOLD  = 2.0;    // max delta between reads to call it "stable"
const int   STABLE_COUNT         = 3;      // how many stable reads in a row
const int   READING_AVERAGE      = 10;     // HX711 samples per reading
const float DRINK_SENSITIVITY    = 15.0;   // min drop => "drank"
const float REFILL_SENSITIVITY   = 30.0;   // min increase => "refill"
const float CUP_REMOVED_MARGIN   = 30.0;   // below (tare - margin) => cup removed

// Runtime state
float tareWeight             = 0.0;  // stable empty cup weight
float lastStableWaterWeight  = 0.0;  // last stable "water only" weight

// Weight helpers
float waitForStableReadingRaw();
float waitForStableReading();
void sendWaterEvent(char type, float amount, char cup);

// ──────────────────────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // ── NeoPixel init ──
  strip.begin();
  strip.show();
  strip.setBrightness(LED_BRIGHTNESS);
  setAllLEDs(strip.Color(0, 0, 255));  // blue on boot

  // ── BLE init ──
  if (!BLE.begin()) {
    Serial.println("BLE init failed!");
    while (true) {
      // hard fail – no point continuing without BLE
    }
  }

  BLE.setLocalName("Scale_Clinical_Test");
  BLE.setAdvertisedService(scaleService);

  scaleService.addCharacteristic(txCharacteristic);
  scaleService.addCharacteristic(rxCharacteristic);
  BLE.addService(scaleService);

  // Initial dummy payload
  txCharacteristic.writeValue("0.00 x");

  // Subscribe callbacks
  txCharacteristic.setEventHandler(BLESubscribed,   onTxSubscribe);
  txCharacteristic.setEventHandler(BLEUnsubscribed, onTxUnsubscribe);

  BLE.advertise();
  Serial.println("BLE ready → advertising");

  // ── HX711 init ──
  Serial.println("Using stored offset/scale factor...");
  scale.begin(dataPin, clockPin);
  scale.set_offset(storedOffset);
  scale.set_scale(storedScaleFactor);

  // Single tare with empty cup.
  // We keep calling BLE.poll() so the phone can still discover & connect.
  Serial.println("=== Place EMPTY cup, will tare once ===");
  while (true) {
    BLE.poll();
    delay(200);
    BLE.poll();

    tareWeight = waitForStableReadingRaw();

    if (tareWeight >= MIN_CUP_WEIGHT) {
      setFirst3LEDsDimWhite();  // idle: small white segment
      break;
    } else {
      setAllLEDs(strip.Color(255, 0, 0));  // red → cup too light
      Serial.print("ERROR: Cup weight too low (");
      Serial.print(tareWeight);
      Serial.println(" g). Use heavier cup or adjust MIN_CUP_WEIGHT.");
    }
  }

  Serial.print("Tared empty cup at (g): ");
  Serial.println(tareWeight);
  Serial.println("Setup complete. Measuring water weight in loop()…");
}

// ──────────────────────────────────────────────────────────────────────────
// MAIN LOOP
// ──────────────────────────────────────────────────────────────────────────
void loop() {
  // 1) Keep BLE alive
  BLE.poll();

  // 2) Track connection state
  bool isCurrentlyConnected = BLE.connected();
  if (isCurrentlyConnected && !bleConnected) {
    bleConnected = true;
    Serial.println("Phone connected.");
    flushBufferedData();
  } else if (!isCurrentlyConnected && bleConnected) {
    bleConnected = false;
    notificationSubscribed = false;
    Serial.println("Phone disconnected.");
    setAllLEDs(strip.Color(0, 0, 0));
  }

  // 3) Handle commands from Android app (RX char)
  if (rxCharacteristic.written()) {
    uint8_t cmd = rxCharacteristic.value();
    if (cmd == 1) {
      // Caregiver reminder: flash the ring to get patient attention
      flashMultiColor5Times();
    }
  }

  // 4) Serial test mode for lab debugging
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'i' || c == 'I') {
      // Simulate 100 g intake
      sendWaterEvent('I', 100.0, 'a');
      playGreenAnimation();
    } else if (c == 'r' || c == 'R') {
      // Simulate 50.12 g refill
      sendWaterEvent('R', 50.12, 'a');
      playBlueAnimation();
    }
  }

  // 5) Wait for stable reading; negative → cup removed.
  float stableReading = 0.0;
  bool cupRemoved = true;

  while (cupRemoved) {
    BLE.poll();

    stableReading = waitForStableReading();
    if (stableReading < 0) {
      setAllLEDs(strip.Color(255, 0, 0));  // red: no cup on coaster
      Serial.println("Cup removed or negative reading → Water Weight: (N/A)");
      delay(200);
      BLE.poll();
    } else {
      cupRemoved = false;
    }
  }

  // 6) Compare against last stable water weight to detect drink / refill
  float difference = stableReading - lastStableWaterWeight;

  if (fabs(difference) < DRINK_SENSITIVITY) {
    // No significant change → just idle LEDs
    setFirst3LEDsDimWhite();
  } else if (difference < -DRINK_SENSITIVITY) {
    // Patient drank
    float amountDrunk = -difference;
    sendWaterEvent('I', amountDrunk, 'a');  // "I X a"
    playGreenAnimation();
    Serial.print("Water drunk (g): ");
    Serial.println(amountDrunk);
    lastStableWaterWeight = stableReading;
  } else if (difference > REFILL_SENSITIVITY) {
    // Cup refilled
    float amountRefilled = difference;
    sendWaterEvent('R', amountRefilled, 'a');  // "R X a"
    playBlueAnimation();
    Serial.print("Refilled (g): ");
    Serial.println(amountRefilled);
    lastStableWaterWeight = stableReading;
  }

  // 7) If connected + subscribed, push any buffered messages out
  if (bleConnected && notificationSubscribed) {
    flushBufferedData();
  }

  delay(200);
}

// ──────────────────────────────────────────────────────────────────────────
// WEIGHT READING HELPERS
// ──────────────────────────────────────────────────────────────────────────

// A) Raw HX711 reading (no tare), waits until the value is stable.
float waitForStableReadingRaw() {
  float previous       = 0.0;
  float current        = 0.0;
  int   stableCounter  = 0;

  while (true) {
    BLE.poll();

    current = scale.get_units(READING_AVERAGE);  // average of N reads
    float diff = fabs(current - previous);

    if (diff < STABILITY_THRESHOLD) {
      stableCounter++;
    } else {
      stableCounter = 0;
    }

    previous = current;

    if (stableCounter >= STABLE_COUNT) {
      return current;
    }

    delay(200);
    BLE.poll();
  }
}

// B) Tared reading → returns "water only" weight, or -1 if cup removed.
float waitForStableReading() {
  float stableReadingRaw = waitForStableReadingRaw();
  float threshold        = tareWeight - CUP_REMOVED_MARGIN;
  bool  removed          = (stableReadingRaw < threshold);

  if (removed) {
    return -1.0;  // cup not on coaster
  }

  float waterWeight = stableReadingRaw - tareWeight;
  if (waterWeight < 0) {
    return 0.0;
  }
  return waterWeight;
}

// ──────────────────────────────────────────────────────────────────────────
// BLE EVENT SENDING
// ──────────────────────────────────────────────────────────────────────────

// Formats and sends one water event (e.g. "I 45.23 a").
// Buffers if BLE is not ready.
void sendWaterEvent(char type, float amount, char cup) {
  char buf[30];
  snprintf(buf, sizeof(buf), "%c %.2f %c", type, amount, cup);

  if (!bleConnected || !notificationSubscribed) {
    storeInBuffer(buf);
    Serial.print("Not connected/subscribed → buffered: ");
    Serial.println(buf);
  } else {
    bool ok = txCharacteristic.writeValue(buf);
    if (!ok) {
      Serial.println("Notify failed → buffering");
      storeInBuffer(buf);
    } else {
      Serial.print("Notified: ");
      Serial.println(buf);
    }
  }
}

// ──────────────────────────────────────────────────────────────────────────
// RING BUFFER
// ──────────────────────────────────────────────────────────────────────────
void storeInBuffer(const char* data) {
  int nextHead = (bufferHead + 1) % BUFFER_CAPACITY;

  // Buffer full → drop oldest entry
  if (nextHead == bufferTail) {
    bufferTail = (bufferTail + 1) % BUFFER_CAPACITY;
  }

  bufferData[bufferHead] = String(data);
  bufferHead = nextHead;
}

// Flush all buffered messages, spacing them out so Android sees clear events.
void flushBufferedData() {
  if (!bleConnected || !notificationSubscribed) return;

  while (bufferTail != bufferHead) {
    String& msg = bufferData[bufferTail];
    bool ok = txCharacteristic.writeValue(msg.c_str());

    if (ok) {
      Serial.print("Flushed from buffer: ");
      Serial.println(msg);
      bufferTail = (bufferTail + 1) % BUFFER_CAPACITY;
      delay(1000);  // space out notifications
    } else {
      Serial.println("Flush failed → stop");
      break;
    }
  }
}

// ──────────────────────────────────────────────────────────────────────────
// BLE SUBSCRIBE / UNSUBSCRIBE
// ──────────────────────────────────────────────────────────────────────────
void onTxSubscribe(BLEDevice central, BLECharacteristic characteristic) {
  notificationSubscribed = true;
  Serial.println("Central subscribed to TX notifications.");
  flushBufferedData();
}

void onTxUnsubscribe(BLEDevice central, BLECharacteristic characteristic) {
  notificationSubscribed = false;
  Serial.println("Central unsubscribed from TX notifications.");
}

// ──────────────────────────────────────────────────────────────────────────
// LED HELPERS
// ──────────────────────────────────────────────────────────────────────────
void setAllLEDs(uint32_t color) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

// Blue “wave” when refill event is detected.
void playBlueAnimation() {
  strip.setBrightness(20);
  for (int i = 0; i < NUM_LEDS; i++) {
    BLE.poll();
    strip.setPixelColor(i, strip.Color(0, 0, 255));
    if (i > 0) {
      strip.setPixelColor(i - 1, strip.Color(0, 0, 80));
    }
    strip.show();
    delay(100);
    BLE.poll();
  }
  setFirst3LEDsDimWhite();
}

// Green “wave” when drink event is detected.
void playGreenAnimation() {
  strip.setBrightness(20);
  for (int i = 0; i < NUM_LEDS; i++) {
    BLE.poll();
    strip.setPixelColor(i, strip.Color(0, 255, 0));
    if (i > 0) {
      strip.setPixelColor(i - 1, strip.Color(0, 100, 0));
    }
    strip.show();
    delay(100);
    BLE.poll();
  }
  setFirst3LEDsDimWhite();
}

// Idle state: only a small white segment lit near the “front”.
void setFirst3LEDsDimWhite() {
  strip.setBrightness(2);

  for (int i = 0; i < NUM_LEDS; i++) {
    BLE.poll();
    strip.setPixelColor(i, 0);
  }
  for (int i = 2; i < 5 && i < NUM_LEDS; i++) {
    BLE.poll();
    strip.setPixelColor(i, strip.Color(255, 255, 255));
  }
  strip.show();
}

// Multi-color flash used for “please drink” reminders.
void flashMultiColor5Times() {
  strip.setBrightness(50);

  for (int cycle = 0; cycle < 5; cycle++) {
    // Phase 1: assign pseudo-random colors per LED
    for (int i = 0; i < strip.numPixels(); i++) {
      BLE.poll();
      uint8_t r = (i * 10) % 256;
      uint8_t g = (i * 20) % 256;
      uint8_t b = (i * 30) % 256;
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
    strip.show();

    unsigned long start = millis();
    while (millis() - start < 400) {
      BLE.poll();
      delay(10);
    }

    // Phase 2: all off
    for (int i = 0; i < strip.numPixels(); i++) {
      BLE.poll();
      strip.setPixelColor(i, 0);
    }
    strip.show();

    start = millis();
    while (millis() - start < 400) {
      BLE.poll();
      delay(10);
    }
  }

  // Back to idle
  setFirst3LEDsDimWhite();
}
