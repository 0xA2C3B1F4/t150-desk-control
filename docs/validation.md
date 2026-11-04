# Supported features and testing

Hardware results apply to the [tested controller](hardware.md#desk-and-controller-compatibility).
Host tests and successful builds do not establish physical behavior.

## Hardware validation

### Current firmware maintenance

Hardware checks covered preparing the current image from FP01, restoring a saved
FP01 image, and full readback after both installations. Preparing an already-current
image preserved its bytes. Encrypted OTA succeeded after verification; active reads
blocked OTA, and OTA blocked new maintenance requests.

### TR01 motion trims

TR01 passed maintenance-page installation, complete application readback,
independent trim writes and retention after a controller power cycle. With the matching ESPHome
adapter, adjusted direct target moves were repeated in both directions at Normal
and Slow speed. Reported endpoints matched the requested heights at the display's
resolution. Short target moves and native preset returns also completed at both
speeds. Short upward moves retained a small undershoot. These are controller
readings, not external height measurements.

The fixed-profile builder reproduces the installed image from both supported R7
input formats. Host installer tests cover preparation from all six earlier images,
all 49 supported image transitions, full readback, lost final acknowledgements
and interrupted original-image recovery.

### RS01 R7 with the current ESPHome adapter

These checks used the R7 image and the updated `FA FA` sender together.

| Feature            | Checked on hardware                                                                                                                            |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Installation       | Browser preparation from CL01, installation and full application readback                                                                      |
| UART and controls  | Discovery, telemetry, preset and load-setting readbacks, and Stop during a preset move                                                         |
| Reset              | Stop, pause, explicit continuation and completed calibration                                                                                   |
| Motion speed       | Normal/Slow moves in both directions, with presets unchanged. Positioning differences remain; see [motion speed](reset-speed.md#motion-speed). |
| Control status     | Browser Ready state and API calibration states                                                                                                 |
| Standby and wake   | Panel standby, button wake, restored UART control, target movement and Stop; presets and load limits unchanged                                 |
| Patched keep-awake | Continuous stationary telemetry in automatic direct mode, without UART errors or preset borrowing                                              |
| Timer controls     | All four panel selections and cancellation; alarm expiry was not tested                                                                        |
| Downward load base | Stationary write, readback and restoration; changed-limit force response and homing were not tested                                            |

### Original, DT02 and CL01

These checks used earlier ESPHome software and apply only to the listed
controller versions.

| Feature                         | Checked on hardware                                                                                                                                         |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Stamp-C3 controls               | Browser login, height display, preset recall and editing, Stop, UART reconnect and raw-load reporting                                                       |
| Preset target backend with CL01 | Upward and downward target moves, changed-target Stop, restored presets retained across power cycles, backup recovery after ESP restart, and Restore/Accept |
| Keep-awake                      | Preset priority, inactivity timing and interruption; patched telemetry continuity                                                                           |
| DT02 direct target with CL01    | Upward and downward target moves, cold-start movement, remote Stop, target changes, unlocked-panel interruption and unchanged presets                       |
| Firmware maintenance            | Installation and full application readback for original, DT02 and CL01; browser cancellation/retry; interrupted-write recovery through the maintenance API  |
| CL01 panel lock                 | Physical button blocking, remote movement and Stop, lock persistence, browser lock/unlock and confirmed-state display                                       |

### TB01 custom timer and buzzer

The [TB01 controls](custom-timer-buzzer.md) were checked with ESPHome 2026.10.1,
including encrypted OTA, saved selections and startup without temporary-preset
recovery.

| Feature              | Checked on hardware                                                                                  |
| -------------------- | ---------------------------------------------------------------------------------------------------- |
| Installation         | Full application readback matched the TB01 image                                                     |
| Buzzer               | Web and HA playback, pulse lengths, pauses, repeat count and interruption with M                     |
| Timer actions        | Web and HA start/cancel, busy/invalid rejection, panel-set timer cancellation and indicator behavior |
| Timer expiry         | Alarm and display wake after standby                                                                 |
| Firmware maintenance | Rejected custom requests during maintenance; controls recovered after exit                           |

### FP01 shared firmware platform

Hardware checks covered installation from original firmware, downgrade to TB01
and reinstallation of FP01, each with full application readback, including the
cleared extension area after downgrade. A separate diagnostic checked the 1 KiB
RAM reservation at cold start before FP01 installation.

Stationary ESPHome 2026.10.1 checks covered restart, timer controls and buzzer
playback through web and HA, and rejection of timer requests while busy.

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

Historical DT02 through FP01 rebuilds use the
[preserved source revision](https://github.com/0xA2C3B1F4/t150-desk-control/tree/fb345b84f07ac7d0d3ffe28acf9c1d3d2631638b).
On the current checkout, `python tools/check_firmware_history.py` checks their
frozen reconstruction data. Use the [current builder](#tr01-reproduction) for
new installations.

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
matrix requires private images for all seven supported versions. These checks model RAM
and peripherals; they do not measure physical SRAM or reset behavior.

### TR01 reproduction

Offline checks reproduced identical full, application and payload images with
the current controller builder and its TR01 predecessor.

Use the same LLVM toolchain and Unicorn version as FP01:

```sh
python tools/embed_motion_trim_patch.py --check --llvm-bin /path/to/llvm/bin
python tools/test_motion_trim_builder.py --llvm-bin /path/to/llvm/bin
```

Build from an exact saved R7 full-flash or application image with LLVM 23.1.0:

```sh
python tools/build_controller_firmware.py saved-r7.bin controller-output \
  --llvm-bin /path/to/llvm/bin --require-clean-source
```

The new output directory contains the application, install payload and a hash
manifest. A full-flash input also produces a full-flash output. The builder
does not access a device. Install the application through
[firmware maintenance](firmware-update.md).

The fixed-profile checks reject unknown source images, changed patch sections,
missing identity pins and overwritten output directories. Instruction tests in CI
cover trim transactions, independent storage and the original firmware's EEPROM
writers.

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
