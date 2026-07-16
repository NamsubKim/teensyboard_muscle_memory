#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Encoder.h>
#include "USBHost_t36.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_ADDR 0x3C

const int AX_BUTTON_PIN = 22;
const int AY_BUTTON_PIN = 6;
const int AX_ENCODER_A_PIN = 3;
const int AX_ENCODER_B_PIN = 5;
const int AY_ENCODER_A_PIN = 8;
const int AY_ENCODER_B_PIN = 10;
const int RED_BUTTON_LEFT_PIN = 15;
const int RED_BUTTON_RIGHT_PIN = 17;

const float DEFAULT_KNOB_SCALE_MIN = 0.25f;
const float DEFAULT_KNOB_SCALE_MAX = 4.00f;
const int COARSE_CPI_PER_STEP = 40;
const int FINE_CPI_PER_STEP = 4;
const int ENCODER_COUNTS_PER_STEP = 4;
const int COARSE_ENCODER_SIGN = 1;
const int FINE_ENCODER_SIGN = 1;

const unsigned long DEBOUNCE_MS = 40;
const int SERIAL_LINE_MAX = 192;
#define SERIAL_DEBUG_RED_BUTTON_RAW 1
#define SERIAL_MOUSE_TELEMETRY 0
#define SERIAL_MOUSE_BINARY_TELEMETRY 1
#define SERIAL_MOUSE_IDLE_TELEMETRY 1
const unsigned long MOUSE_TELEMETRY_INTERVAL_US = 2000;
const int MOUSE_TELEMETRY_MIN_WRITE_SPACE = 64;
const uint8_t BINARY_MAGIC_0 = 0xA5;
const uint8_t BINARY_MAGIC_1 = 0x5A;
const uint8_t BINARY_PROTOCOL_VERSION = 2;
const uint8_t BINARY_PACKET_MOUSE = 1;
const uint8_t BINARY_MOUSE_PAYLOAD_LEN = 80;

const uint16_t BOARD_BUTTON_LEFT = 0x0001;
const uint16_t BOARD_BUTTON_RIGHT = 0x0002;
const uint16_t BOUNDARY_MIN = 0x0001;
const uint16_t BOUNDARY_MAX = 0x0002;

USBHost myusb;
USBHub hub1(myusb);
USBHub hub2(myusb);

USBHIDParser hid1(myusb);
USBHIDParser hid2(myusb);
USBHIDParser hid3(myusb);
USBHIDParser hid4(myusb);
USBHIDParser hid5(myusb);

MouseController mouse1(myusb);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Encoder coarseEncoder(AY_ENCODER_A_PIN, AY_ENCODER_B_PIN);
Encoder fineEncoder(AX_ENCODER_A_PIN, AX_ENCODER_B_PIN);

uint16_t trialIndex = 0;
uint16_t baselineCpi = 800;
uint16_t mouseInputCpi = 800;
uint16_t randomizedCpi = 800;
float startScale = 1.0f;
float knobScale = 1.0f;
float knobScaleMin = DEFAULT_KNOB_SCALE_MIN;
float knobScaleMax = DEFAULT_KNOB_SCALE_MAX;
uint16_t effectiveCpiMinLimit = 100;
uint16_t effectiveCpiMaxLimit = 6400;
bool displayBlindMode = false;
uint32_t lastAppliedCommandId = 0;

float rem_x = 0.0f;
float rem_y = 0.0f;
long lastCoarseEncoderPos = 0;
long lastFineEncoderPos = 0;
int16_t coarseSteps = 0;
int16_t fineSteps = 0;
int16_t knobCpiOffset = 0;
uint32_t telemetryCounter = 0;
uint16_t boardButtons = 0;
uint16_t boundaryFlags = 0;

long pendingRawDx = 0;
long pendingRawDy = 0;
long pendingOutDx = 0;
long pendingOutDy = 0;
int pendingWheel = 0;
int pendingWheelH = 0;
uint8_t pendingMouseButtons = 0;
uint8_t lastMouseButtons = 0;
uint16_t pendingMouseReports = 0;
unsigned long lastMouseTelemetryUs = 0;

