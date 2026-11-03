# Supported features and testing

Hardware results apply to the [tested controller](hardware.md#desk-and-controller-compatibility).
Host tests and successful builds do not establish physical behavior.

## Hardware validation

### RS01 R7 with the current ESPHome adapter

These checks used the R7 image and the updated `FA FA` sender together.

| Feature            | Checked on hardware                                                                                                                                                                                                                                                                                   |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Installation       | Browser preparation from CL01, installation and comparison of all application bytes by full readback                                                                                                                                                                                                  |
| UART and controls  | Controller discovery, telemetry, preset and load-setting readbacks, and remote Stop during a preset move                                                                                                                                                                                              |
| Reset              | Stop during descent, a stationary pause, explicit continuation and completed calibration with controller outputs off; physical movement confirmed by the operator                                                                                                                                     |
| Motion speed       | Normal/Slow selection and comparison runs in both directions, with short and longer direct-target moves; preset values unchanged. Target-position differences remained, including Slow upward moves stopping below the requested height. See [positioning observations](reset-speed.md#motion-speed). |
| Control status     | Ready displayed in the browser; reset, pause, continuation and completion states observed through the ESPHome API                                                                                                                                                                                     |
| Standby and wake   | Panel display went dark while ESPHome was stopped for the test. A local button woke the panel; UART, identification and Ready returned. Target movement and Stop worked after waking, with operator confirmation. Presets and load limits were unchanged.                                             |
| Patched keep-awake | Stationary telemetry stayed continuous in automatic direct mode, without new UART errors or gaps. This mode uses the firmware sleep fix and does not borrow a preset.                                                                                                                                 |
| Timer controls     | All four menu selections and cancellation, including operator confirmation of the panel display; alarm expiry was not tested                                                                                                                                                                          |
| Downward load base | Stationary write, controller readback and restoration of the original value; force response and homing with a changed value were not tested                                                                                                                                                           |

### Original, DT02 and CL01

These results apply to the original, DT02 and CL01 configurations with earlier
ESPHome software. They do not validate the current sender or Control status
layout with those images, or establish R7 coverage.

| Feature                         | Checked on hardware                                                                                                                                         |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Stamp-C3 controls               | Browser login, height display, preset recall and editing, Stop, UART reconnect and raw-load reporting                                                       |
| Preset target backend with CL01 | Upward and downward target moves, changed-target Stop, restored presets retained across power cycles, backup recovery after ESP restart, and Restore/Accept |
| Keep-awake                      | Preset priority, inactivity timing and interruption; patched telemetry continuity                                                                           |
| DT02 direct target with CL01    | Upward and downward target moves, cold-start movement, remote Stop, target changes, unlocked-panel interruption and unchanged presets                       |
| Firmware maintenance            | Installation and full application readback for original, DT02 and CL01; browser cancellation/retry; interrupted-write recovery through the maintenance API  |
| CL01 panel lock                 | Physical button blocking, remote movement and Stop, lock persistence, browser lock/unlock and confirmed-state display                                       |

### TB01 custom timer and buzzer

The [TB01 image](custom-timer-buzzer.md) was built from the exact RS01 R7 source
with pinned LLVM 23.1.0. Its full-image SHA-256 is
`d9154fc156515d9d2d169b73130ee6ab71bd21061f2e9ac5fb2107735ffb4f2a`.
The adapter checks used ESPHome 2026.10.1.
The TB01 control package was also checked after encrypted OTA installation:
web buzzer playback, web and HA timer controls, saved selections and startup
without temporary-preset recovery. Buzzer output and timer indicator changes
were confirmed on the desk.

| Feature               | Checked on hardware                                                                              |
| --------------------- | ------------------------------------------------------------------------------------------------ |
| Installation          | Full application readback matched the TB01 image                                                 |
| Home Assistant buzzer | Short and long pulses, pauses, pulse count and physical M interruption confirmed by the operator |
| Timer actions         | Set, busy/invalid rejection, HA and panel-set timer cancellation, and indicator behavior         |
| Timer expiry          | Alarm after display standby; display woke with the alarm, confirmed by the operator              |
| Firmware maintenance  | Rejected custom requests during maintenance; controls recovered after exit                       |

### FP01 shared firmware platform

The maintenance interface prepared and installed FP01 from original firmware,
installed a saved TB01 image over FP01, and prepared and reinstalled FP01 from
TB01. Each installation passed full application readback, including the cleared
extension area after downgrade. Maintenance exited with no recovery pending.
A separate cold-start diagnostic checked the 1 KiB RAM reservation before FP01
installation.

With ESPHome 2026.10.1, the browser and Home Assistant returned to Ready after
restart. Timer start, cancellation and busy rejection worked through both
interfaces. Browser buzzer playback produced three pulses, confirmed by the operator.
These stationary checks used a disconnected motor. The R7 motion and standby/wake
results above remain specific to R7.

## Controller firmware fixes

The sleep fix is included in DT02, CL01 and RS01 R7. The original firmware's
RTC hour-wrap branch could request sleep while channel-2 telemetry was active.
The patch makes that branch use the same telemetry check as the ordinary sleep
path. Arithmetic and instruction tests cover the branch. Hardware checks cover
installation, full readback and telemetry continuity. The R7 panel standby and
wake test also confirmed that restarting the adapter did not restore telemetry
until a local button was pressed. These observations do not measure the MCU's
internal STOP state or RTC phase. Use the physical panel to wake the controller.

RS01 R7 limits UART parsing to received bytes, retains partial frames across
ring wrap and clears parser state when UART reception is reinitialized. It also
rejects wrong-length kind 17 commands. The updated ESPHome sender adds a second
FA start byte so a truncated escape cannot consume the next command's start.
Use both changes together; older single-FA senders can still lose Stop and
apply a merged kind 19 write. See [framing](protocol.md#framing).

Instruction tests cover split frames, stale ring contents, escaping and
exactly-once dispatch. The R7 hardware checks above cover installation and
normal command handling, including Stop, calibration and control after panel
wake. Malformed-frame injection and DMA timing under sustained traffic have not
been tested on the physical controller. An unnoticed full 256-byte DMA-ring lap
remains indistinguishable with the current 8-bit cursors.

## Automated tests

ESPHome host tests verify that direct height control leaves legacy preset backups
untouched, including damaged or conflicting records. They cover backend changes,
reconnection, active preset recovery and command admission while detection is
incomplete. Stock preset recovery remains covered separately.

Release tests cover source and annotated-tag validation, repeatable assets,
checksum and allowed-file failures, and draft-only API operations. Synthetic inputs
check the firmware catalog against compiled install and patch-preparation guards.
Copied components, Git fixtures and a host UART peer cover source diagnostics.
The existing-device snippet compiles without a web server and retains the user's
project, network, API and OTA settings. GitHub examples validate and compile with
only the selected YAML and test secrets in the configuration directory. Tests
check that packages and components resolve to the same commit, local component
sources are replaced, and firmware builds retain the OTA guard.

The first release still requires a stationary Stamp-C3 smoke test of its exact
ESPHome candidate. The earlier R7 hardware results above apply to the unchanged
controller image. See [release procedure](releases.md#maintainer-workflow).

The CI workflow defines Debug/Release native tests with ASan/UBSan, ESPHome host
UART tests and Stamp-C3/ESP32 builds. Tests use dummy credentials and never upload
firmware.

Pull requests run once per update. A PR only adding or modifying `README.md` or
Markdown under `docs/` runs the CI selection tests and documentation checks.
These check whitespace, closed code fences and local link files; they do not
check URL availability or heading fragments. Selection uses the whole PR diff.
Other changes, including deletions, file-type changes and `CHANGELOG.md`, run
the full suite. A newer PR update cancels its previous run.

Main pushes, manual runs and release validation run every test. The `ci-result`
check requires all selected jobs to pass. Release validation also requires each
individual test job to succeed; skipped jobs cannot qualify a release.

Native and host tests cover unit conversions, framing, presets, Stop ordering,
target changes, recovery, keep-awake, timers, load values, identity and firmware
feature detection. UART simulations exercise cancellation, late or lost replies,
reconnection and storage failures. Compiler checks cover optional lock and speed
controls together and separately.

Component-layout tests build an isolated copy of `components/`, including a
host target, an ESP32 without a web server and the firmware updater. They check
that optional web hooks are registered only for the supported configurations.
Packaging tests compile each portable header independently and use the C++
library through CMake, a PlatformIO archive and a local Git dependency.

RS01 instruction tests cover calibration guards, lease expiry, interruption and
explicit continuation during descent and rebound. R7 parser tests cover split
frames, escaping, ring wrap, command dispatch and rejection of wrong-length kind
17 frames. They check recovery after each truncation of the tested commands with
the updated sender.

Firmware tests cover size limits, CRC/sequence checks, buffer ownership, image
recognition, recovery records, interrupted transfers and UI states. The
[fixture manifest](../tests/fixtures/manifest.json) records capture provenance.

Run all commands below from the repository root. Install the ESPHome version
pinned in `requirements-dev.txt` and set `TMPDIR` to a writable temporary
directory. Native CMake commands are in the [README](../README.md#tests).

```sh
python tests/esphome/run_all.py --work-dir "${TMPDIR:?}/t150-tests"
```

The reader lifecycle test needs a C++17 compiler, OpenSSL development files and
`pkg-config`. Its HTTP substitute checks reader and response ownership:

```sh
python tests/esphome/firmware_export_lifecycle.py --work-dir "${TMPDIR:?}/t150-lifecycle"
```

An optional local reference image must match the pinned original application
hash. It is read at runtime and never bundled into generated code:

```sh
python tests/esphome/firmware_profile_integration.py --work-dir "${TMPDIR:?}/t150-profile-test" --firmware /path/to/UploadedFlashImage.bin
```

GitHub example builds require a published checkout commit and repository read
access. They fetch that commit and compare generated sources with the checkout.
Use fresh work directories:

```sh
python tests/esphome/github_example_build.py --example stamp-c3 --work-dir "${TMPDIR:?}/t150-web-check/stamp"
python tests/esphome/github_example_build.py --example esp32 --work-dir "${TMPDIR:?}/t150-web-check/esp32"
```

Remove task-owned build directories and caches when finished.

## Patch reproduction

With LLVM installed, run from the repository root. These checks rebuild our
replacement code without needing a manufacturer image:

```sh
python3 tools/verify_dt02_assembly.py --llvm-bin /path/to/llvm/bin
python3 tools/embed_child_lock_patch.py --check --llvm-bin /path/to/llvm/bin
PYTHONDONTWRITEBYTECODE=1 python3 tools/test_child_lock_builder.py
node tests/esphome/firmware_ui_test.cjs
```

The UI test requires Node.js 18 or later and contacts no device. For a standalone
CL01 image, provide your saved DT02 full image or application. Output paths must
not already exist:

```sh
python3 tools/build_child_lock_patch.py /path/to/saved-dt02.bin \
  /path/to/new-cl01.bin --manifest /path/to/new-cl01.json \
  --llvm-bin /path/to/llvm/bin
```

The local CL01 emulator checks interrupted writes and startup recovery with
modeled EEPROM transport. This optional check runs outside CI.
Install Unicorn 2.1.4 in a temporary virtual environment and supply full images:

```sh
python3 tools/test_child_lock_patch.py /path/to/saved-full-dt02.bin \
  /path/to/new-full-cl01.bin /path/to/new-cl01.json --output /path/to/new-report.json
```

The builder checks its input and output hashes. Accepted application hashes are
listed in the [maintenance reference](protocol.md#firmware-maintenance).

### RS01 reproduction

Use LLVM 23.1.0 and run from the checkout root. Set `TMPDIR` to an existing
writable temporary directory for patch builds.

The builder assembles the committed `reset_speed_core.S` and checks its C-source
hash. After changing `reset_speed.c`, regenerate the assembly:

```sh
python3 tools/generate_reset_speed_core.py --clang /path/to/llvm/bin/clang
```

Check the generated assembly and embedded patch without a manufacturer image:

```sh
python3 tools/generate_reset_speed_core.py --clang /path/to/llvm/bin/clang --check
python3 tools/embed_reset_speed_patch.py --check --llvm-bin /path/to/llvm/bin
```

Formatting may change the C-source SHA-256 comment. Regenerating with another
Clang 23.1.0 distribution may also change the `.ident` compiler string.
Instructions, patch bytes, symbol addresses and accepted image hashes must stay
unchanged. Stop if they differ; do not change expected bytes or hashes to make a
formatting change pass. CI checks the generated assembly and embedded bytes.
The assembly comparison ignores only the `.ident` string. The linker discards
its `.comment` section.

To build an image, supply the exact CL01 application or full image and place
outputs outside the repository:

```sh
python3 tools/build_reset_speed_patch.py /path/to/saved-cl01.bin \
  /path/to/new-rs01.bin --manifest /path/to/new-rs01.json \
  --llvm-bin /path/to/llvm/bin
```

The builder compiles the parser with the same LLVM toolchain and requires
the resulting image to match its expected SHA-256.

The optional Unicorn instruction test takes a saved full CL01 image:

```sh
python3 tools/test_reset_speed_patch.py /path/to/saved-full-cl01.bin \
  --llvm-bin /path/to/llvm/bin --report /path/to/new-rs01-test.json
```

The parser test uses a full R7 image. Add `--stock-image` to compare complete
frames with the saved original firmware:

```sh
python3 tools/test_uart_parser_boundary.py --image /path/to/new-full-rs01.bin \
  --stock-image /path/to/saved-full-original.bin
```

### TB01 reproduction

TB01 uses generated assembly for its C11 core and controller hooks. Check the
committed assembly with LLVM 23.1.0, then run the synthetic instruction test
with Unicorn 2.1.4 installed:

```sh
python3 tools/generate_tb01_core.py --clang /path/to/llvm/bin/clang --check
python3 tools/test_tb01_patch.py --llvm-bin /path/to/llvm/bin
```

These public tests need no manufacturer image. Native tests cover timer and
buzzer state transitions and protocol transactions. The instruction test uses
synthetic controller helpers and modeled peripherals. ESPHome host tests cover
UART discovery, command results and the API actions:

```sh
python tests/esphome/run_all.py --work-dir "${TMPDIR:?}/t150-tb01-tests" \
  --only custom_timer_buzzer_integration custom_timer_buzzer_api_integration
```

### FP01 reproduction

FP01 compiles the maintained C sources directly with LLVM 23.1.0; its instruction
tests require Unicorn 2.1.4. Check the shared
layout, embedded sections and synthetic instruction tests with:

```sh
python tools/generate_platform_layout.py --check
python tools/embed_tb01_patch.py --check --llvm-bin /path/to/llvm/bin
python tools/embed_platform_patch.py --check --llvm-bin /path/to/llvm/bin
python tools/test_platform_patch.py --llvm-bin /path/to/llvm/bin
```

The instruction tests cover startup, profile rejection, RAM guards and the moved
TB01 state. Installer tests cover the accepted image transitions, full readback,
interrupted writes and repeated original-image recovery. The complete installer
matrix requires private images for all six supported versions. These checks model RAM
and peripherals; they do not measure physical SRAM or reset behavior.

## ESPHome 2026.10.1

Stamp-C3 hardware checks used a complete build from GitHub sources with the local
web UI, panel lock, motion speed and encrypted OTA:

- USB installation and subsequent OTA updates retained the device name and
  existing web and Home Assistant credentials.
- Encrypted OTA succeeded while idle and after a completed controller read with
  maintenance still claimed. An active read rejected OTA without interrupting
  maintenance; an active OTA upload rejected a new maintenance request.
- Browser login, live status, panel lock commands from the browser and Home
  Assistant, and browser maintenance entry and exit worked. Unauthenticated
  requests to the controls and maintenance endpoints were rejected.
- A full controller application read matched the known R7 image. UART telemetry
  and Home Assistant reconnected after the ESP32 updates.

Configuration and host tests cover native ESP-IDF generation, supported OTA
platforms, queued operations, upload and transfer settling, and guard release on
failure. Removing the required guard fails the link; regeneration restores it.
