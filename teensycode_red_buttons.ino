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

const float DEFAULT_KNOB_SCALE_MIN = 0.50f;
const float DEFAULT_KNOB_SCALE_MAX = 2.00f;
const float ENCODER_SCALE_STEP = 0.04f;
const int ENCODER_COUNTS_PER_STEP = 4;

const unsigned long DEBOUNCE_MS = 40;
const int SERIAL_LINE_MAX = 160;
#define SERIAL_DEBUG_RED_BUTTON_RAW 1
#define SERIAL_MOUSE_TELEMETRY 0
#define SERIAL_MOUSE_BINARY_TELEMETRY 1
const unsigned long MOUSE_TELEMETRY_INTERVAL_US = 2000;
const int MOUSE_TELEMETRY_MIN_WRITE_SPACE = 64;
const uint8_t BINARY_MAGIC_0 = 0xA5;
const uint8_t BINARY_MAGIC_1 = 0x5A;
const uint8_t BINARY_PROTOCOL_VERSION = 1;
const uint8_t BINARY_PACKET_MOUSE = 1;
const uint8_t BINARY_MOUSE_PAYLOAD_LEN = 54;

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
Encoder knobEncoder(AY_ENCODER_A_PIN, AY_ENCODER_B_PIN);

uint16_t trialIndex = 0;
uint16_t baselineCpi = 800;
uint16_t randomizedCpi = 800;
float startScale = 1.0f;
float knobScale = 1.0f;
float knobScaleMin = DEFAULT_KNOB_SCALE_MIN;
float knobScaleMax = DEFAULT_KNOB_SCALE_MAX;

float rem_x = 0.0f;
float rem_y = 0.0f;
long lastKnobEncoderPos = 0;
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

float effectiveScale() {
  return startScale * knobScale;
}

uint16_t effectiveCpi() {
  float value = (float)baselineCpi * effectiveScale();
  if (value < 0.0f) value = 0.0f;
  if (value > 65535.0f) value = 65535.0f;
  return (uint16_t)(value + 0.5f);
}

float clampKnobScale(float value) {
  boundaryFlags = 0;
  if (value < knobScaleMin) {
    value = knobScaleMin;
    boundaryFlags |= BOUNDARY_MIN;
  }
  if (value > knobScaleMax) {
    value = knobScaleMax;
    boundaryFlags |= BOUNDARY_MAX;
  }
  return round(value * 100.0f) / 100.0f;
}

void clearRemainders() {
  rem_x = 0.0f;
  rem_y = 0.0f;
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
  clearRemainders();
  clearMouseTelemetry();
  boundaryFlags = 0;
  knobEncoder.write(0);
  lastKnobEncoderPos = 0;
}

void drawDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Trial ");
  display.print(trialIndex);

  display.setCursor(72, 0);
  display.print("Knob");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(knobScale, 2);

  display.display();
}

void printReady() {
  Serial.print("READY firmware=teensy_serial_cpi protocol=1 left_pin=");
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
  Serial.print(" randomized_cpi=");
  Serial.print(randomizedCpi);
  Serial.print(" start_scale=");
  Serial.print(startScale, 6);
  Serial.print(" knob_scale=");
  Serial.print(knobScale, 6);
  Serial.print(" effective_scale=");
  Serial.print(effectiveScale(), 6);
  Serial.print(" effective_cpi=");
  Serial.print(effectiveCpi());
  Serial.print(" knob_raw=");
  Serial.print(lastKnobEncoderPos);
  Serial.print(" board_buttons=");
  Serial.print(boardButtons);
  Serial.print(" boundary_flags=");
  Serial.println(boundaryFlags);
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
  Serial.println(boardButtons);
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
  Serial.print(" baseline_cpi=");
  Serial.print(baselineCpi);
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
  Serial.print(" knob_raw=");
  Serial.print(lastKnobEncoderPos);
  Serial.print(" boundary_flags=");
  Serial.println(boundaryFlags);
}