bool lastRedLeftReading = HIGH;
bool lastRedRightReading = HIGH;
bool stableRedLeftState = HIGH;
bool stableRedRightState = HIGH;
unsigned long lastRedLeftChangeMs = 0;
unsigned long lastRedRightChangeMs = 0;

char serialLine[SERIAL_LINE_MAX];
int serialLineLen = 0;

long effectiveCpiForSteps(int16_t coarse, int16_t fine) {
  return (long)randomizedCpi + (long)coarse * COARSE_CPI_PER_STEP + (long)fine * FINE_CPI_PER_STEP;
}

long minEffectiveCpi() {
  return (long)effectiveCpiMinLimit;
}

long maxEffectiveCpi() {
  return (long)effectiveCpiMaxLimit;
}

long clampEffectiveCpi(long value) {
  long minValue = minEffectiveCpi();
  long maxValue = maxEffectiveCpi();
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

long currentEffectiveCpiValue() {
  return clampEffectiveCpi(effectiveCpiForSteps(coarseSteps, fineSteps));
}

void refreshKnobState() {
  long rawValue = effectiveCpiForSteps(coarseSteps, fineSteps);
  long value = clampEffectiveCpi(rawValue);
  long minValue = minEffectiveCpi();
  long maxValue = maxEffectiveCpi();

  boundaryFlags = 0;
  if (rawValue <= minValue) boundaryFlags |= BOUNDARY_MIN;
  if (rawValue >= maxValue) boundaryFlags |= BOUNDARY_MAX;

  knobCpiOffset = (int16_t)(value - (long)randomizedCpi);
  if (randomizedCpi > 0) {
    knobScale = (float)value / (float)randomizedCpi;
  } else {
    knobScale = 1.0f;
  }
}

float effectiveScale() {
  if (mouseInputCpi == 0) return 1.0f;
  return (float)currentEffectiveCpiValue() / (float)mouseInputCpi;
}

float baselineEffectiveScale() {
  if (baselineCpi == 0) return 1.0f;
  return (float)currentEffectiveCpiValue() / (float)baselineCpi;
}

uint16_t effectiveCpi() {
  long value = currentEffectiveCpiValue();
  if (value < 0) value = 0;
  if (value > 65535L) value = 65535L;
  return (uint16_t)value;
}

int consumeWholePixels(float &remainder, float scaledDelta) {
  float total = scaledDelta + remainder;
  int whole = 0;

  if (total >= 1.0f) {
    whole = (int)floor(total);
  } else if (total <= -1.0f) {
    whole = (int)ceil(total);
  }

  remainder = total - whole;

  if (remainder > -0.000001f && remainder < 0.000001f) {
    remainder = 0.0f;
  }

  return whole;
}

void clearMouseTelemetry() {
  pendingRawDx = 0;
  pendingRawDy = 0;
  pendingOutDx = 0;
  pendingOutDy = 0;
  pendingWheel = 0;
  pendingWheelH = 0;
  pendingMouseButtons = 0;
  pendingMouseReports = 0;
}

void resetTrialState() {
  knobScale = 1.0f;
  coarseSteps = 0;
  fineSteps = 0;
  knobCpiOffset = 0;
  boundaryFlags = 0;
  coarseEncoder.write(0);
  fineEncoder.write(0);
  lastCoarseEncoderPos = 0;
  lastFineEncoderPos = 0;
  refreshKnobState();
}

void drawDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (displayBlindMode) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Trial");

    display.setTextSize(3);
    display.setCursor(0, 10);
    display.print(trialIndex);

    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Trial ");
  display.print(trialIndex);

  display.setCursor(64, 0);
  display.print("C/F ");
  display.print(coarseSteps);
  display.print("/");
  display.print(fineSteps);

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(effectiveCpi());

  display.display();
}

