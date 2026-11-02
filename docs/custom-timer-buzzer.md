# Custom timer and buzzer

TB01 adds a 2–720 minute timer and configurable buzzer pulses to RS01 R7.
Enable the ESPHome actions below after installing TB01. See
[test coverage](validation.md#tb01-custom-timer-and-buzzer).

The browser installer does not support TB01. The offline builder requires the
exact R7 image. Keep generated images and backups private and outside version
control; they contain manufacturer firmware.

## ESPHome actions

Merge these entries into the existing `desk_uart` and `api` sections, keeping
your device settings. Replace `desk` with your component ID. Do not add another
reader on the same UART.

```yaml
desk_uart:
  id: desk
  experimental_custom_timer_buzzer: true
  custom_available:
    name: Custom controls available
  custom_command_result:
    name: Custom command result

api:
  actions:
    - action: custom_timer_set
      variables:
        minutes: int
      then:
        - lambda: id(desk)->custom_set_timer(minutes);
    - action: custom_timer_cancel
      then:
        - lambda: id(desk)->custom_cancel_timer();
    - action: custom_beep
      variables:
        on_ms: int
        off_ms: int
        count: int
      then:
        - lambda: id(desk)->custom_beep(on_ms, off_ms, count);
```

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

`custom_available` requires TB01 discovery and fresh telemetry.
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
gate and no latched storage fault. Held or pending keys, menus, motion, calibration,
lock operations and native alarms block requests. A custom beep also blocks
setting a timer. A backend error leaves the operation's outcome unknown.

Physical keys or movement stop the custom buzzer; native sound takes priority
and sleep releases the output. Press M to stop the sound. ESPHome Stop and timer
cancellation do not stop it remotely. No pause follows the final pulse. Pulses
end through SysTick, with no independent hardware cutoff if the CPU or interrupts
stop.

After a beep request is transmitted, automatic preset keep-awake waits for the
sequence duration plus 3.5 seconds, even if the controller rejects it or does not
reply. Later requests can extend this wait, up to 138.5 seconds from transmission.
The usual 30-second quiet interval still applies. This wait does not delay user
controls, Stop, key releases or preset recovery.

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
