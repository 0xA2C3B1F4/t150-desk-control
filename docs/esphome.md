# ESPHome setup and controls

The examples use ESPHome 2026.4.2. Choose a board configuration from the
[README](../README.md#install); `desk.yaml` is the monitoring-only alternative.

## Setup

### Configuration paths

The main YAML is the file passed to `esphome` or opened in Device Builder.

| Workflow | YAML and secrets | Component source |
|---|---|---|
| Local checkout | Keep files in `examples/esphome/`, with `secrets.yaml` beside the device YAML | Default local source |
| Device Builder | Device YAML and `secrets.yaml` in its configuration directory | Uncomment `source: github://...` |
| Downloaded YAML | Board YAML and `secrets.yaml` together in your chosen directory | Uncomment `source: github://...` |

Copy the keys from `secrets.example.yaml` and fill in your credentials.
Generate an API encryption key with `openssl rand -base64 32` if needed.

Local component paths resolve from the main YAML's directory. The examples'
`../../components` points to the checkout's `components/`. If you move the YAML,
change that path or use an absolute path. Keep `lib/desk_protocol` in the same
checkout; copying only `components/` is insufficient. The GitHub source overrides
the local entry and downloads both the component and its headers.

`!include` paths resolve from the YAML containing the include. When copying
optional packages, preserve their `packages/` subdirectory or adjust the includes.
Package files are not downloaded by `external_components`.

`build_directory` sets ESPHome's build path, relative to its data directory.
For CLI use this defaults to `.esphome/` beside the YAML; the Home Assistant
add-on uses `/data`. The examples' `.esphome/build/...` values therefore create
`.esphome/.esphome/build/...` in a default CLI setup. Use an absolute
`build_directory` to choose an exact location.

### Standalone install

Save the board YAML and secrets file together and enable the GitHub source.
From that directory, install ESPHome with Python 3.11–3.14:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install esphome==2026.4.2
esphome config stamp-c3.yaml
esphome compile stamp-c3.yaml
esphome upload stamp-c3.yaml
```

Replace `stamp-c3.yaml` with your board's filename. Use USB for the first upload;
later uploads can use its network address. Configuration output may contain
secrets, so keep it private.

### Local build and install

Copy `examples/esphome/secrets.example.yaml` to `examples/esphome/secrets.yaml`
and fill it in. With ESPHome installed, run from the repository root:

```sh
esphome config examples/esphome/stamp-c3.yaml
esphome compile examples/esphome/stamp-c3.yaml
esphome upload examples/esphome/stamp-c3.yaml
```

Choose the intended USB port or network address when prompted. For temporary
builds, redirect `build_directory` and dependency caches outside the checkout.

### Browser controls

Open `http://<device_name>.local/` or the ESP32's IP address and log in with
`web_username` and `web_password`. Browser and Home Assistant controls use the
same entities. Core web assets are stored locally; icon fonts load externally.
Click a sensor for its history graph. Home Assistant and internet access are
optional during use.

The examples disable Wi-Fi/API reboot timeouts. Losing the network does not
stop movement already underway. ESP32 updates use ESPHome OTA or USB;
[controller firmware maintenance](firmware-update.md) is a separate optional build.

### Other ESP32 boards

Adapt `esp32.variant`, `flash_size`, `uart_tx_pin`, `uart_rx_pin` and `logger`
from the closest example. Keep the ESP-IDF framework and a hardware UART at
115200 baud, 8N1. Check pin availability: ESP32-WROVER boards may use GPIO16/17
for PSRAM. Disable serial logging with `baud_rate: 0` when appropriate.
Validate and compile, then check telemetry on the actual board before using
controls. See [wiring](hardware.md#uart-wiring) and
[ESPHome's ESP32 settings](https://esphome.io/components/esp32/).

## Display units

Set `display_unit: cm` or `in` under `desk_uart` and reinstall. This controls
height, target and preset entities independently of the panel's own setting.
Inputs use 0.1-unit steps. Inch limits round inward, giving 28.8–46.4 in for the
default range. UART-derived readings may differ from the panel's truncated display.

`min_height`, `max_height`, `move_to_height()` and `set_preset_height()` always
use centimetres. The configured range can narrow 73–118 cm, but cannot widen it.
Optional `up`/`down` buttons request 1 cm targets, not held direction keys.

## Configuration reference

Options specific to `desk_uart` are grouped below. Entity entries also accept
the standard ESPHome settings for their type, such as `name` and `id`. A dash
in the Default column means the entity is omitted unless configured.

Configuration validation checks the listed dependencies. Keep the component
`id` stable across updates: it determines the storage key for the preset backup.

### Connection and status

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `id` | ID | generated | Keep it stable: it derives the flash key of the preset recovery journal |
| `uart_id` | UART ID | the default UART bus | UART at 115200 baud, 8N1, with both TX and RX |
| `height` | sensor | **required** | — |
| `load_raw` | sensor | — | Diagnostic; integer raw value with no unit. See [raw load telemetry](#raw-load-telemetry) |
| `display_unit` | `cm` or `in` | `cm` | Sets height entity units, input conversion and number limits; independent of panel units |
| `connected` | binary sensor | — | — |
| `control_status` / `command_result` | text sensor | — | — |
| `uart_frame_errors` | sensor | — | Diagnostic. Checksum, length, overflow and incomplete-frame timeout errors since boot; published at most once per second |
| `height_sample_gap` | sensor | — | Diagnostic. Longest interval in ms between accepted height samples while connected, per 60-second window; unknown when no samples arrived |
| `uart_observation` | boolean | `false` | Cumulative DEBUG log every five seconds; no entity or UART commands |

### Movement and stop

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `experimental_preset_buttons` | boolean | `false` | — |
| `preset_1` / `preset_2` / `preset_3` | button | — | `experimental_preset_buttons: true` |
| `experimental_target_height` | boolean | `false` | — |
| `target_height_mode` | `auto`, `preset` or `direct` | `preset` | Auto detects the supported stock/DT02 identity; direct requires experimental target opt-in |
| `target_height_backend` | text sensor | not created | Configured or detected target backend, `Unknown` or `Disabled` |
| `target_height` | number, 0.1 selected-unit steps | — | `experimental_target_height: true` |
| `up` / `down` | button, 1 cm target step | — | `experimental_target_height: true` |
| `min_height` / `max_height` | float, 73.0–118.0 cm | `73.0` / `118.0` | `min_height` below `max_height`; also bounds the height numbers |
| `stop` | button | — | — |
| `stop_locked` | binary sensor | — | — |

### Panel lock

`child_lock` is a switch, `child_lock_status` a text sensor and
`child_lock_available` a binary sensor. All are optional. See
[panel lock](child-lock.md) for CL01 requirements and use.

### Preset heights and recovery

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `preset_1_height` / `preset_2_height` / `preset_3_height` | number, 0.1 selected-unit steps | — | — |
| `restore_preset_3` / `accept_current_preset_3` | button | — | — |
| `preset_restore_pending` | binary sensor | — | — |

### Keeping the panel awake

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `keep_awake` | `auto` or boolean | `false` | Experimental target opt-in; direct rejects `true` |

### Timer sequences

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `experimental_timer_buttons` | boolean | `false` | — |
| `timer_30min` / `timer_60min` / `timer_90min` / `timer_120min` / `cancel_timer` | button | — | `experimental_timer_buttons: true` |
| `timer_command_pending` | binary sensor | — | `experimental_timer_buttons: true` |

### Load limits

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `upward_load_limit` / `downward_load_limit` | number, raw 1–65535 | — | — |
| `restore_upward_load_limit` / `restore_downward_load_limit` | button | — | the matching `*_load_limit` |

The component also registers the `desk_uart.stop` action described under
[Stop and recovery](#stop-and-recovery).

## Target height and preset preservation

The examples enable preset recalls and target height explicitly. Use a number
slider that sends its value on release; custom cards may send intermediate targets.
Preset buttons forward a short press, including during movement when no other
transaction blocks it. The controller decides whether that press stops or starts motion.

### Preset backend

`target_height_mode: preset` is the default. It backs up preset 3 on the ESP32,
borrows it for the target and restores the original RAM value once movement starts
or the desk settles. Avoid local M/save and preset-3 use until the transaction ends.
The controller can autosave all presets while one is borrowed; RAM restoration
neither checks nor repairs EEPROM.

A different target while monitoring a preset-target move brakes and restores
the original preset if needed. No replacement target is queued. Wait for settling
and select the target again to start another move. Repeating the same active
target does not interrupt it. During preset preparation or restoration, target
requests are rejected without braking; use Stop to interrupt motion. Requests
during braking are also rejected.
Hardware checks on DT02+CL01R3 cover 73–116 cm travel and stopping on a changed
target; see [validation](validation.md). EEPROM persistence remains unverified.

Editing a preset-height number does not move the desk. The component verifies
its RAM value; the controller may save it later. Before deliberately changing
preset 3, discard any retained backup with **Accept current preset 3**.

### Automatic backend selection

To select the control method from the connected controller, add these settings
under `desk_uart`:

```yaml
  experimental_target_height: true
  target_height_mode: auto
  keep_awake: auto
```

After fresh settled telemetry, the component checks the supported identity and
height profile, then queries DT02. The optional `target_height_backend` shows
`Direct target (DT02)`, `Preset-based` or `Unknown`. CL01 includes DT02 support.
See the [detection reference](protocol.md#automatic-target-detection) for exact
identity matching; other stock revisions require explicit `preset` mode.

Unknown or incomplete detection blocks target moves and keep-awake, but leaves
native preset buttons and Stop available. Failed checks retry after reconnection
or maintenance. No target is replayed; select it again once ready. Runtime
identification does not verify firmware bytes. Examples retain `preset` mode
until stock auto-detection has been checked on hardware.

### Direct backend

`target_height_mode: direct` requires DT02 or CL01 and
`experimental_target_height: true`. It never borrows presets or falls back to
the preset backend. Each request checks capability and idle state first.
An uncertain acknowledgement requests Stop without retrying the target.

Changing a target during a direct move stops the desk. Wait for settling and
select the target again; no replacement is queued. A preset press during uncertain
command admission also requests Stop and must be repeated. Once a direct move
is acknowledged, preset buttons can interrupt it normally. Physical keys also
interrupt movement unless [panel lock](child-lock.md) is active.

## Stop and recovery

### What Stop does

Stop requests soft deceleration without waiting for fresh telemetry. It is not
an emergency stop; UART failure can prevent delivery. Repeated requests are
suppressed while an ACK is pending and for two seconds after dispatch. There
is no automatic retry.

Movement becomes available after the ACK, at least two seconds from dispatch,
1.5 seconds of unchanged fresh height, confirmed button release and preset cleanup.
See [troubleshooting](#troubleshooting) if the lock remains.

### Calling Stop from an automation

Use this action in a local ESPHome automation:

```yaml
then:
  - desk_uart.stop: desk
```

A main-loop lambda can call `id(desk).stop()`; a true return means dispatched,
not physically stopped. Do not call it from an interrupt handler.

### Recovering a borrowed preset

The ESP retains the original backup after target and keep-awake cycles. On
restart or UART reconnection, it checks preset 3. A matching value resolves the
check; a different value requires [Restore or Accept](#preset-restoration-pending-is-on).
These actions do not move the desk and do not verify EEPROM.

Keep `id: desk` stable because it identifies the saved backup. Resolve recovery
before erasing ESP flash, switching height profiles, downgrading the component
or connecting another desk. A profile mismatch, damaged record or storage error
blocks both recovery actions; preserve the data for inspection.

## Keep-awake

`keep_awake` defaults to `false`. Enabling it requires
`experimental_target_height: true`.

| Setting | Preset backend | DT02/CL01 direct backend |
|---|---|---|
| `false` | No workaround | No ESP workaround |
| `auto` | Enable preset-3 workaround | Rely on firmware sleep fix |
| `true` | Enable preset-3 workaround | Rejected in explicit direct mode; no workaround after auto detection selects direct |

The patched sleep behavior uses the normal telemetry subscription, not periodic
keep-awake commands. It does not wake an already sleeping UART. No keep-awake
switch is added to the interface.

In preset mode, after 30 seconds of inactivity the component briefly sets
preset 3 to the current position, recalls it and restores the backup. A user
preset press interrupts preparation and waits for cleanup if needed. Failed or
expired requests are not replayed; press again when ready. Movement or stale
telemetry aborts the cycle. After cleanup or user activity, the 30-second wait
starts again. Faults pause automatic cycles until resolved.

## Timers

Timer buttons emulate T-key presses after a quiet interval. Keep physical
buttons untouched while the sequence is pending. Cancellation takes about
15 seconds. There is no verified duration or remaining-time readback, and an
aborted sequence can leave a partial selection. Full timer/alarm behavior still
needs hardware testing.

## Raw load telemetry

Add `load_raw: {name: "Load (raw)"}` under `desk_uart` for the filtered 16-bit
value from existing height telemetry. It sends no extra requests and becomes
unknown when telemetry expires or firmware reading begins. Its units and filter
delay are not calibrated; it is not used for obstruction detection.

## Optional load settings

Load-limit numbers use raw values, not kilograms or amperes. The range 1–65535
is the wire range, not a validated operating range. Increasing thresholds
reduces sensitivity. The downward base also affects homing and remains experimental.
A protection trip can require a panel reset.

Values are read at startup, never replaced by defaults. Writes need an idle desk,
fresh settled telemetry and no pending transaction. Each write uses ACK and
readback without automatic retry. Restore buttons use the first value read
since the ESP boot; those baselines disappear on reboot. No calibration,
factory-reset or explicit EEPROM-save command is exposed.

## Troubleshooting

**Control status** describes the current operation or blocking condition.
**Last command result** explains the latest outcome, even after status changes.
Healthy background keep-awake can show `Ready` while cleanup still blocks a command.

### A button release is unconfirmed

For `Button release uncertain; inspect desk and restart ESP`, check the desk
and resolve any held-button behavior locally or with its power switch. Then
restart the ESP and wait for UART telemetry. Restarting clears the ESP interlock;
it does not prove the controller released the button. Resolve pending preset
recovery before moving again.

### The UART connection is lost

Height expires after three seconds without a valid new sample. Wake the panel
with a local key, then check wiring and pins if telemetry remains absent.
The ESP requests telemetry every five seconds while disconnected; keep-awake
cannot wake an already sleeping controller.

### Stop is still blocking movement

For a missing brake ACK, inspect whether the desk stopped. After the two-second
suppression interval, press Stop again if needed. If status says it is waiting
for settled height and preset cleanup, allow settling and resolve any pending
preset recovery. Restore/Accept do not clear a separate missing-ACK lock.

### Preset restoration pending is on

Leave the desk stationary with fresh UART telemetry and no release fault.
Choose one action:

- **Restore interrupted preset** restores the saved original in RAM and retains
  the backup. This can overwrite a newer local preset.
- **Accept current preset 3** keeps the controller's value and discards the
  backup. Choose it after a deliberate local edit you want to retain.

Check the command result and that pending recovery turns off. Both choices
verify the current value before completing. Stop, changing values, lost telemetry
or a failed storage update leaves recovery pending; resolve the cause and retry.

| Reported problem | Action |
|---|---|
| Different height profile | Use the original component/profile and component ID to resolve the backup. |
| Storage or legacy cleanup failed | Restart to recheck; preserve the data if the error persists. |
| Unreadable backup or disagreeing records | Preserve the records for inspection; neither button can choose or repair them. |

### Automatic keep-awake has paused

Read the last command result and resolve pending preset recovery. If no recovery
is pending, leave the desk idle with fresh telemetry and press **Restore
interrupted preset** to clear the keep-awake fault. The next cycle can start
after 30 seconds.

## Renamed example devices

Keep `device_name` when updating an existing installation. To rename it, upload
to its current IP with `esphome upload <config> --device <IP>`, where `<config>`
is your YAML path. Check Home Assistant discovery afterward. Keep the component
ID unchanged to retain its recovery storage key.

## Controller identity

The examples expose `controller_model`, `controller_manufacturer`,
`controller_identifiers` and `controller_date` as diagnostic text sensors.
They describe the desk controller, not the ESP32. Reads occur while idle;
missing replies remain unknown and successful values persist until ESP restart.
The date is displayed as received, without interpreting it as a manufacturing date.
See [identity fields](protocol.md#controller-identity) for exact replies.
