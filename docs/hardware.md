# Hardware and wiring

## Desk and controller compatibility

| Desk or identifier                        | Status            | Details                                             |
| ----------------------------------------- | ----------------- | --------------------------------------------------- |
| Deltaco Gaming DT410 / GAM-140            | Tested desk       | Development and desk tests used metric mode.        |
| T150-22-03-PJ enclosure, T150-PJ-V1.5 PCB | Tested controller | STM32F030C8T6 MCU; UART reports model `T150-22-03`. |

Sources: [DT410 product sheet](https://asset.conrad.com/media10/add/160267/c1/-/en/002749663DS00/fiche-technique-2749663-deltaco-gaming-dt410-table-de-gaming-noir.pdf)
and the [shared manual](https://www.electroland.gr/media/productattachment/0/14965/GAM140GAM140Wmanual.pdf).
See [validation](validation.md) for feature-specific test coverage.

All hardware tests ran on one DT410 desk with one T150-PJ-V1.5 controller in
metric mode. Another unit of the same model may have a different controller
revision or firmware. The maintenance page writes only over
[exact known application images](firmware-update.md#accepted-images-and-storage).
Before installing anything, check the
[controller identity](esphome.md#controller-identity) and confirm that the page
recognizes the image it reads. Report results from your desk with the
[hardware report form](https://github.com/0xA2C3B1F4/t150-desk-control/issues/new?template=hardware-report.yml).

### Possible compatibility, untested

These are candidates for firmware comparison and future support. The sources
establish related desk or panel models; they do not establish matching UART,
controller electronics or firmware. None has been tested with this project.

| Desk or controller                | Basis for investigation                                                                                                                                                                                                                                                                         |
| --------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Deltaco Gaming WT95 / GAM-140-W   | White variant of the tested GAM-140 / DT410 desk family, covered by the [shared GAM-140/GAM-140-W manual](https://www.electroland.gr/media/productattachment/0/14965/GAM140GAM140Wmanual.pdf) and [WT95 product sheet](https://www.electroland.gr/media/productattachment/0/14965/GAM140W.pdf). |
| T150-22-CH                        | An owner identifies this controller in a [repair discussion](https://www.reddit.com/r/DIY/comments/13fop5m/).                                                                                                                                                                                   |
| Kenson Get Up / T150-22           | Kenson's [controller manual](https://kenson.no/wp-content/uploads/2019/06/Kenson-Get-Up-desk-controller-manual.pdf) lists model T150-22.                                                                                                                                                        |
| LUMI T150, exact revision unknown | LUMI's [troubleshooting guide](https://www.lumi.cn/en/troubleshooting) identifies the push-button T150 panel.                                                                                                                                                                                   |
| LUMI C150, exact revision unknown | The same [LUMI guide](https://www.lumi.cn/en/troubleshooting) groups the touch-screen C150 with T150 for error and operating instructions.                                                                                                                                                      |

### Help develop support

Firmware from these controllers can reveal shared code and guide support for
another revision. In a
[hardware report](https://github.com/0xA2C3B1F4/t150-desk-control/issues/new?template=hardware-report.yml),
provide the desk model, controller and PCB markings, and how the image was
obtained. If reader compatibility has been confirmed for your controller, use
the maintenance page's [optional private upload](firmware-update.md#unknown-versions).
Quote the image's SHA-256 or upload receipt in the issue to identify it.
For images obtained another way, use the issue to arrange a private submission
without attaching the firmware publicly. A reading method must be checked for
the controller before attempting a backup.

If comparison identifies shared code and the hardware mappings and motor-control
behavior are compatible, shared code or adapted patches can be tested on the
development DT410. Operation on the candidate model still needs confirmation on
that model. Submissions do not guarantee support or authorize installation of
an unknown image; the existing reader and patches have their own compatibility
requirements.

### Protocol comparison reference

The LUMI **LP66** TV-lift family belongs in protocol research, outside the desk
compatibility list. Its [communication protocol document](https://forum.arduino.cc/uploads/short-url/1SzSmvdjiuLaih1iWS0IsTECClu.pdf)
describes 115200-baud communication with `FA`/`FD` delimiters, `FE` escaping and
an XOR checksum. The framing provides a protocol comparison reference; LP66
is not a supported device.

## UART wiring

Adapter used in development: **M5Stack Stamp-C3 C056-B**, ESP32-C3, 4 MB flash.
The protocol runs at **115200 baud, 8N1, 3.3 V logic**, with a common ground.

| Desk signal | Stamp-C3 pin | ESPHome setting  |
| ----------- | ------------ | ---------------- |
| TX, MCU PB6 | G5           | `rx_pin: GPIO5`  |
| RX, MCU PB7 | G4           | `tx_pin: GPIO4`  |
| Logic GND   | GND          | Common reference |

On the STM32F030C8T6 package, PB6 is pin 42 and PB7 is pin 43.

The connector carries UART signals rather than USB data. Only TX, RX and a
common ground are connected between the desk and the ESP32. The ESP32 is
powered separately through its own USB connection.

The Stamp 5 V and 3.3 V pins remain disconnected from the desk.

GPIO4/GPIO5 are explicitly configured. The Stamp's USB serial logger uses
UART0 on G21/G20, separately from the desk UART.

Start with monitoring and compare the reported height with the panel. Wake the
panel locally if necessary.

Board reference: [M5Stack Stamp-C3](https://docs.m5stack.com/en/core/Stamp-C3).
