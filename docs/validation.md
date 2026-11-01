# Supported features and testing

Hardware results apply to the [tested controller](hardware.md#desk-and-controller-compatibility).
Host tests and successful builds do not establish physical behavior.
The hardware checks below predate the new `FA FA` sender and current Control
status layout; those changes have only been tested offline.

## Hardware validation

| Feature | Checked on hardware |
|---|---|
| Stamp-C3 controls | Browser login, height display, preset recall and editing, Stop, UART reconnect and raw-load reporting |
| Preset target backend, DT02+CL01R3 | Upward and downward target moves, changed-target Stop, restored presets retained across power cycles, backup recovery after ESP restart, and Restore/Accept |
| Keep-awake | Preset priority, inactivity timing and interruption; patched telemetry continuity |
| DT02 direct target, DT02+CL01R3 | Upward and downward target moves, cold-start movement, remote Stop, target changes, unlocked-panel interruption and unchanged presets |
| Firmware maintenance | Installation and full application readback for original, DT02, CL01 and RS01 R3–R5; browser cancellation/retry; interrupted-write recovery through the maintenance API |
| CL01 panel lock | Physical button blocking, remote movement and Stop, lock persistence, browser lock/unlock and confirmed-state display |
| RS01 reset and speed, R3–R5 | Completed calibration; preset moves at Normal and Slow speeds with unchanged presets; Stop during calibration and explicit continuation on R5; preset recall and Stop after calibration |

## Controller firmware fixes

The sleep fix is included in DT02 and the cumulative CL01 and RS01 patches.
The original firmware's RTC hour-wrap branch could request sleep while channel-2
telemetry was active. The patch makes that branch use the same telemetry check
as the ordinary sleep path. Arithmetic and instruction tests cover the change;
patched firmware has also been installed, read back and observed for telemetry
continuity. Those observations do not establish execution at a specific internal
RTC boundary. The fix does not wake an already sleeping controller.

The UART receive fix is an **unreleased RS01 R7 candidate, tested offline**.
It limits parsing to received bytes, retains partial frames across ring wrap
and clears parser state when UART reception is reinitialized. It also rejects
wrong-length kind 17 commands. The updated ESPHome sender adds a second FA
start byte so a truncated escape cannot consume the next command's start.
Use both changes together; older single-FA senders can still lose Stop and
apply a merged kind 19 write. See [framing](protocol.md#framing).

R7 passes instruction and host tests. It has not been installed or tested on
the physical controller. DMA timing, sleep/wake behavior and physical Stop
remain hardware checks. An unnoticed full 256-byte DMA-ring lap is still
indistinguishable with the current 8-bit cursors.

## Automated tests

The CI workflow defines Debug/Release native tests with ASan/UBSan, ESPHome host
UART tests and Stamp-C3/ESP32 builds. Tests use dummy credentials and never upload firmware.

Native and host tests cover unit conversions, framing, presets, Stop ordering,
target changes, recovery, keep-awake, timers, load values, identity and firmware
feature detection. UART simulations exercise cancellation, late or lost replies,
reconnection and storage failures. Compiler checks cover optional lock and speed
controls together and separately.

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

RS01 builds from the exact CL01 application or full image. From the checkout
root, with LLVM tools available and outputs outside the repository:

```sh
python3 tools/build_reset_speed_patch.py /path/to/saved-cl01.bin \
  /path/to/new-rs01.bin --manifest /path/to/new-rs01.json \
  --llvm-bin /path/to/llvm/bin
python3 tools/embed_reset_speed_patch.py --check --llvm-bin /path/to/llvm/bin
```

The builder assembles the committed `reset_speed_core.S` and checks its C source
hash. Changing `reset_speed.c` requires regenerating that assembly with clang
23.1.0 via `tools/generate_reset_speed_core.py --clang /path/to/clang`.
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
