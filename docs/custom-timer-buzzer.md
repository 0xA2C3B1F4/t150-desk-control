# Custom timer and buzzer

The current controller firmware adds adjustable volume, short pulses and a
separate sound for timer alarms. Install it through
[firmware maintenance](firmware-update.md), then enable the controls below.

## Web and Home Assistant controls

For Stamp-C3, use `examples/esphome/stamp-c3-tb01.yaml` with your existing
`secrets.yaml`. It includes desk controls, firmware maintenance and sound controls.
For another supplied board, include its example first and
`packages/custom-timer-buzzer.yaml` second. Keep the supplied directory layout.
The package uses direct height commands; saved presets remain available.

The Timer group has a duration, alarm pulse length, pause, repetitions, volume
and softening, plus Start timer and Cancel timer. The Buzzer group has separate
sound settings, Play buzzer and Cancel buzzer. Home Assistant exposes the same
controls. Support and command results appear under Diagnostics.

Changing a setting saves the selection without sending a command. Press Start
timer or Play buzzer to use it. Settings survive an ESP32 restart; timers and
sounds do not restart automatically. Changing the alarm settings after starting
a timer affects the next timer.

Both volumes default to 50%. The controls step by 5%; enter a whole number
manually or use an action for finer adjustment. Volume is relative output
strength, not measured loudness. Softening rounds the start and end of longer
pulses. It is off by default and has no effect on very short pulses, volumes
up to 5% or full-volume output.

A zero-volume timer is silent. Play buzzer at zero volume stops the current
custom sound. Cancel buzzer stops playback without cancelling a timer that has
not expired. Cancel timer also stops its custom alarm if it is already playing.
The panel's own timer and ordinary key sounds retain their original sound.

## Home Assistant actions

Automations can send all sound settings in one call, without changing the
number entities first:

```yaml
action: esphome.desk_custom_beep_profile
data:
  on_ms: 200
  off_ms: 300
  count: 3
  volume: 50
  soften: false
```

Use `esphome.desk_custom_timer_profile` with the same fields and an additional
`minutes` field for a timer alarm. Replace `desk` with your ESPHome device name.
These action names use the device name, not the Home Assistant entity name.

| Parameter | Range                 |
| --------- | --------------------- |
| `minutes` | 2–720 minutes         |
| `on_ms`   | 5–2000 ms             |
| `off_ms`  | 5–5000 ms             |
| `count`   | 1–20 pulses           |
| `volume`  | 0–100%, whole numbers |
| `soften`  | `true` or `false`     |

`custom_timer_cancel` and `custom_buzzer_cancel` take no parameters.
Existing `custom_timer_set(minutes)` and `custom_beep(on_ms, off_ms, count)`
automations keep their original behavior: the native timer alarm and full-volume
beeps with a 50 ms minimum. Use the profile actions for the new settings.

## Behavior and compatibility

The timer shares the panel's T-button timer and indicator. Cancel an existing
timer before setting another. Expiry can be almost one minute early because the
controller sets alarm seconds to zero. Cancellation of a panel-set timer works
after the T selection menu closes. A pending custom alarm survives controller
sleep, but not a power cycle.

Physical keys, motion and native sounds can interrupt custom playback. Sleep
stops an active sequence. No pause follows the final pulse. Pulse timing depends
on the controller's interrupts; it has no independent cutoff if the CPU stops.

Starting a sound or timer requires an idle, unlocked controller. Held keys,
menus, calibration and storage operations can block requests. The command-result
sensor reports acceptance or rejection. Commands are never retried automatically,
queued across a disconnect or used to wake a sleeping controller.

Older TB01, FP01 and TR01 firmware still accepts the original actions. The new
profile actions and Start timer require the current firmware. Use
`custom_timer_set` with older firmware. Unsupported requests show a result
message. A full-volume, unsoftened Play buzzer request with pulse and pause of at
least 50 ms can use the older command.

Keep the sound package after the base package. Remove duplicate custom API
actions and old timer overrides from copied YAML. If switching from temporary
preset control with an interrupted move, restore its backup first.

For offline builds, use the [current firmware builder](firmware-update.md) with
your saved R7 image. Firmware images and backups contain manufacturer code and
must remain private. See [test coverage](validation.md#custom-sound-profiles).
