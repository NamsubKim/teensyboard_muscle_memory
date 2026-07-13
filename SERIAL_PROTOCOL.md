# Teensy Serial Protocol

This sketch uses USB Serial for experiment control and telemetry. The PC still
receives the modified mouse movement through the Teensy Mouse HID interface.

## PC To Teensy

Ping:

```text
PING
```

Read current red-button pin state for diagnosis:

```text
BUTTONS?
```

Start or reset a trial:

```text
TRIAL,<trial_index>,<baseline_cpi>,<randomized_cpi>,<start_scale>,<knob_min>,<knob_max>
```

Example:

```text
TRIAL,1,800,1200,1.50000000,0.50000000,2.00000000
```

On `TRIAL`, Teensy resets `knob_scale` to `1.0`, zeros the encoder position and
movement remainders, and applies:

```text
effective_scale = start_scale * knob_scale
```

Only the left encoder changes `knob_scale`; the same effective scale is applied
to both `dx` and `dy`.

## Teensy To PC

All lines are whitespace-separated `key=value` records. `EVENT` and `BUTTON_RAW`
button rows are emitted immediately. `MOUSE` rows are disabled by default to
avoid delaying button events behind mouse telemetry. If `SERIAL_MOUSE_TELEMETRY`
is enabled in the sketch, `MOUSE` rows are aggregated and rate-limited;
`mouse_reports` tells how many original mouse reports are included in the row.

```text
READY firmware=teensy_serial_cpi protocol=1 left_pin=15 right_pin=17
ACK cmd=PING status=OK
ACK cmd=TRIAL status=OK trial=1
EVENT name=RED_BUTTON_RIGHT pin=17 state=PRESSED trial=0 board_buttons=2
EVENT name=RED_BUTTON_LEFT pin=15 state=PRESSED trial=1 board_buttons=1
BUTTON_RAW name=RED_BUTTON_RIGHT pin=17 raw=LOW pressed=1
BUTTONS reason=query left_pin=15 left_raw=HIGH left_stable=HIGH right_pin=17 right_raw=LOW right_stable=HIGH board_buttons=0
STATE reason=trial_start trial=1 baseline_cpi=800 randomized_cpi=1200 ...
MOUSE trial=1 mouse_reports=8 raw_dx=4 raw_dy=-2 out_dx=6 out_dy=-3 knob_scale=0.80 ...
```

Button bits:

| Bit | Meaning |
|---:|---|
| `0x0001` | left red button, submit current trial |
| `0x0002` | right red button, start experiment |

Boundary flags:

| Bit | Meaning |
|---:|---|
| `0x0001` | knob clamped at minimum |
| `0x0002` | knob clamped at maximum |
