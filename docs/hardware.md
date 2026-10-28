# Hardware and wiring

## Desk and controller compatibility

The development desk is a **Deltaco Gaming DT410**, also identified as
**GAM-140** in Deltaco's documentation. Its controller enclosure is marked
**T150-22-03-PJ** on the back. The investigated PCB is marked **T150-PJ-V1.5**
and uses an **STM32F030C8T6** MCU.
The enclosure identifier and PCB revision are different markings on the same
tested unit.

| Desk or identifier | Status | Evidence and scope |
|---|---|---|
| Deltaco Gaming DT410 / GAM-140, black | Tested unit | Project development and desk tests used this unit in metric mode. Feature-specific limits are listed in [validation](validation.md). |
| T150-22-03-PJ enclosure, T150-PJ-V1.5 PCB | Tested controller combination | Enclosure and PCB markings of the development unit. |
| Deltaco Gaming WT95 / GAM-140-W, white | White variant; controller untested | Same desk family, shared manual and matching published specifications. |

Deltaco's [DT410 product sheet](https://asset.conrad.com/media10/add/160267/c1/-/en/002749663DS00/fiche-technique-2749663-deltaco-gaming-dt410-table-de-gaming-noir.pdf)
and [WT95 product sheet](https://www.electroland.gr/media/productattachment/0/14965/GAM140W.pdf)
identify the article numbers GAM-140 and GAM-140-W. Both list a 140 × 75 cm
desktop, 73–118 cm height range, one motor and a 50 kg load capacity. The cover of their
[shared instruction manual](https://www.electroland.gr/media/productattachment/0/14965/GAM140GAM140Wmanual.pdf)
lists both models. These are manufacturer documents hosted by retailers.
The retailer listings identify [DT410 / GAM-140 as black](https://www.multitronic.fi/en/products/3031488)
and [WT95 / GAM-140-W as white](https://www.multitronic.fi/en/products/3031489/deltaco-gaming-wt95-white-line-motorized--pelipoyta--sahkopoyta--valkoinen).

### Untested T150 and C150 compatibility candidates

| Model or family | Reason for inclusion |
|---|---|
| T150-22-CH | Label shown in a [controller repair discussion](https://www.reddit.com/r/DIY/comments/13fop5m/). |
| T150-22-01-CH | Suggested by its close model designation; label source pending. |
| T150-22-QX | Suggested T150 variant; label source pending. |
| Kenson Get Up / T150-22 | Kenson's [controller manual](https://kenson.no/wp-content/uploads/2019/06/Kenson-Get-Up-desk-controller-manual.pdf) identifies the panel as T150-22. |
| LUMI T150, exact revision unknown | LUMI's [troubleshooting guide](https://www.lumi.cn/en/troubleshooting) identifies T150 as a push-button panel. |
| LUMI C150 touch panel | LUMI's [troubleshooting guide](https://www.lumi.cn/en/troubleshooting) groups C150 with T150 for ER1, ER2/HOT, ER3 and ER5 errors, presets, timers and reset instructions. |
| Desky Single, exact controller revision unknown | Suggested by the panel's appearance. |

### Protocol comparison reference

The LUMI **LP66** TV-lift family belongs in protocol research, outside the desk
compatibility list. Its [communication protocol document](https://forum.arduino.cc/uploads/short-url/1SzSmvdjiuLaih1iWS0IsTECClu.pdf)
describes 115200-baud communication with `FA`/`FD` delimiters, `FE` escaping and
an XOR checksum. The framing provides a protocol comparison reference; LP66
is not a supported device.

## UART wiring

Adapter used in development: **M5Stack Stamp-C3 C056-B**, ESP32-C3, 4 MB flash.
The protocol runs at **115200 baud, 8N1, 3.3 V logic**, with a common ground.

| Desk signal | Stamp-C3 pin | ESPHome setting |
|---|---|---|
| TX, MCU PB6 | G5 | `rx_pin: GPIO5` |
| RX, MCU PB7 | G4 | `tx_pin: GPIO4` |
| Logic GND | GND | Common reference |

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
