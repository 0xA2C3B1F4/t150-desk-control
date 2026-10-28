# ESPHome setup and behavior

Tested with ESPHome 2026.4.2. Code generation copies the portable headers from
`lib/desk_protocol` through ESPHome's includes helper.

## Setup

`stamp-c3.yaml` and `esp32.yaml` are full control profiles for
Stamp-C3 and ESP32-WROOM-32 development boards respectively. Both provide browser controls plus the Home Assistant API.
The WROOM profile assumes 4 MB flash and GPIO17 TX / GPIO16 RX; it is build-tested
only. The Stamp-C3 wiring and desk controls have been tested on hardware.
`desk.yaml` enables monitoring only.

### Configuration paths

The main YAML is the file passed to `esphome` or opened as a device in Device
Builder. Choose the source according to where that file lives:

| Workflow | YAML and secrets location | Component source |
|---|---|---|
| Local checkout | `examples/esphome/` in the complete repo, with `secrets.yaml` beside the device YAML | Leave the default local source enabled |
| ESPHome Device Builder | Device YAML and `secrets.yaml` in Device Builder's configuration directory | Uncomment the `source: github://...` line in the device YAML |
| Standalone downloaded YAML | Board YAML and `secrets.yaml` together in a directory of your choice | Uncomment the same GitHub source line |

`external_components.source.path` is relative to the main YAML's directory,
not the terminal's working directory. Thus `../../components` works for the
supplied `examples/esphome/*.yaml` layout. If you move a YAML and keep a local
source, change that path to the checkout's `components/` directory, using an
absolute path or a path relative to the new YAML location. Keep `lib/desk_protocol`
in the same checkout; copying only `components/` is insufficient.

The GitHub override replaces the local source through the YAML merge (`<<`),
so the unused local path needs no edit. ESPHome downloads the component and
portable headers on the machine running the build, including the Device Builder
host when applicable.

Use the keys from `secrets.example.yaml` in the adjacent `secrets.yaml`. Keep
`device_name` unchanged when updating an existing device unless you intend to
rename it.

`build_directory` sets ESPHome's `build_path`. In ESPHome 2026.4.2, relative build
paths start from its data directory: `.esphome/` beside the main YAML by default,
`ESPHOME_DATA_DIR` when set, or `/data` in the Home Assistant add-on. The examples'
current `.esphome/build/<device-name>` values therefore create
`.esphome/.esphome/build/<device-name>` beside the YAML in a default CLI setup.
Set `build_directory` to an absolute path to choose an exact output location.

### Standalone install

Create a directory for the device and save your board's YAML and
`secrets.example.yaml` there. Enable the GitHub source as described above,
rename the secrets example to `secrets.yaml`, and replace every placeholder.
Generate the API key with `openssl rand -base64 32`.

From that device directory, set up ESPHome with Python 3.11–3.14:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install esphome==2026.4.2
```

Run these commands from the same directory. For ESP32-WROOM-32, replace
`stamp-c3.yaml` with `esp32.yaml`:

```sh
esphome config stamp-c3.yaml
esphome compile stamp-c3.yaml
esphome upload stamp-c3.yaml
```

Connect the ESP32 by USB for the first upload and choose its serial port when
prompted. Later uploads can use the device's network address. The build/upload
output identifies the device name and address; your router also lists its IP.
Open `http://<device_name>.local/` or `http://<device-IP>/` on the same network.

### Browser controls

