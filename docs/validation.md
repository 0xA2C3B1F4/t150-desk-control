# Supported features and testing

The supported profile comes from a Deltaco Gaming DT410 / GAM-140 with a
T150-22-03-PJ controller enclosure, T150-PJ-V1.5 PCB and STM32F030C8T6 in metric
mode during development.
See [hardware compatibility](hardware.md#desk-and-controller-compatibility)
for identification, the WT95 white variant and the scope of controller testing.

| Feature | Behavior | Limitations |
|---|---|---|
| Height | Reads channel-2 telemetry; displays cm or inches selected in YAML | Uses the same position profile in either unit |
| Presets | Recalls and edits three stored heights | Local buttons and controller autosave can conflict with remote edits |
| Target height | Borrows preset 3, restores RAM and retains its original backup | Uses a profile-specific 73–118 cm range; EEPROM restoration is unverified |
| Stop | Requests controlled deceleration | Not an emergency stop; UART failure can prevent delivery |
| Retargeting | Brakes, restores the borrowed preset, then starts the latest target | Covered by simulated-peer tests; physical retarget behavior still needs testing |
| Keep-awake | Uses the current raw position for an idle-only preset recall after 30 seconds of inactivity | Cannot wake an already sleeping UART |
| Recovery | Preserves ambiguous preset values until the user chooses how to proceed | Pending recovery blocks new movement |
| Load settings | Reads and writes raw protection thresholds | No conversion to kilograms or amperes; downward adjustment needs hardware testing |
| Timers | Sends T-key sequences for 30–120 minutes or cancellation | No timer-state readback; the full ESP-driven sequence needs hardware testing |


## Hardware validation

Stamp-C3 has been checked with the controller above for startup recovery,
preset acceptance, idle keep-awake, browser login, live telemetry and preset
readback. ESP32-WROOM-32 has build coverage; its UART wiring has not been tested
on hardware. The adapter's inch display has simulated-peer coverage;
installation and Home Assistant checks for that setting are pending.
Stamp-C3 checks also cover preset-button priority over keep-awake, the inactivity
interval and a second preset press during movement.

Early preset restoration has been checked during short moves. Long moves and
moves with the original preset far from the target remain unverified. These
checks do not establish positioning accuracy across the full height range.
Backup retention has been checked across Stamp power loss. A controller restart
with a different preset value blocked automation until explicit Restore; Restore
and Accept were checked without desk movement. EEPROM contents remain unverified.
See the [protocol documentation](protocol.md) for movement and recovery behavior.

## Automated tests

The CI workflow defines native tests in Debug and Release with ASan/UBSan, ESPHome integration
tests against simulated UART peers, and full Stamp-C3 and ESP32 example builds.
The examples fetch the tested commit from GitHub and compare generated component
sources with that checkout. Tests use dummy device credentials and do not upload
firmware.

Native profile tests compare every uint16 telemetry, preset and target input
with the original conversion behavior. Synthetic profiles cover endpoint
plateaus, gaps, binary32 rounding and rejection of unapproved control profiles.
Firmware-image tests check normalization spans, scalar validation and the
research-only extraction result. The host SHA-256 test rejects synthetic unknown
images and can also check a separately supplied known application image. Optional
configuration checks include the profile entities; the normal YAML omits them.
The reader lifecycle test compiles the unchanged adapter with narrow host
substitutes for UART, heap and synchronous HTTP sending, plus ESPHome's real
OpenSSL SHA-256. It checks retained images after failed reads and cancellation,
profile/status agreement, and download ownership during Clear in Debug and
Release with ASan/UBSan. It uses synthetic bytes; recognized profile metadata
is seeded explicitly for retention checks, without claiming image recognition.

Coverage includes framing, height units, presets, Stop, retargeting, recovery,
keep-awake, timers, load settings and controller identity. Simulated-peer tests
check timeouts, malformed replies and interrupted operations without a desk.
The [fixture manifest](../tests/fixtures/manifest.json) records UART fixture
provenance and checksums.

See the [README](../README.md#tests) for native test commands. Run the ESPHome
suite from the repository root with the version pinned in `requirements-dev.txt`.
Set `TMPDIR` to a writable temporary directory first:

```sh
python tests/esphome/run_all.py --work-dir "$TMPDIR/t150-tests"
```

The lifecycle test also needs a C++17 compiler, OpenSSL development files and
`pkg-config`. Run it alone from the same repository root:

```sh
python tests/esphome/firmware_export_lifecycle.py --work-dir "$TMPDIR/t150-lifecycle"
```

The HTTP substitute pauses the synchronous send before reading its non-owning
image pointer. These tests check adapter ownership, not ESP-IDF scheduling or
hardware SHA behavior.

To include a local reference image in the profile test, run from the same root:

```sh
python tests/esphome/firmware_profile_integration.py --work-dir "$TMPDIR/t150-profile-test" --firmware /path/to/UploadedFlashImage.bin
```

The optional file must match the pinned 55,296-byte application hash. The test
reads it at runtime and keeps synthetic variants in memory; it is not bundled
with the repository or embedded in generated code.

The GitHub build in this suite requires a published checkout commit and repository
read access. It uses Stamp-C3 by default; from the same repository root, run
either example separately with:

```sh
python tests/esphome/github_example_build.py --example stamp-c3 --work-dir "$TMPDIR/t150-web-check/stamp"
python tests/esphome/github_example_build.py --example esp32 --work-dir "$TMPDIR/t150-web-check/esp32"
```

Use fresh work directories for GitHub builds. Remove test directories and their
shared dependency caches when finished.