void printReady() {
  Serial.print("READY firmware=teensy_serial_cpi protocol=");
  Serial.print(BINARY_PROTOCOL_VERSION);
  Serial.print(" binary_mouse_payload_len=");
  Serial.print(BINARY_MOUSE_PAYLOAD_LEN);
  Serial.print(" left_pin=");
  Serial.print(RED_BUTTON_LEFT_PIN);
  Serial.print(" right_pin=");
  Serial.println(RED_BUTTON_RIGHT_PIN);
}

void printState(const char *reason) {
  Serial.print("STATE reason=");
  Serial.print(reason);
  Serial.print(" trial=");
  Serial.print(trialIndex);
  Serial.print(" baseline_cpi=");
  Serial.print(baselineCpi);
  Serial.print(" mouse_input_cpi=");
  Serial.print(mouseInputCpi);
  Serial.print(" randomized_cpi=");
  Serial.print(randomizedCpi);
  Serial.print(" start_scale=");
  Serial.print(startScale, 6);
  Serial.print(" knob_scale=");
  Serial.print(knobScale, 6);
  Serial.print(" effective_scale=");
  Serial.print(effectiveScale(), 6);
  Serial.print(" baseline_effective_scale=");
  Serial.print(baselineEffectiveScale(), 6);
  Serial.print(" effective_cpi=");
  Serial.print(effectiveCpi());
  Serial.print(" effective_cpi_min=");
  Serial.print(effectiveCpiMinLimit);
  Serial.print(" effective_cpi_max=");
  Serial.print(effectiveCpiMaxLimit);
  Serial.print(" knob_cpi_offset=");
  Serial.print(knobCpiOffset);
  Serial.print(" coarse_steps=");
  Serial.print(coarseSteps);
  Serial.print(" fine_steps=");
  Serial.print(fineSteps);
  Serial.print(" coarse_raw=");
  Serial.print(lastCoarseEncoderPos);
  Serial.print(" fine_raw=");
  Serial.print(lastFineEncoderPos);
  Serial.print(" board_buttons=");
  Serial.print(boardButtons);
  Serial.print(" boundary_flags=");
  Serial.print(boundaryFlags);
  Serial.print(" display_blind=");
  Serial.print(displayBlindMode ? 1 : 0);
  Serial.print(" command_id=");
  Serial.print(lastAppliedCommandId);
  Serial.print(" device_timestamp_us=");
  Serial.println(micros());
}

void printButtonEvent(const char *name, int pin, bool pressed) {
  Serial.print("EVENT name=");
  Serial.print(name);
  Serial.print(" pin=");
  Serial.print(pin);
  Serial.print(" state=");
  Serial.print(pressed ? "PRESSED" : "RELEASED");
  Serial.print(" trial=");
  Serial.print(trialIndex);
  Serial.print(" board_buttons=");
  Serial.print(boardButtons);
  Serial.print(" device_timestamp_us=");
  Serial.print(micros());
  Serial.print(" baseline_cpi=");
  Serial.print(baselineCpi);
  Serial.print(" mouse_input_cpi=");
  Serial.print(mouseInputCpi);
  Serial.print(" randomized_cpi=");
  Serial.print(randomizedCpi);
  Serial.print(" effective_cpi=");
  Serial.print(effectiveCpi());
  Serial.print(" knob_scale=");
  Serial.print(knobScale, 6);
  Serial.print(" effective_scale=");
  Serial.print(effectiveScale(), 6);
  Serial.print(" coarse_steps=");
  Serial.print(coarseSteps);
  Serial.print(" fine_steps=");
  Serial.print(fineSteps);
  Serial.print(" knob_cpi_offset=");
  Serial.print(knobCpiOffset);
  Serial.print(" boundary_flags=");
  Serial.print(boundaryFlags);
  Serial.print(" display_blind=");
  Serial.print(displayBlindMode ? 1 : 0);
  Serial.print(" command_id=");
  Serial.println(lastAppliedCommandId);
}

void printButtonRawChange(const char *name, int pin, bool reading) {
#if SERIAL_DEBUG_RED_BUTTON_RAW
  Serial.print("BUTTON_RAW name=");
  Serial.print(name);
  Serial.print(" pin=");
  Serial.print(pin);
  Serial.print(" raw=");
  Serial.print(reading == LOW ? "LOW" : "HIGH");
  Serial.print(" pressed=");
  Serial.println(reading == LOW ? 1 : 0);
#endif
}

