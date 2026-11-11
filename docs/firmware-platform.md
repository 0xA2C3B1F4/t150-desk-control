# Controller firmware

The current firmware includes direct height commands, panel locking, calibration,
Normal/Slow speeds, timer and buzzer controls, and independent motion trims.
Use **Prepare current firmware** in [firmware maintenance](firmware-update.md)
and the ESPHome adapter from the same release.

It supports the documented STM32F030C8T6 controller with 8 KiB SRAM and exact
known source images. Check [hardware identification](hardware.md) before use.
A matching desk name or UART protocol is not enough.

FP01 is the earlier shared-platform image. Saved FP01 and other
[supported images](firmware-update.md#firmware-versions) remain available for
restoration through the maintenance page. Their hashes keep their original
meaning.

See [build and reproduction checks](validation.md#tr01-reproduction) for the
command-line builder.
