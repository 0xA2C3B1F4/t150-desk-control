# Changelog

## Unreleased

## 2026.11.0

- ESPHome height reporting in cm or inches, preset controls, target-height moves,
  Stop, controller identity, load reporting and preset recovery.
- Optional DT02 direct-height commands and sleep fix, CL01 persistent panel lock,
  and RS01 R7 calibration, motion-speed controls and UART parser fix.
- Browser tools for firmware backup, patch preparation, installation and recovery.
- Components that work when copied without the rest of the repository. Native
  C++ users retain the existing protocol include paths and CMake target.
- Optional `source_version` and `source_revision` diagnostic text sensors, also
  reported in startup configuration logs. Existing entity IDs are unchanged.
- Versioned example configurations, a firmware identity catalog and release
  validation tools.

Hardware coverage is for the Deltaco Gaming DT410 and its documented controller.
ESP32-WROOM examples are build tested. Timer expiry, changed load-limit force
response and exact RTC/MCU sleep-branch execution remain unverified. Slow moves
can stop short of their target. See [validation](docs/validation.md) and
[motion speed](docs/reset-speed.md#motion-speed) for coverage and limits.