void printButtonDiagnostics(const char *reason) {
  Serial.print("BUTTONS reason=");
  Serial.print(reason);
  Serial.print(" left_pin=");
  Serial.print(RED_BUTTON_LEFT_PIN);
  Serial.print(" left_raw=");
  Serial.print(digitalRead(RED_BUTTON_LEFT_PIN) == LOW ? "LOW" : "HIGH");
  Serial.print(" left_stable=");
  Serial.print(stableRedLeftState == LOW ? "LOW" : "HIGH");
  Serial.print(" right_pin=");
  Serial.print(RED_BUTTON_RIGHT_PIN);
  Serial.print(" right_raw=");
  Serial.print(digitalRead(RED_BUTTON_RIGHT_PIN) == LOW ? "LOW" : "HIGH");
  Serial.print(" right_stable=");
  Serial.print(stableRedRightState == LOW ? "LOW" : "HIGH");
  Serial.print(" board_buttons=");
  Serial.println(boardButtons);
}

void printMouseTelemetry(long dx, long dy, long out_x, long out_y,
                         int wheel, int wheelH, uint8_t mouseButtons,
                         uint16_t mouseReports) {
  Serial.print("MOUSE trial=");
  Serial.print(trialIndex);
  Serial.print(" counter=");
  Serial.print(telemetryCounter++);
  Serial.print(" mouse_reports=");
  Serial.print(mouseReports);
  Serial.print(" raw_dx=");
  Serial.print(dx);
  Serial.print(" raw_dy=");
  Serial.print(dy);
  Serial.print(" out_dx=");
  Serial.print(out_x);
  Serial.print(" out_dy=");
  Serial.print(out_y);
  Serial.print(" start_scale=");
  Serial.print(startScale, 6);
  Serial.print(" knob_scale=");
  Serial.print(knobScale, 6);
  Serial.print(" effective_scale=");
  Serial.print(effectiveScale(), 6);
  Serial.print(" baseline_effective_scale=");
  Serial.print(baselineEffectiveScale(), 6);
  Serial.print(" baseline_cpi=");
  Serial.print(baselineCpi);
  Serial.print(" mouse_input_cpi=");
  Serial.print(mouseInputCpi);
  Serial.print(" randomized_cpi=");
  Serial.print(randomizedCpi);
  Serial.print(" effective_cpi=");
  Serial.print(effectiveCpi());
  Serial.print(" mouse_buttons=");
  Serial.print(mouseButtons);
  Serial.print(" board_buttons=");
  Serial.print(boardButtons);
  Serial.print(" wheel=");
  Serial.print(wheel);
  Serial.print(" wheel_h=");
  Serial.print(wheelH);
  Serial.print(" knob_cpi_offset=");
  Serial.print(knobCpiOffset);
  Serial.print(" coarse_steps=");
  Serial.print(coarseSteps);
  Serial.print(" fine_steps=");
  Serial.print(fineSteps);
  Serial.print(" coarse_raw=");
  Serial.print(lastCoarseEncoderPos);
  Serial.print(" fine_raw=");
  Serial.print(lastFineEncoderPos);
  Serial.print(" boundary_flags=");
  Serial.println(boundaryFlags);
}

