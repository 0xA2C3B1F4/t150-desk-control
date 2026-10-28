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
| Target height | Borrows preset 3, then restores its original value | Uses a profile-specific 73–118 cm range |
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
See the [protocol documentation](protocol.md) for movement and recovery behavior.

## Automated tests

The CI workflow defines native tests in Debug and Release with ASan/UBSan, ESPHome integration
tests against simulated UART peers, and full Stamp-C3 and ESP32 example builds.
The examples fetch the tested commit from GitHub and compare generated component
sources with that checkout. Tests use dummy device credentials and do not upload
firmware.

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

The GitHub build in this suite requires a published checkout commit and repository
read access. It uses Stamp-C3 by default; from the same repository root, run
either example separately with:

```sh
python tests/esphome/github_example_build.py --example stamp-c3 --work-dir "$TMPDIR/t150-web-check/stamp"
python tests/esphome/github_example_build.py --example esp32 --work-dir "$TMPDIR/t150-web-check/esp32"
```

Use fresh work directories for GitHub builds. Remove test directories and their
shared dependency caches when finished.