int16_t scaleToQ1000(float value) {
  float scaled = value * 1000.0f;
  if (scaled > 32767.0f) scaled = 32767.0f;
  if (scaled < -32768.0f) scaled = -32768.0f;
  return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
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

  putU32(packet, index, telemetryCounter++);
  putU32(packet, index, millis());
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
  putU16(packet, index, randomizedCpi);
  putU16(packet, index, effectiveCpi());
  putI32(packet, index, (int32_t)lastKnobEncoderPos);
  putU16(packet, index, boundaryFlags);

  packet[index] = binaryChecksum(packet, index);
  index++;
  Serial.write(packet, index);
}

void flushMouseTelemetry(bool force) {
#if SERIAL_MOUSE_TELEMETRY || SERIAL_MOUSE_BINARY_TELEMETRY
  if (pendingMouseReports == 0) {
    return;
  }

  unsigned long now = micros();
  if (!force && now - lastMouseTelemetryUs < MOUSE_TELEMETRY_INTERVAL_US) {
    return;
  }

  if (!force && Serial.availableForWrite() < MOUSE_TELEMETRY_MIN_WRITE_SPACE) {
    return;
  }

#if SERIAL_MOUSE_BINARY_TELEMETRY
  sendBinaryMouseTelemetry(
      pendingRawDx,
      pendingRawDy,
      pendingOutDx,
      pendingOutDy,
      pendingWheel,
      pendingWheelH,
      pendingMouseButtons,
      pendingMouseReports);
#else
  printMouseTelemetry(
      pendingRawDx,
      pendingRawDy,
      pendingOutDx,
      pendingOutDy,
      pendingWheel,
      pendingWheelH,
      pendingMouseButtons,
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

void handleEncoder() {
  long pos = knobEncoder.read();
  long delta = pos - lastKnobEncoderPos;

  if (delta >= ENCODER_COUNTS_PER_STEP || delta <= -ENCODER_COUNTS_PER_STEP) {
    int steps = delta / ENCODER_COUNTS_PER_STEP;
    lastKnobEncoderPos += steps * ENCODER_COUNTS_PER_STEP;

    knobScale = clampKnobScale(knobScale + steps * ENCODER_SCALE_STEP);
    clearRemainders();
    drawDisplay();
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

  trialIndex = newTrialIndex;
  baselineCpi = newBaselineCpi;
  randomizedCpi = newRandomizedCpi;
  startScale = newStartScale;
  knobScaleMin = newKnobMin;
  knobScaleMax = newKnobMax;
  if (knobScaleMin < 0.01f) knobScaleMin = 0.01f;
  if (knobScaleMax < knobScaleMin) knobScaleMax = knobScaleMin;

  resetTrialState();
  drawDisplay();

  Serial.print("ACK cmd=TRIAL status=OK trial=");
  Serial.println(trialIndex);
  printState("trial_start");
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

  handleRedButton(RED_BUTTON_LEFT_PIN, "RED_BUTTON_LEFT", BOARD_BUTTON_LEFT,
                  lastRedLeftReading, stableRedLeftState, lastRedLeftChangeMs);
  handleRedButton(RED_BUTTON_RIGHT_PIN, "RED_BUTTON_RIGHT", BOARD_BUTTON_RIGHT,
                  lastRedRightReading, stableRedRightState, lastRedRightChangeMs);
  handleEncoder();
  flushMouseTelemetry(false);

  if (mouse1.available()) {
    int dx = mouse1.getMouseX();
    int dy = mouse1.getMouseY();
    int wheel = mouse1.getWheel();
    int wheelH = mouse1.getWheelH();
    uint8_t mouseButtons = mouse1.getButtons();

    float scale = effectiveScale();
    float sx = dx * scale + rem_x;
    float sy = dy * scale + rem_y;

    int out_x = (int)sx;
    int out_y = (int)sy;

    rem_x = sx - out_x;
    rem_y = sy - out_y;

    if (mouseButtons & 1) Mouse.press(MOUSE_LEFT);
    else Mouse.release(MOUSE_LEFT);

    if (mouseButtons & 2) Mouse.press(MOUSE_RIGHT);
    else Mouse.release(MOUSE_RIGHT);

    if (mouseButtons & 4) Mouse.press(MOUSE_MIDDLE);
    else Mouse.release(MOUSE_MIDDLE);

    Mouse.move(out_x, out_y, wheel);
    queueMouseTelemetry(dx, dy, out_x, out_y, wheel, wheelH, mouseButtons);

    mouse1.mouseDataClear();
  }
}
