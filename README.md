# Teensy CPI Red Button Firmware

Firmware for the CPI muscle-memory experiment board.

The Teensy sits between a wireless mouse dongle and the PC:

```text
wireless mouse -> mouse dongle -> Teensy USB host -> PC
```

It forwards mouse movement to the PC as USB Mouse HID after applying a CPI scale.
Experiment control and telemetry are sent over USB Serial.
The physical mouse should be set to 800 CPI for the experiment; randomized
trial gains and knob adjustments are applied in firmware.

Button/control telemetry is sent as readable ASCII lines. Mouse trajectory
telemetry is sent as compact binary packets at 500 Hz, aggregating all mouse
reports received in each 2 ms window. During an active trial, a packet is still
sent every 2 ms even if no mouse report arrived; those idle packets have
`mouse_reports=0` and zero movement deltas. The PC still receives mouse movement
immediately through Mouse HID.

## Files

| File | Purpose |
|---|---|
| `teensycode_red_buttons.ino` | Teensy sketch |
| `SERIAL_PROTOCOL.md` | Serial command and telemetry format |

## Arduino / Teensyduino Settings

Use Arduino IDE with Teensyduino installed.

Recommended settings:

| Setting | Value |
|---|---|
| Board | Teensy 4.1 |
| USB Type | Serial + Keyboard + Mouse + Joystick |
| CPU Speed | 600 MHz |
| Optimize | Faster or default |

The USB Type must include both `Serial` and `Mouse`. If Serial is missing, the
Python experiment manager will not see a COM port.

## Hardware Mapping

| Control | Pin | Role |
|---|---:|---|
| Left red button | 15 | Submit current trial |
| Right red button | 17 | Start experiment |
| Coarse encoder A | 8 | Left knob encoder A, 40 CPI per accepted tick |
| Coarse encoder B | 10 | Left knob encoder B |
| Fine encoder A | 3 | Right knob encoder A, 4 CPI per accepted tick |
| Fine encoder B | 5 | Right knob encoder B |
| OLED I2C | default Wire pins | Display current state before the experiment; trial number only in blind mode |

Both knobs change the same effective CPI for `dx` and `dy`. The firmware adapts
the gain so that one left-knob tick changes effective CPI by 40 and one
right-knob tick changes it by 4, independent of the randomized gain.

## Experiment Flow

1. Python opens the Teensy serial COM port and sends `PING`.
2. Teensy replies with `READY firmware=teensy_serial_cpi ...`.
3. User double-presses the right red button to start the experiment.
4. Python sends `BLIND,1`, so the OLED hides CPI and knob values.
5. Python sends a `TRIAL,...` command with randomized CPI settings.
6. Teensy resets both knob step counters to zero.
7. User adjusts the coarse/fine knobs until the sensitivity feels normal.
8. User presses the left red button to submit the trial.
9. Python logs the selected state and starts the next trial until the
   time-limited session expires.
10. When the session aborts or completes, Python sends `BLIND,0` and
    `TRIAL,0,800,800,1.0,...` to return the device to 800 CPI.

## Quick Serial Checks

After upload, close Arduino Serial Monitor before running Python because only one
program can own the COM port.

Expected serial check:

```text
PING
ACK cmd=PING status=OK
READY firmware=teensy_serial_cpi protocol=1 left_pin=15 right_pin=17
```

Button diagnostic command:

```text
BUTTONS?
```

Expected button event examples:

```text
EVENT name=RED_BUTTON_RIGHT pin=17 state=PRESSED trial=0 board_buttons=2
EVENT name=RED_BUTTON_LEFT pin=15 state=PRESSED trial=1 board_buttons=1
```

See `SERIAL_PROTOCOL.md` for the full protocol.
