# Changelog

## Unreleased

## 2026.11.0

- ESPHome height reporting in cm or inches, preset controls, target-height moves,
  Stop, controller identity, load reporting and preset recovery.
- Stamp-C3 USB factory image with Wi-Fi setup, a local web interface and
  device-specific web and Home Assistant credentials. YAML installation remains
  available with OTA and optional controls.
- Optional controller firmware adds direct height commands, persistent panel
  locking with local M + T control and physical Stop, calibration, speed
  selection, independent motion trims, configurable timer sounds and display timing.
- Targeted fixes for controller sleep logic and UART receive handling.
- Browser tools prepare, install and verify the current controller firmware
  and restore supported earlier images from saved backups.
- Components work when copied without the rest of the repository. Native C++
  users retain the protocol include paths and CMake target.
- Source version and revision diagnostics, versioned configurations, firmware
  identities and checked release assets.

Hardware coverage is for the Deltaco Gaming DT410 and its documented controller.
ESP32-WROOM examples are build tested. Changed load-limit force response and
exact RTC/MCU sleep-branch execution remain unverified. Positioning depends on
speed, travel and trim settings. See [validation](docs/validation.md) and
[motion trims](docs/motion-trims.md) for coverage and limits.