int16_t scaleToQ1000(float value) {
  float scaled = value * 1000.0f;
  if (scaled > 32767.0f) scaled = 32767.0f;
  if (scaled < -32768.0f) scaled = -32768.0f;
  return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

int32_t scaleToQ1000000(float value) {
  double scaled = (double)value * 1000000.0;
  if (scaled > 2147483647.0) scaled = 2147483647.0;
  if (scaled < -2147483648.0) scaled = -2147483648.0;
  return (int32_t)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

void putU16(uint8_t *packet, int &index, uint16_t value) {
  packet[index++] = (uint8_t)(value & 0xFF);
  packet[index++] = (uint8_t)((value >> 8) & 0xFF);
}

void putI16(uint8_t *packet, int &index, int16_t value) {
  putU16(packet, index, (uint16_t)value);
}

void putU32(uint8_t *packet, int &index, uint32_t value) {
  packet[index++] = (uint8_t)(value & 0xFF);
  packet[index++] = (uint8_t)((value >> 8) & 0xFF);
  packet[index++] = (uint8_t)((value >> 16) & 0xFF);
  packet[index++] = (uint8_t)((value >> 24) & 0xFF);
}

void putI32(uint8_t *packet, int &index, int32_t value) {
  putU32(packet, index, (uint32_t)value);
}

uint8_t binaryChecksum(const uint8_t *packet, int lengthWithoutChecksum) {
  uint8_t sum = 0;
  for (int i = 2; i < lengthWithoutChecksum; i++) {
    sum = (uint8_t)(sum + packet[i]);
  }
  return sum;
}

void sendBinaryMouseTelemetry(long dx, long dy, long out_x, long out_y,
                              int wheel, int wheelH, uint8_t mouseButtons,
                              uint16_t mouseReports) {
  const int packetLen = 2 + 3 + BINARY_MOUSE_PAYLOAD_LEN + 1;
  uint8_t packet[packetLen];
  int index = 0;

  packet[index++] = BINARY_MAGIC_0;
  packet[index++] = BINARY_MAGIC_1;
  packet[index++] = BINARY_PROTOCOL_VERSION;
  packet[index++] = BINARY_PACKET_MOUSE;
  packet[index++] = BINARY_MOUSE_PAYLOAD_LEN;

  uint32_t nowUs = micros();
  putU32(packet, index, telemetryCounter++);
  putU32(packet, index, nowUs / 1000UL);
  putU32(packet, index, nowUs);
  putU16(packet, index, trialIndex);
  putU16(packet, index, mouseReports);
  putI32(packet, index, (int32_t)dx);
  putI32(packet, index, (int32_t)dy);
  putI32(packet, index, (int32_t)out_x);
  putI32(packet, index, (int32_t)out_y);
  putI16(packet, index, (int16_t)wheel);
  putI16(packet, index, (int16_t)wheelH);
  putU16(packet, index, mouseButtons);
  putU16(packet, index, boardButtons);
  putI16(packet, index, scaleToQ1000(startScale));
  putI16(packet, index, scaleToQ1000(knobScale));
  putI16(packet, index, scaleToQ1000(effectiveScale()));
  putU16(packet, index, baselineCpi);
  putU16(packet, index, mouseInputCpi);
  putU16(packet, index, randomizedCpi);
  putU16(packet, index, effectiveCpi());
  putI16(packet, index, coarseSteps);
  putI16(packet, index, fineSteps);
  putI16(packet, index, knobCpiOffset);
  putI32(packet, index, (int32_t)lastCoarseEncoderPos);
  putI32(packet, index, (int32_t)lastFineEncoderPos);
  putU16(packet, index, boundaryFlags);
  putU16(packet, index, displayBlindMode ? 1 : 0);
  putI32(packet, index, scaleToQ1000000(rem_x));
  putI32(packet, index, scaleToQ1000000(rem_y));

  packet[index] = binaryChecksum(packet, index);
  index++;
  Serial.write(packet, index);
}

void flushMouseTelemetry(bool force) {
#if SERIAL_MOUSE_TELEMETRY || SERIAL_MOUSE_BINARY_TELEMETRY
  bool hasMouseReports = pendingMouseReports > 0;
#if SERIAL_MOUSE_IDLE_TELEMETRY
  bool shouldSendIdleTelemetry = trialIndex > 0;
#else
  bool shouldSendIdleTelemetry = false;
#endif
  if (!hasMouseReports && !shouldSendIdleTelemetry) {
    return;
  }

  unsigned long now = micros();
  if (!force && now - lastMouseTelemetryUs < MOUSE_TELEMETRY_INTERVAL_US) {
    return;
  }

  if (!force && Serial.availableForWrite() < MOUSE_TELEMETRY_MIN_WRITE_SPACE) {
    return;
  }

  uint8_t reportMouseButtons = hasMouseReports ? pendingMouseButtons : lastMouseButtons;

#if SERIAL_MOUSE_BINARY_TELEMETRY
  sendBinaryMouseTelemetry(
      pendingRawDx,
      pendingRawDy,
      pendingOutDx,
      pendingOutDy,
      pendingWheel,
      pendingWheelH,
      reportMouseButtons,
      pendingMouseReports);
#else
  printMouseTelemetry(
      pendingRawDx,
      pendingRawDy,
      pendingOutDx,
      pendingOutDy,
      pendingWheel,
      pendingWheelH,
      reportMouseButtons,
      pendingMouseReports);
#endif
  clearMouseTelemetry();
  lastMouseTelemetryUs = now;
#endif
}

void queueMouseTelemetry(int dx, int dy, int out_x, int out_y,
                         int wheel, int wheelH, uint8_t mouseButtons) {
#if SERIAL_MOUSE_TELEMETRY || SERIAL_MOUSE_BINARY_TELEMETRY
  pendingRawDx += dx;
  pendingRawDy += dy;
  pendingOutDx += out_x;
  pendingOutDy += out_y;
  pendingWheel += wheel;
  pendingWheelH += wheelH;
  pendingMouseButtons = mouseButtons;
  lastMouseButtons = mouseButtons;
  if (pendingMouseReports < 65535) {
    pendingMouseReports++;
  }
  flushMouseTelemetry(false);
#endif
}

void handleRedButton(int pin, const char *name, uint16_t bit,
                     bool &lastReading, bool &stableState, unsigned long &lastChangeMs) {
  bool reading = digitalRead(pin);

  if (reading != lastReading) {
    printButtonRawChange(name, pin, reading);
    lastChangeMs = millis();
    lastReading = reading;
  }

  if ((millis() - lastChangeMs) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      boardButtons |= bit;
    } else {
      boardButtons &= ~bit;
    }
    printButtonEvent(name, pin, stableState == LOW);
  }
}

bool applySingleKnobStep(int coarseDelta, int fineDelta) {
  int16_t candidateCoarse = coarseSteps + coarseDelta;
  int16_t candidateFine = fineSteps + fineDelta;
  long currentCpi = effectiveCpiForSteps(coarseSteps, fineSteps);
  long candidateCpi = effectiveCpiForSteps(candidateCoarse, candidateFine);
  long minValue = minEffectiveCpi();
  long maxValue = maxEffectiveCpi();

  if (candidateCpi < minValue && currentCpi <= minValue && candidateCpi <= currentCpi) {
    boundaryFlags = BOUNDARY_MIN;
    return false;
  }
  if (candidateCpi > maxValue && currentCpi >= maxValue && candidateCpi >= currentCpi) {
    boundaryFlags = BOUNDARY_MAX;
    return false;
  }

  coarseSteps = candidateCoarse;
  fineSteps = candidateFine;
  refreshKnobState();
  return true;
}

bool applyKnobSteps(int coarseDelta, int fineDelta) {
  bool changed = false;
  bool attempted = false;

  while (coarseDelta != 0) {
    int step = coarseDelta > 0 ? 1 : -1;
    attempted = true;
    changed = applySingleKnobStep(step, 0) || changed;
    coarseDelta -= step;
  }

  while (fineDelta != 0) {
    int step = fineDelta > 0 ? 1 : -1;
    attempted = true;
    changed = applySingleKnobStep(0, step) || changed;
    fineDelta -= step;
  }

  return changed || attempted;
}

int readEncoderSteps(Encoder &encoder, long &lastPos, int sign) {
  long pos = encoder.read();
  long delta = pos - lastPos;

  if (delta < ENCODER_COUNTS_PER_STEP && delta > -ENCODER_COUNTS_PER_STEP) {
    return 0;
  }

  int rawSteps = delta / ENCODER_COUNTS_PER_STEP;
  lastPos += rawSteps * ENCODER_COUNTS_PER_STEP;
  return rawSteps * sign;
}

void handleEncoders() {
  int coarseDelta = readEncoderSteps(coarseEncoder, lastCoarseEncoderPos, COARSE_ENCODER_SIGN);
  int fineDelta = readEncoderSteps(fineEncoder, lastFineEncoderPos, FINE_ENCODER_SIGN);

  if (coarseDelta != 0 || fineDelta != 0) {
    // Finish the old-gain interval before changing the scale metadata.
    flushMouseTelemetry(true);
    applyKnobSteps(coarseDelta, fineDelta);
    if (!displayBlindMode) {
      drawDisplay();
    }
    printState("knob");
  }
}

void processTrialCommand(char *line) {
  char *token = strtok(line, ",");
  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_trial");
    return;
  }
  uint16_t newTrialIndex = (uint16_t)atoi(token);

  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_baseline");
    return;
  }
  uint16_t newBaselineCpi = (uint16_t)atoi(token);

  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_randomized");
    return;
  }
  uint16_t newRandomizedCpi = (uint16_t)atoi(token);

  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_start_scale");
    return;
  }
  float newStartScale = atof(token);

  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_knob_min");
    return;
  }
  float newKnobMin = atof(token);

  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=TRIAL status=ERR reason=missing_knob_max");
    return;
  }
  float newKnobMax = atof(token);

  uint16_t newEffectiveMin = (uint16_t)((float)newRandomizedCpi * newKnobMin + 0.999f);
  uint16_t newEffectiveMax = (uint16_t)((float)newRandomizedCpi * newKnobMax);
  uint16_t newMouseInputCpi = newBaselineCpi;

  token = strtok(NULL, ",");
  if (token) {
    long value = atol(token);
    if (value < 1) value = 1;
    if (value > 65535L) value = 65535L;
    newEffectiveMin = (uint16_t)value;
  }

  token = strtok(NULL, ",");
  if (token) {
    long value = atol(token);
    if (value < 1) value = 1;
    if (value > 65535L) value = 65535L;
    newEffectiveMax = (uint16_t)value;
  }

  token = strtok(NULL, ",");
  if (token) {
    long value = atol(token);
    if (value < 1) value = 1;
    if (value > 65535L) value = 65535L;
    newMouseInputCpi = (uint16_t)value;
  }

  uint32_t commandId = 0;
  token = strtok(NULL, ",");
  if (token) {
    commandId = (uint32_t)strtoul(token, NULL, 10);
  }

  // Do not let an aggregate collected under the old gain cross this boundary.
  flushMouseTelemetry(true);

  trialIndex = newTrialIndex;
  baselineCpi = newBaselineCpi;
  mouseInputCpi = newMouseInputCpi;
  randomizedCpi = newRandomizedCpi;
  startScale = newStartScale;
  knobScaleMin = newKnobMin;
  knobScaleMax = newKnobMax;
  if (knobScaleMin < 0.01f) knobScaleMin = 0.01f;
  if (knobScaleMax < knobScaleMin) knobScaleMax = knobScaleMin;
  if (newEffectiveMax < newEffectiveMin) newEffectiveMax = newEffectiveMin;
  if (newEffectiveMin > randomizedCpi) newEffectiveMin = randomizedCpi;
  if (newEffectiveMax < randomizedCpi) newEffectiveMax = randomizedCpi;
  effectiveCpiMinLimit = newEffectiveMin;
  effectiveCpiMaxLimit = newEffectiveMax;
  lastAppliedCommandId = commandId;

  resetTrialState();
  drawDisplay();

  Serial.print("ACK cmd=TRIAL status=OK command_id=");
  Serial.print(commandId);
  Serial.print(" trial=");
  Serial.println(trialIndex);
  printState("trial_start");
}

