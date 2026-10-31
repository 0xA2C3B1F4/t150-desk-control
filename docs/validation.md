# Supported features and testing

Hardware results apply to the [tested controller](hardware.md#desk-and-controller-compatibility).
Host tests and successful builds do not establish physical behavior.

## Hardware validation

| Feature | Checked on hardware |
|---|---|
| Stamp-C3 controls | Browser login, height, native preset 1/2 recalls over 73–116 cm, preset 3 recall, preset editing, Stop, UART reconnect and raw-load reporting |
| Preset target backend | 73–116–73 cm on DT02+CL01R3; changed-target Stop without automatic restart; early RAM restoration; preset values retained through a controller power cycle after restoration; ESP restart backup and Restore/Accept |
| Keep-awake | Preset priority, inactivity timing and interruption; patched telemetry continuity |
| DT02 direct target | 73–116–73 cm travel on DT02+CL01R3, remote Stop, unlocked-panel interruption and unchanged preset readbacks; earlier target-change stopping and cold-start movement |
| Firmware maintenance | Browser original/DT02/CL01 install and complete readback; browser cancellation/retry; partial-block power-loss recovery through the maintenance API |
| CL01 panel lock | Complete firmware readback; button blocking, remote movement/Stop and persistence; ESPHome browser lock/unlock and confirmed-state colors; locking with the motor connected |
| Raw load | Controller telemetry readings |

## Automated tests

The CI workflow defines Debug/Release native tests with ASan/UBSan, ESPHome host
UART tests and Stamp-C3/ESP32 builds. Tests use dummy credentials and never upload firmware.

Host coverage includes metric and inch height conversions, framing, presets, Stop, recovery,
keep-awake, timers, load values, identity, automatic target detection, DT02 and CL01.
A changed target stops Moving/Monitoring and is rejected without braking in
Recall and RestoreRead/Write/Verify/ClearJournal. Explicit Stop in each phase
restores the original RAM preset without automatic restart.
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

The child-lock host UART fixture preserves outbound writes when clearing input
so it can check Stop ordering with a simulated peer.
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

The local CL01 emulator checks 1,536 interrupted-write cases with modeled
EEPROM transport and initialization. This optional check runs outside CI.
Install Unicorn 2.1.4 in a temporary virtual environment and supply full images:

```sh
python3 tools/test_child_lock_patch.py /path/to/saved-full-dt02.bin \
  /path/to/new-full-cl01.bin /path/to/new-cl01.json --output /path/to/new-report.json
```

The builder checks its input and output hashes. Accepted application hashes are
listed in the [maintenance reference](protocol.md#firmware-maintenance).
