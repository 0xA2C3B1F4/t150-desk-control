# Shared firmware platform

FP01 includes direct height commands, panel locking, calibration, motion speed,
and the TB01 timer and buzzer. It reserves space for further controller extensions
without changing the existing UART commands.

FP01 is restricted to the documented STM32F030C8T6 controller with 8 KiB SRAM
and the exact supported source images. A matching desk name or UART protocol
is not enough. See [hardware identification](hardware.md) and
[firmware maintenance](firmware-update.md) before installation.

Use the same [timer and buzzer package](custom-timer-buzzer.md) for the browser
and Home Assistant controls. The maintenance page can prepare FP01 from an
original, DT02, CL01, R7 or TB01 backup. Downgrading requires a saved supported
application and full readback, as described in the maintenance guide.

## Layout and builds

`tools/patches/platform_layout.json` defines the shared flash and RAM layout.
The generator produces matching C and linker constants; the installer uses the
same payload length. FP01 reserves 1 KiB at `0x20001000` and leaves the original
stack position unchanged. Each startup clears the reservation before use.
A failed profile or RAM check disables FP01's custom timer and buzzer handling.

The flash payload contains 53 KiB. Full verification reads all 54 KiB of the
application, including the final page that the installer preserves. The layout
descriptor does not authorize an image; installation requires an exact SHA-256.

Build from a private R7 full or application backup with LLVM 23.1.0:

```sh
python tools/build_platform_patch.py /path/to/saved-r7.bin \
  /path/to/new-platform --llvm-bin /path/to/llvm/bin
```

Set `TMPDIR` to a writable temporary directory. For an acceptance build, commit
the sources and add `--require-clean-source`; the builder rejects a dirty checkout.
The manifest records the source commit, file hashes, image hashes, compiler
commands and RAM layout. Keep generated images outside
version control. See [reproduction checks](validation.md#fp01-reproduction).