void processBlindCommand(char *line) {
  char *token = strtok(line, ",");
  token = strtok(NULL, ",");
  if (!token) {
    Serial.println("ACK cmd=BLIND status=ERR reason=missing_enabled");
    return;
  }

  int enabled = atoi(token);
  token = strtok(NULL, ",");
  uint32_t commandId = token ? (uint32_t)strtoul(token, NULL, 10) : 0;
  bool newBlindMode = enabled != 0;
  bool changed = displayBlindMode != newBlindMode;
  displayBlindMode = newBlindMode;
  lastAppliedCommandId = commandId;
  if (changed) {
    drawDisplay();
  }

  Serial.print("ACK cmd=BLIND status=OK command_id=");
  Serial.print(commandId);
  Serial.print(" enabled=");
  Serial.println(displayBlindMode ? 1 : 0);
  printState("display");
}

void processSerialLine(char *line) {
  if (strcmp(line, "PING") == 0) {
    Serial.println("ACK cmd=PING status=OK");
    printReady();
    return;
  }

  if (strcmp(line, "BUTTONS?") == 0 || strcmp(line, "BUTTONS") == 0) {
    printButtonDiagnostics("query");
    return;
  }

  if (strncmp(line, "TRIAL,", 6) == 0) {
    processTrialCommand(line);
    return;
  }

  if (strncmp(line, "BLIND,", 6) == 0) {
    processBlindCommand(line);
    return;
  }

  Serial.print("ACK cmd=UNKNOWN status=ERR line=");
  Serial.println(line);
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      serialLine[serialLineLen] = '\0';
      if (serialLineLen > 0) {
        processSerialLine(serialLine);
      }
      serialLineLen = 0;
    } else if (c != '\r' && serialLineLen < SERIAL_LINE_MAX - 1) {
      serialLine[serialLineLen++] = c;
    }
  }
}

