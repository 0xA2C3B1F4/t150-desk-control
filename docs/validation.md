# Supported features and testing

Hardware results apply to the [tested controller](hardware.md#desk-and-controller-compatibility).
Host tests and successful builds do not establish physical behavior.

## Hardware validation

| Feature | Checked on hardware | Remaining limits |
|---|---|---|
| Stamp-C3 controls | Browser login, height, preset recall/editing, Stop, UART reconnect and raw-load reporting | Full-range positioning accuracy unmeasured |
| Preset target backend | Early restoration during short moves; backup retention across ESP restart; Restore/Accept after controller restart | Long moves, full retarget behavior and EEPROM restoration unverified |
| Keep-awake | Preset priority, inactivity timing and interruption; patched telemetry continuity | Observed continuity does not prove execution of a particular RTC branch |
| DT02 direct target | Supervised moves, Stop, target-change stopping, panel interruption and cold-start movement | Full-range positioning accuracy unmeasured |
| Firmware maintenance | Browser original/DT02 install and complete readback; browser cancellation/retry; partial-block power-loss recovery through the maintenance API | CL01 browser installation is host-tested, not yet device-tested |
| CL01 R3 firmware | Complete readback; physical button blocking, remote movement/Stop, lock/unlock persistence and physical control after unlock | New ESPHome lock controls still need a device test; EEPROM brownouts, interrupt latency and held-key contact bounce unmeasured |
| ESP32-WROOM-32 and inch display | Not hardware-tested | Build and simulated-peer coverage only |
| Timers and load settings | Raw-load readings checked; controller parameters investigated | Full timer/alarm sequence and downward-threshold adjustment unverified; load units uncalibrated |

Automatic target selection has host coverage; stock detection needs a device check
before becoming the example default. The panel-lock offline emulator includes
1,536 interrupted-write cases with modeled EEPROM transport and initialization.
It does not establish electrical brownout behavior or EEPROM endurance.

## Automated tests

The CI workflow defines Debug/Release native tests with ASan/UBSan, ESPHome host
UART tests and Stamp-C3/ESP32 builds. Tests use dummy credentials and never upload firmware.

Coverage includes framing, height conversions, presets, Stop, recovery,
keep-awake, timers, load values, identity, target detection, DT02 and CL01.
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
`pkg-config`. Its HTTP substitute checks ownership, not ESP-IDF scheduling:

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
so it can check Stop ordering. It does not verify physical UART drain timing.
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

For firmware emulation, install Unicorn 2.1.4 in a temporary virtual environment
and supply full images:

```sh
python3 tools/test_child_lock_patch.py /path/to/saved-full-dt02.bin \
  /path/to/new-full-cl01.bin /path/to/new-cl01.json --output /path/to/new-report.json
```

The builder checks its input and output hashes. Accepted application hashes are
listed in the [maintenance reference](protocol.md#firmware-maintenance).
