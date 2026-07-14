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
TRIAL,1,800,1200,1.50000000,0.25000000,4.00000000
```

On `TRIAL`, Teensy resets both knobs, zeros movement remainders, and starts at:

```text
randomized_gain = start_scale = randomized_cpi / baseline_cpi
effective_cpi = randomized_cpi
knob_scale = 1.0
```

The left knob is coarse and the right knob is fine. Knob steps are adaptive in
CPI units:

```text
effective_cpi = randomized_cpi + 40 * coarse_steps + 4 * fine_steps
knob_scale = effective_cpi / randomized_cpi
effective_scale = effective_cpi / baseline_cpi
```

This keeps the CPI resolution fixed across randomized gains: one coarse tick is
40 CPI and one fine tick is 4 CPI.

## Teensy To PC

Control/status records are whitespace-separated `key=value` ASCII lines.
`EVENT` and `BUTTON_RAW` button rows are emitted immediately.

```text
READY firmware=teensy_serial_cpi protocol=1 left_pin=15 right_pin=17
ACK cmd=PING status=OK
ACK cmd=TRIAL status=OK trial=1
EVENT name=RED_BUTTON_RIGHT pin=17 state=PRESSED trial=0 board_buttons=2
EVENT name=RED_BUTTON_LEFT pin=15 state=PRESSED trial=1 board_buttons=1
BUTTON_RAW name=RED_BUTTON_RIGHT pin=17 raw=LOW pressed=1
BUTTONS reason=query left_pin=15 left_raw=HIGH left_stable=HIGH right_pin=17 right_raw=LOW right_stable=HIGH board_buttons=0
STATE reason=trial_start trial=1 baseline_cpi=800 randomized_cpi=1200 ...
```

Mouse telemetry is sent as compact binary packets by default:

```text
magic:       0xA5 0x5A
version:     u8, currently 1
type:        u8, 1 = mouse telemetry
payload_len: u8, currently 68
payload:     little-endian fixed layout below
checksum:    u8, sum(version..payload) & 0xFF
```

Mouse payload layout:

| Field | Type | Meaning |
|---|---|---|
| `counter` | `u32` | Firmware telemetry counter |
| `device_timestamp_ms` | `u32` | Device timestamp in milliseconds |
| `device_timestamp_us` | `u32` | Device timestamp in microseconds from Teensy `micros()` |
| `trial` | `u16` | Current trial index |
| `mouse_reports` | `u16` | Number of original mouse reports aggregated into this packet |
| `raw_dx`, `raw_dy` | `i32`, `i32` | Original mouse deltas from the dongle |
| `out_dx`, `out_dy` | `i32`, `i32` | Deltas sent to the PC after scaling |
| `wheel`, `wheel_h` | `i16`, `i16` | Wheel deltas |
| `mouse_buttons` | `u16` | Mouse button bitfield |
| `board_buttons` | `u16` | Red button bitfield |
| `start_scale_q1000` | `i16` | Trial randomized scale times 1000 |
| `knob_scale_q1000` | `i16` | Knob scale times 1000 |
| `effective_scale_q1000` | `i16` | `start_scale * knob_scale * 1000` |
| `baseline_cpi` | `u16` | Participant baseline CPI |
| `randomized_cpi` | `u16` | Current randomized CPI |
| `effective_cpi` | `u16` | Effective CPI after scaling |
| `coarse_steps` | `i16` | Accepted coarse knob steps, 40 CPI each |
| `fine_steps` | `i16` | Accepted fine knob steps, 4 CPI each |
| `knob_cpi_offset` | `i16` | `effective_cpi - randomized_cpi` |
| `coarse_raw` | `i32` | Raw coarse encoder count |
| `fine_raw` | `i32` | Raw fine encoder count |
| `boundary_flags` | `u16` | Clamp flags |

Binary mouse telemetry is rate-limited to 500 Hz (`2000 us`) and aggregates all
mouse reports received since the previous packet. During an active trial
(`trial > 0`), the Teensy also sends idle packets at the same rate when no mouse
report arrived. Idle packets have `mouse_reports=0`, zero movement deltas, and
the current knob/effective CPI state.

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