void setup() {
  pinMode(AX_BUTTON_PIN, INPUT_PULLUP);
  pinMode(AY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(AX_ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(AX_ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(AY_ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(AY_ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(RED_BUTTON_LEFT_PIN, INPUT_PULLUP);
  pinMode(RED_BUTTON_RIGHT_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  while (!Serial && millis() < 2000) {
  }
  printReady();

  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("EVENT name=OLED state=FAILED");
  } else {
    drawDisplay();
  }

  myusb.begin();
  printState("boot");
}

void loop() {
  myusb.Task();
  handleSerialInput();

  handleEncoders();
  handleRedButton(RED_BUTTON_LEFT_PIN, "RED_BUTTON_LEFT", BOARD_BUTTON_LEFT,
                  lastRedLeftReading, stableRedLeftState, lastRedLeftChangeMs);
  handleRedButton(RED_BUTTON_RIGHT_PIN, "RED_BUTTON_RIGHT", BOARD_BUTTON_RIGHT,
                  lastRedRightReading, stableRedRightState, lastRedRightChangeMs);

  if (mouse1.available()) {
    int dx = mouse1.getMouseX();
    int dy = mouse1.getMouseY();
    int wheel = mouse1.getWheel();
    int wheelH = mouse1.getWheelH();
    uint8_t mouseButtons = mouse1.getButtons();

    float scale = effectiveScale();
    int out_x = consumeWholePixels(rem_x, dx * scale);
    int out_y = consumeWholePixels(rem_y, dy * scale);

    if (mouseButtons & 1) Mouse.press(MOUSE_LEFT);
    else Mouse.release(MOUSE_LEFT);

    if (mouseButtons & 2) Mouse.press(MOUSE_RIGHT);
    else Mouse.release(MOUSE_RIGHT);

    if (mouseButtons & 4) Mouse.press(MOUSE_MIDDLE);
    else Mouse.release(MOUSE_MIDDLE);

    if (mouseButtons & 8) Mouse.press(MOUSE_BACK);
    else Mouse.release(MOUSE_BACK);

    if (mouseButtons & 16) Mouse.press(MOUSE_FORWARD);
    else Mouse.release(MOUSE_FORWARD);

    Mouse.move(out_x, out_y, wheel, wheelH);
    queueMouseTelemetry(dx, dy, out_x, out_y, wheel, wheelH, mouseButtons);

    mouse1.mouseDataClear();
  }

  flushMouseTelemetry(false);
}
