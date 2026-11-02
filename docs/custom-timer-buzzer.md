# Custom timer and buzzer

TB01 adds a 2–720 minute timer and parameterized buzzer pulses to the exact
RS01 R7 controller image. The ESPHome controls are opt-in and require TB01
capability discovery. See [validation status](validation.md#tb01-custom-timer-and-buzzer)
before using a candidate.

The browser firmware installer does not recognize TB01. The offline builder
does not install firmware, and generated images contain manufacturer firmware.
Keep those images and backups private and outside version control.

## ESPHome actions

Merge these entries into the existing `desk_uart` and `api` sections. Retain
the device's UART, network, API encryption and OTA settings. Replace `desk`
with the existing component ID. Do not add another reader on the same UART.

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

ESPHome prefixes Home Assistant action names with the device name. Check the
actual name after installing and connecting the adapter. For a device named
`desk`, a call would look like this:

```yaml
action: esphome.desk_custom_beep
data:
  on_ms: 200
  off_ms: 300
  count: 3
```

`custom_available` requires a matching capability response and fresh telemetry.
`custom_command_result` reports rejection, transmission, controller acceptance
or an unknown outcome. Discovery results do not replace the last user-command
result. An ACK confirms command handling, not sound, timer
expiry or physical output. Interrupted or unanswered writes are never retried.
Read-only discovery retries while idle and starts again after telemetry loss
or firmware maintenance. Commands are not queued across a disconnect and do
not wake a sleeping controller.

## Behavior and limits

The timer uses the native RTC Alarm A and T-button indicator. Alarm seconds
are zero, so expiry can be almost one minute earlier than the requested
duration. Cancel the existing timer before setting another. A live alarm is
never replaced, and `custom_timer_cancel` also cancels a timer set with T.
Wait for the T selection menu to close before sending cancellation.
An already delivered native alarm is preserved if it races cancellation.

The timer and buzzer actions require the CL01 panel to be unlocked, its physical-key
gate cleared and no latched CL01 storage fault. These checks also block timer
cancellation. Commands require an idle controller, without held or pending
keys, menu, motion, reset, child-lock operation or a native alarm. Setting a
timer is BUSY while a custom beep runs. A backend error does not prove that
peripheral state stayed unchanged.

The buzzer runs from SysTick. Its final pulse ends without a trailing pause.
Keys or movement abort it on the next SysTick; native sound takes priority,
and sleep releases the custom output. Pulse-off timing requires SysTick to
keep running. TB01 adds no independent hardware output-off timer; a CPU fault
or permanent interrupt mask can leave the output high.

Press physical M to stop the custom sound. `custom_timer_cancel` affects only
the timer. ESPHome Stop does not provide a remote buzzer abort.

Every transmitted beep defers automatic preset keep-awake for
`count * on_ms + (count - 1) * off_ms + 3500` ms from transmission, up to
138500 ms. The usual 30-second quiet interval still applies. The deferral
remains after a rejected or unknown result. Later beep requests can extend an
active deferral and cannot shorten it. Explicit user controls, Stop, releases
and preset recovery are not delayed by this deferral.

## Build offline

Use LLVM 23.1.0. The builder accepts only the SHA-verified R7 source and checks
the hooks, erased reservation and patch boundary. It does not combine TB01
with other controller variants. Run from the checkout root and use a new
output directory whose parent already exists. Set `TMPDIR` to an existing
writable temporary directory:

```sh
python3 tools/build_tb01_patch.py /path/to/saved-r7.bin \
  /path/to/new-tb01 --llvm-bin /path/to/llvm/bin
```

Accepted source SHA-256 values:

| R7 source   | SHA-256                                                            |
| ----------- | ------------------------------------------------------------------ |
| Full image  | `96fe9d27e3e0f5ab2654c2552ae4e914595917b3e7b1dbbb127e773183c5a534` |
| Application | `ad1f321c570de25abef0f106175a5722d709b92e5db6d3da792bed9cc55fe78b` |

The output directory contains `manifest.json`, `t150-tb01.application.bin`
and `t150-tb01.payload.bin`. A full source image also produces
`t150-tb01.full.bin`.

The manifest records input and output hashes, tool versions, commands and
inverse changes. Reversing those changes must reproduce the source exactly.
The complete application includes its erased tail; the smaller payload is
only the existing flash window and does not provide an installation route.

See [patch reproduction](validation.md#tb01-reproduction) for public tests that
run without a manufacturer image.