All examples enable [ESPHome's web server](https://esphome.io/components/web_server/)
on port 80, with the version-3 interface and locally stored JavaScript.
Sign in using `web_username` and `web_password` from `secrets.yaml`. The control
profiles expose the same height, preset, Stop, timer and recovery entities as
the Home Assistant API. The monitoring profile shows telemetry only.

Home Assistant and an internet connection are not required during use.
`api.reboot_timeout: 0s` keeps the device running without an API client.
Firmware updates use the configured ESPHome OTA password; browser firmware
upload is not enabled. Browser login, live telemetry and preset-height writing
have been tested on Stamp-C3. The WROOM profile has build coverage only.
The version-3 interface groups daily controls first, followed by presets, timers,
load settings, recovery and diagnostics. The optional firmware reader adds a
separate Firmware group. Sensor history graphs open when a sensor is clicked.
Grouping changes only the web interface, not Home Assistant entity categories.
Core interface files are embedded locally; icon fonts load externally.

### Other ESP32 boards

The adapter uses ESPHome's `UARTDevice` and preferences APIs. It contains no
Stamp-C3 pin assignments or M5Stack library dependency. The portable protocol
library has no ESPHome, Arduino or ESP-IDF dependency.

Start from the closest example and adjust:

- `esp32.variant` and `flash_size` to match the chip and flash capacity. Keep
  `framework.type: esp-idf`, as used by the tested configurations.
- `uart_tx_pin` and `uart_rx_pin` to two available pins on the board. ESP32-WROVER
  boards can use GPIO16/17 for PSRAM, so the WROOM example's pins do not apply.
  See the [ESP32-WROVER pin mapping](https://www.espressif.com/sites/default/files/documentation/esp32-wrover_datasheet_en.pdf).
- `logger` to the board's USB/serial arrangement. `baud_rate: 0` disables serial
  logging while retaining API logs; this is the generic example's setting.

Keep the desk UART at 115200 baud, 8N1, and connect TX to desk RX, RX to desk TX,
and a common ground. Power the ESP32 separately through USB.
ESP32-C3, S2, S3 and C6 boards with Wi-Fi are candidates for adaptation;
only the supplied Stamp-C3 and original ESP32 profiles have build coverage here.
See [ESPHome's ESP32 configuration](https://esphome.io/components/esp32/) for
variant settings. Validate and compile each adapted YAML, then check UART
telemetry on that board before relying on its controls.

### Local build and install

Keep the example files in their checkout locations and create
`examples/esphome/secrets.yaml` from `examples/esphome/secrets.example.yaml`.
Replace the placeholders and generate an API key with `openssl rand -base64 32`.
With ESPHome installed, run the following from the repository root. Validation
may print configuration secrets; keep its output private.

```sh
esphome config examples/esphome/stamp-c3.yaml
esphome compile examples/esphome/stamp-c3.yaml
# Choose the intended USB device or network address when installing:
esphome upload examples/esphome/stamp-c3.yaml
```

Generated build paths can be redirected with the `build_directory` substitution.
For isolated builds, set `ESPHOME_DATA_DIR`, `PLATFORMIO_CORE_DIR`,
`PLATFORMIO_BUILD_CACHE_DIR`, `IDF_TOOLS_PATH`, `PIP_CACHE_DIR` and `UV_CACHE_DIR`
to directories under your temporary work directory.

Add the ESPHome device in Home Assistant using its address and the same API key.
The protocol loop runs locally; Wi-Fi/API `reboot_timeout` is disabled in all
examples. Losing Home Assistant does not stop a desk movement already underway.

## Display units

Set `display_unit: cm` or `display_unit: in` under `desk_uart`; the default is
`cm`. Rebuild and install after changing it. The setting applies to height,
target height and all three editable preset heights, including their API unit
labels and number limits. It does not change or detect the physical panel's
unit setting. The panel and the browser/Home Assistant interfaces can use different units.

The controller uses internal position coordinates. Firmware analysis indicates
that its cm/in setting changes the panel's display conversion. No verified UART
query or telemetry field for that setting has been identified. Select the
adapter's display unit manually.

Input in inches is multiplied by 2.54 before entering the existing centimetre
control path. Display conversion uses the unrounded metric value, avoiding the
panel's whole-centimetre truncation above 100 cm. Height defaults to one displayed
decimal; it may differ from the panel's truncated inch reading.

Height number inputs use 0.1 steps in the selected unit. Inch input limits are
rounded inward to tenths, so the default 73–118 cm range permits 28.8–46.4 in.
Measured values can fall outside those rounded input limits near an endpoint.
`min_height` and `max_height` remain in centimetres, as do the C++ methods
`move_to_height()` and `set_preset_height()`. Optional `up`/`down` buttons still
request a 1 cm step. UART values, interlocks and preset backups are unchanged.
Height entities use the unit selected by `display_unit`; configuration validation
rejects conflicting `unit_of_measurement` values.

## Entities

| Configuration key | Meaning |
|---|---|
| `height` | Height in the selected display unit; mandatory |
| `connected` | Fresh UART height within three seconds, independent of Wi-Fi |
| `uart_frame_errors` / `height_sample_gap` | Link diagnostics: rejected frames and the longest gap between height samples |
| `control_status` | Current operation or reason control is blocked |
| `command_result` | Last command outcome, which can outlive the current status |
| `preset_1` / `preset_2` / `preset_3` | Recall an existing preset |
| `preset_1_height` / `preset_2_height` / `preset_3_height` | Editable Configuration values |
| `target_height` | Requested height, distinct from measured height |
| `stop` / `stop_locked` | Controlled brake and pending stop cleanup |
| `restore_preset_3` / `preset_restore_pending` | Preset RAM restoration and unresolved recovery |
| `accept_current_preset_3` | Keep the controller's current preset 3 and discard the pending backup |
| `timer_30min` / `timer_60min` / `timer_90min` / `timer_120min` | Experimental T-key sequences |
| `cancel_timer` / `timer_command_pending` | Cancellation sequence and sequence-in-progress status |

Preset recalls require `experimental_preset_buttons: true`. Target height
requires `experimental_target_height: true`. Timer buttons require
`experimental_timer_buttons: true`. These component defaults are false;
the full control profiles opt in explicitly.

Preset height edits check active scale coefficients A=24 and B=39, write one
RAM setting and verify it by readback. They do not recall the preset or move
the desk. An edit can subsequently be autosaved by the controller. The number
entities belong in Configuration; dashboards can place them beside recall buttons.
If an original preset-3 backup is retained, first use **Accept current preset 3**
to discard it before deliberately editing preset 3. Accept is available while
idle even when no restoration is pending.

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
| `display_unit` | `cm` or `in` | `cm` | Sets height entity units, input conversion and number limits; independent of panel units |
| `connected` | binary sensor | — | — |
| `control_status` / `command_result` | text sensor | — | — |
| `uart_frame_errors` | sensor | — | Diagnostic. Frames rejected for checksum, length or overflow since boot; published at most once per second |
| `height_sample_gap` | sensor | — | Diagnostic. Longest interval in ms between accepted height samples while connected, per 60-second window; unknown when no samples arrived |

Neither diagnostic sensor feeds an interlock.

### Movement and stop

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `experimental_preset_buttons` | boolean | `false` | — |
| `preset_1` / `preset_2` / `preset_3` | button | — | `experimental_preset_buttons: true` |
| `experimental_target_height` | boolean | `false` | — |
| `target_height` | number, 0.1 selected-unit steps | — | `experimental_target_height: true` |
| `up` / `down` | button, 1 cm target step | — | `experimental_target_height: true` |
| `min_height` / `max_height` | float, 73.0–118.0 cm | `73.0` / `118.0` | `min_height` below `max_height`; also bounds the height numbers |
| `stop` | button | — | — |
| `stop_locked` | binary sensor | — | — |

Preset buttons send a 150 ms button pulse. With fresh telemetry and no blocking
transaction or fault, they also work while height changes; the desk controller
decides how that press affects movement. During automatic keep-awake, see
[preset priority](#normal-operation). A target-height transaction has its own
[braking and restoration sequence](#target-height-and-preset-preservation).

### Preset heights and recovery

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `preset_1_height` / `preset_2_height` / `preset_3_height` | number, 0.1 selected-unit steps | — | — |
| `restore_preset_3` / `accept_current_preset_3` | button | — | — |
| `preset_restore_pending` | binary sensor | — | — |

### Keeping the panel awake

| Key | Type | Default | Requirements and notes |
|---|---|---|---|
| `keep_awake` | boolean | `false` | `experimental_target_height: true` |

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

The metric target range is 73–118 cm and can be narrowed in configuration.
Input steps are 0.1 cm or 0.1 in. Metric panel displays above 100 cm can omit
the decimal even though UART coordinates retain it.

Before movement the adapter reads scaling and preset 3, durably records the
original value on the ESP, writes a temporary target and verifies it. It then
sends a 150 ms preset pulse. Once release is confirmed, the first fresh height
change toward the target starts restoration and readback of the original.
The adapter continues monitoring the move after restoring the preset. It finishes
after at least three seconds from recall and 1.5 seconds of unchanged fresh
height. If no movement is detected, those same settling conditions trigger
restoration. Idle keep-awake cycles use this settling path.
The adapter restores RAM and reads it back; it does not send an EEPROM-save
command. The controller saves all three presets during a local M/save action,
sleep entry or software reset. A save while preset 3 is borrowed can therefore
persist the temporary value. RAM restoration does not verify or repair EEPROM.

Wait for the target transaction to finish before using local M/save or preset 3.
An unexpected preset value pauses the transaction for recovery. Settling is
determined from unchanged height telemetry.

A target change during an active target move requests one controlled brake,
waits for ACK, fresh settled height and restoration, then recalls the latest
requested target. Further changes during that transition replace the pending
target. Preparation/cleanup conflicts are rejected visibly. Stop, lost telemetry
or a fault cancels the pending target; reconnection does not replay movement.
Complete physical retarget behavior remains unvalidated.

Use a Home Assistant number slider that sends its value on release. Custom
cards may send intermediate values; the ESP cannot observe pointer release.
Optional `up`/`down` component buttons request 1 cm targets, not held direction
keys. The Stamp example intentionally omits them.

## Stop and recovery

### What Stop does

Stop sends `17 01 00 00` through the priority path, without waiting for network
access or fresh telemetry. It keeps the controller's soft deceleration.
Stops are suppressed while a brake ACK is pending and for two seconds after
sending. After that interval, a new explicit Stop sends a new brake request,
even if a missing ACK or preset conflict has kept the control lock active.
There is no automatic brake retry. A detector calling Stop continuously can
therefore send at most one request per two seconds; it should latch its anomaly
and avoid repeated braking targets.

The temporary stop block clears after a matching ACK, at least two seconds
since dispatch, 1.5 seconds of unchanged fresh height, confirmed release and
successful preset cleanup. Communication faults or restoration conflicts remain
visible. `clear_stop_latch()` cannot bypass these requirements. A separate load
detector must maintain its own anomaly inhibit; no detector is implemented here.

### Calling Stop from an automation

Local main-loop use:

```yaml
then:
  - desk_uart.stop: desk
```

Or call `id(desk).stop()` from a main-loop lambda. A true return reports that
the request was dispatched. This method runs in the main loop, outside an ISR.
Braking with a pending controller button event is not physically validated.

### Recovering a borrowed preset

If `Preset restoration pending` is on, follow the
[preset recovery steps](#preset-restoration-pending-is-on). Recovery does not
recall a preset or start movement. Restore verifies the saved value in RAM and
retains the backup. Accept keeps the current value and explicitly discards the
backup. Stop cancels either explicit recovery action and leaves the backup
pending. Neither action verifies EEPROM.

The ESP retains the original backup after every completed target or keep-awake
cycle. After an ESP restart or a height-telemetry gap of at least three seconds,
it checks preset 3 with read-only requests once telemetry is fresh and settled.
A matching original resolves that recovery check while keeping the backup; it
does not clear an existing keep-awake fault. Any different value needs a
recovery decision. A later preset read cannot silently discard the backup or
replace it with a new original.

Backups include the configured height profile. A different profile, damaged
record or storage error blocks movement, preset edits, Restore and Accept;
Stop and height telemetry remain available. Check **Control status** and keep
the stored data for inspection. Resolve a backup under its original profile
before switching profiles. Units, entity names and user height limits do
not change the profile identity. This identity does not identify the connected
controller or its firmware.

Earlier eight-byte records migrate with the same component ID and the built-in
profile. Accept commits a resolved record before clearing the old format, so an
interrupted cleanup cannot revive an accepted backup. Resolve backups before
downgrading: older component versions cannot enforce the profile binding.

The recovery buttons and pending sensor can also be configured with target
movement disabled. Keep them available for retained backups. Keep the
component ID stable, and resolve pending recovery before erasing ESP flash or
connecting the ESP to another desk.

## Keep-awake

### Enabling it

`keep_awake: true` requires `experimental_target_height: true`. It defaults to
false in the component and is enabled in the full Stamp profile. A local wake
is needed if the panel is already asleep.

### Normal operation

After 30 seconds without user commands or height changes, with fresh settled
height and no other transaction active, it reads the integer position and
original preset 3. It backs up, temporarily writes the
exact raw position, verifies it, rereads position to exclude a change, pulses
preset 3 and restores the original with readback. No centimetre round trip is
used. Movement, timer sequences, parameter work and faults exclude the cycle.

Healthy automatic cycles remain in DEBUG logs but do not toggle Control status
or Preset restoration pending in Home Assistant. Errors, release failures,
connection loss and interrupted recovery remain visible, as do user operations.
During a healthy cycle, a preset button cancels keep-awake before its automatic press. If preset 3
has not been borrowed, the button is forwarded immediately. Otherwise it waits
for the original preset to be restored and verified. An already-sent automatic
press finishes its release and settling sequence first. The latest button
replaces an earlier waiting button without extending the four-second deadline.
Stop, a height change, a height-sample gap over 750 ms, a transaction failure
or the deadline cancels the waiting button. Press again after the blocking
condition clears; the cancelled button is never replayed. Other conflicting
commands are rejected with a reason and leave the waiting preset unchanged.
Use Stop to cancel it. Buttons requested during an already interrupted or failed
cycle are rejected. The next cycle waits another 30 seconds after cleanup or user activity.

### Early stop before the press

If height changes, the raw position changes, or a height sample is more than
750 ms old before the preset press has been sent, the cycle stops without a
brake. If preset 3 was already written, the original is restored and read back
first; restoration pending is shown meanwhile. Once the original is verified,
or nothing was written, the next cycle starts after the normal 30 seconds and
no fault is set. If telemetry is lost before the original is verified,
restoration fails or preset 3 holds a third value, keep-awake pauses as a fault.
This also applies when a preset button interrupted the cycle.
A height-change or late-sample abort after the press has been sent requests a
brake and pauses keep-awake. A failed preset restoration remains a fault; it
does not necessarily send another brake request.

### Retained backups

Identical cycles reuse the retained ESP flash record instead of saving and
clearing it on every cycle. Retention also applies to user target moves. See
[Recovering a borrowed preset](#recovering-a-borrowed-preset) for startup,
reconnect and local-edit handling. A backup can remain stored while
`Preset restoration pending` is off; that sensor reports unresolved recovery,
not whether a retained record exists.

A restore-button request during a healthy cycle is rejected without disabling
that cycle's height-change and telemetry checks.

## Timers

Timer commands emulate short T presses after a five-second quiet interval.
There are one to four presses for 30/60/90/120 minutes, or five to cancel,
followed by another settling interval. Cancellation takes about 15 seconds.
Keep physical buttons untouched while the sequence is pending.

ACKs verify button-flag writes, not the selected timer duration. There is no
confirmed timer-state or remaining-time readback. An aborted sequence can still
leave a partial timer selection. The component does not publish a guessed countdown.
The full timer sequence, including alarm operation during sleep, still needs
hardware testing.

## Optional load settings

The full Stamp profile exposes these Configuration controls. Omit them if you
do not intend to adjust protection parameters.

| Key | Meaning |
|---|---|
| `upward_load_limit` | Parameter 0E, upward raw cutoff |
| `downward_load_limit` | Parameter 0F, downward base; also affects homing |
| `restore_upward_load_limit` / `restore_downward_load_limit` | Restore the first value read during this ESP boot |

The number range 1–65535 is a wire range, not a validated operating range.
Increasing thresholds reduces sensitivity. The downward mapping uses base +
1200 for normal motion and the base directly for homing. Downward adjustment
remains experimental. A protection trip can leave an Er3 fault requiring a reset.

Configured values are read at startup, never overwritten with defaults.
Writes are one-shot with ACK and readback; reconnects only read again.
Restore baselines live in ESP RAM and are lost on reboot. No factory-reset,
calibration or explicit EEPROM-save command is exposed. Use direct Stop for a
future anomaly detector, rather than repeatedly changing protection limits.

## Load-setting write guards

Adding a load-limit number explicitly enables that setting. Writes and restore
buttons require a known baseline, fresh telemetry no older than 750 ms, 1.5
seconds of unchanged raw height and three seconds since a normal preset recall.
Stop/release faults and other transactions block writes.

## Troubleshooting

`Control status` shows the current operation or the highest-priority blocking
condition. Healthy automatic keep-awake cycles deliberately display `Ready`
while their internal checks continue. A preset waiting for keep-awake cleanup
shows that waiting state. Other conflicting commands can still be rejected
while this entity says `Ready`.

`Last command result` shows the last reported command outcome. Check it when a
command does not run; it can explain a rejection after the current status has
changed. Normal progress messages, such as a target move or timer sequence,
usually require only waiting for that operation to finish.

### A button release is unconfirmed

Message: `Button release uncertain; inspect desk and restart ESP`.

The ESP did not receive a button-release acknowledgment, or a timer click was
held too long. The controller may still treat the button as held. Further
remote button commands are blocked.

1. Check the desk itself. Confirm that it is stationary and the panel responds
   normally. If it does not, use the desk's local controls or power switch to
   resolve its state before continuing.
2. Restart the ESP to clear its release interlock, then wait for `UART connected`.
   The restart does not itself release a button or prove that the controller
   accepted a release.
3. If `Preset restoration pending` is on, follow the recovery steps below before
   requesting a new move.

### The UART connection is lost

Message: `No fresh UART height; wake panel or check connection`.

The ESP has no fresh height sample. Samples expire after three seconds; repeated
or out-of-order frames do not refresh them. Wake the panel with a local key. If
the status remains, check wiring and UART pins. The ESP re-requests telemetry
every five seconds while the link is stale. Keep-awake maintains an awake
panel's connection; it cannot wake a panel that is already asleep.

### Stop is still blocking movement

Message: `Brake ACK missing or pending; movement blocked`.

The controller has not acknowledged Stop. Check the desk itself; this message
cannot tell you whether it stopped. After the two-second suppression interval,
you can press Stop again to send a new request. There is no automatic retry.

Message: `Stop: waiting for settled height and preset cleanup`.

Stop was acknowledged, but cleanup is incomplete. The lock clears only after
at least two seconds since Stop, 1.5 seconds of unchanged fresh height,
confirmed button release and preset cleanup. If `Preset restoration pending`
is on, resolve it below. See [What Stop does](#what-stop-does) for the braking
behavior and limits.

### Preset restoration pending is on

The ESP cannot confirm preset 3 against its retained original and blocks new
moves until recovery is resolved. Use the recovery buttons under Configuration.
The ordinary preset recall buttons command movement; they are not a way to
inspect a saved height.

1. Leave the desk stationary, wait for `UART connected`, and allow other
   operations to finish. If a release fault is shown, handle it first using
   [A button release is unconfirmed](#a-button-release-is-unconfirmed).
2. Choose one recovery action:
   - **Restore interrupted preset** (`restore_preset_3`) restores the saved
     original after two matching reads with different receive sequences. This
     explicit choice can replace a newer local value or an older temporary
     value. If the original is already present, no write is needed. Restoration
     verifies RAM, clears the pending state and retains the backup.
   - **Accept current preset 3** (`accept_current_preset_3`) keeps the value
     currently held by the controller and discards the backup after two matching
     reads. Choose this when you want to keep the current preset, for example
     after deliberately saving a new value locally. It sends no preset write
     and does not establish that the accepted value is stored in EEPROM.
3. Check `Last command result` for successful recovery and that
   `Preset restoration pending` turns off. A changing value, timeout, lost
   telemetry, Stop or failed flash update leaves the backup pending. Resolve
   the reported cause before trying again.

Neither recovery action recalls a preset or starts movement. Resolving the
backup does not bypass a separate Stop lock: that lock still needs its own ACK,
settled height and confirmed release. If the Stop ACK was lost, a new explicit
Stop can be sent after the suppression interval.

The following messages describe why recovery is pending:

| Message | Meaning |
|---|---|
| `Interrupted preset transaction; restore required` | A preset backup was loaded after an ESP restart; the transaction needs recovery |
| `Preset recovery needs a decision: restore backup or accept current preset` | A recovery check found that preset 3 differs from the saved original |
| `Preset 3 differs from retained backup; restore backup or accept current preset` | A preset read or new borrow found a different value during the current session; choose which value to keep |
| `Preset changed during recovery; backup retained` | The two confirmation reads disagreed during Restore or Accept |
| `Preset changed locally; refusing overwrite` | Automatic restoration found a third value and left it untouched; choose explicitly whether to Restore or Accept |
| `Preset recovery interrupted; backup retained` | The recovery checks no longer passed, for example because height changed or telemetry was lost |
| A timeout or readback mismatch | The value could not be confirmed; inspect `Last command result` before retrying |

### Automatic keep-awake has paused

Message: `Keep-awake failed; inspect last command result, restore preset or restart ESP`.

Read `Last command result` for the cause. If `Preset restoration pending` is
on, follow the recovery steps above. Otherwise, leave the desk stationary with
a fresh UART connection, then press **Restore interrupted preset**. This also
clears a keep-awake fault when there is no pending backup. The next cycle can
start after 30 seconds if its normal conditions are met; an ESP restart is not
needed for this case.

## Renamed example devices

The examples use `t150-desk-control` and `t150-desk-control-stamp-c3`. For an
existing installation, keep its previous `device_name` to retain the hostname,
or upload to its existing IP using `esphome upload <config> --device <IP>` when
intentionally renaming it. Replace `<config>` with the YAML path relative to your
current directory, or an absolute path. Check Home Assistant's discovery and
entity bindings afterward. Keep `id: desk` unchanged so the preset recovery key remains stable.

## Controller identity

Optional diagnostic text sensors read the controller's own UART identity fields:

```yaml
desk_uart:
  # Existing UART and height configuration goes here.
  controller_model:
    name: Controller model
  controller_manufacturer:
    name: Controller manufacturer
  controller_identifiers:
    name: Controller identifiers
  controller_date:
    name: Controller date
```

The examples include these entities. They appear in Home Assistant and in the
ESPHome web interface. They describe the desk controller; ESPHome's own device
information still describes the ESP32 running this component.

| Entity | Verified value on the tested controller | Meaning |
|---|---|---|
| Controller model | `T150-22-03` | Matches the enclosure model `T150-22-03-PJ` without its suffix. |
| Controller manufacturer | `LUMI LEGEND GROUP` | Company identifier reported by the controller. |
| Controller identifiers | `05: 1.5.0; 06: 1.6; 07: 1.9.0118` | Three identifiers, labelled by their UART selectors. Their hardware/software/protocol roles are unconfirmed; `1.5.0` may also relate to the T150 model family. |
| Controller date | `21/01/18` | A constant embedded in the firmware, probably 18 January 2021. Its meaning as a build, release or manufacturing date is unconfirmed. |

Identity values are read from the connected controller, not hard-coded from
this table. Queries run while fresh height telemetry indicates an idle desk.
Each requested field gets at most three attempts per ESP32 boot; missing or
unsupported replies leave its entity unknown. Successful static values are
kept until the ESP32 restarts. These diagnostics do not wake the controller.

The identifiers list publishes after all three fields have been read. The date
is shown exactly as reported, without converting it into a manufacturing date.
