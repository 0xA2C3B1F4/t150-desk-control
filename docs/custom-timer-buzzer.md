# Custom timer and buzzer

TB01 adds a 2–720 minute timer and configurable buzzer pulses to RS01 R7.
Enable the ESPHome controls below after installing TB01 or FP01. See
[test coverage](validation.md#tb01-custom-timer-and-buzzer).

Install TB01 through [firmware maintenance](firmware-update.md), or use the
offline builder below with the exact R7 image. [FP01](firmware-platform.md)
includes the same timer and buzzer commands. Keep images and backups private;
they contain manufacturer firmware.

## Web and Home Assistant controls

For Stamp-C3, use `examples/esphome/stamp-c3-tb01.yaml` with your existing
`secrets.yaml`. It includes the normal desk controls, firmware maintenance and
TB01 controls. For another supplied board example, include that example first
and `packages/custom-timer-buzzer.yaml` second in a wrapper YAML's `packages`
section. Place the wrapper beside the supplied examples and keep their directory
layout. The package uses direct height commands and omits the temporary-preset
recovery controls. Saved presets remain available.

The Timer group has a duration, Start timer and Cancel timer. It replaces the
fixed panel-timer shortcuts. The Buzzer group has pulse length, pause,
repetitions and Play buzzer. Home Assistant exposes the same numbers and buttons
on the device. Support and command-result sensors use the existing Diagnostics
group in the web interface.

Changing a number only saves the selection; press the corresponding button to
send it. Values survive an ESP32 restart, but neither timers nor sounds start
at boot. Cancel an existing timer before starting another. Press the panel's M
button to stop a buzzer sequence; TB01 has no remote buzzer-cancel command.

Keep the TB01 package after the base package. Main YAML values override packages,
so remove older timer-control overrides and duplicate custom API actions from
copied configurations, including any `target_height_mode: preset` or `auto`
override. If a previous installation has an interrupted preset move, restore its
backup before switching to this package.

## Home Assistant actions

The package also keeps these actions for automations:

| Action                | Arguments                  | Limits                                     |
| --------------------- | -------------------------- | ------------------------------------------ |
| `custom_timer_set`    | `minutes`                  | 2–720 minutes                              |
| `custom_timer_cancel` | none                       | Cancels the shared controller timer        |
| `custom_beep`         | `on_ms`, `off_ms`, `count` | 50–2000 ms on, 50–5000 ms off, 1–20 pulses |

Home Assistant prefixes action names with the device name. For a device named
`desk`:

```yaml
action: esphome.desk_custom_beep
data:
  on_ms: 200
  off_ms: 300
  count: 3
```

`custom_available` requires timer/buzzer discovery and fresh telemetry.
`custom_command_result` reports the last user command's outcome. An accepted
command does not confirm sound or timer expiry. Writes never retry. Commands
do not queue across a disconnect or wake a sleeping controller.

## Behavior and limits

The timer shares the native T-button timer and indicator. Expiry can be almost
one minute early because the controller sets alarm seconds to zero. Cancel an
existing timer before setting another. Cancellation also works for panel-set
timers once the T selection menu closes; it does not suppress an alarm already
delivered by the controller.

Commands, including cancellation, require an unlocked panel, a cleared physical-key
gate and no controller-reported storage fault. Held or pending keys, menus, motion, calibration,
lock operations and native alarms block requests. A custom beep also blocks
setting a timer. A backend error leaves the operation's outcome unknown.

Physical keys or movement stop the custom buzzer; native sound takes priority
and sleep releases the output. Press M to stop the sound. ESPHome Stop and timer
cancellation do not stop it remotely. No pause follows the final pulse. Pulses
end through SysTick, with no independent hardware cutoff if the CPU or interrupts
stop.

## Build offline

Use LLVM 23.1.0 and a new output directory under an existing parent. Run from
the checkout root with `TMPDIR` set to a writable temporary directory. The
builder checks the source hash, hooks and reserved patch space. It does not
combine TB01 with other controller variants:

```sh
python3 tools/build_tb01_patch.py /path/to/saved-r7.bin \
  /path/to/new-tb01 --llvm-bin /path/to/llvm/bin
```

Accepted source SHA-256 values:

| R7 source   | SHA-256                                                            |
| ----------- | ------------------------------------------------------------------ |
| Full image  | `96fe9d27e3e0f5ab2654c2552ae4e914595917b3e7b1dbbb127e773183c5a534` |
| Application | `ad1f321c570de25abef0f106175a5722d709b92e5db6d3da792bed9cc55fe78b` |

The output includes a manifest, complete application and smaller flash payload.
A full source image also produces a patched full image. The manifest records
hashes, build commands and reversible changes. The builder does not install
firmware; the payload alone is not an installation method.

See [patch reproduction](validation.md#tb01-reproduction) for tests that need
no manufacturer image.
