# Hardware and wiring

## Desk and controller compatibility

| Desk or identifier | Status | Details |
|---|---|---|
| Deltaco Gaming DT410 / GAM-140, black | Tested desk | Development and desk tests used metric mode. |
| T150-22-03-PJ enclosure, T150-PJ-V1.5 PCB | Tested controller | STM32F030C8T6 MCU; UART reports model `T150-22-03`. |

Sources: [DT410 product sheet](https://asset.conrad.com/media10/add/160267/c1/-/en/002749663DS00/fiche-technique-2749663-deltaco-gaming-dt410-table-de-gaming-noir.pdf),
[shared manual](https://www.electroland.gr/media/productattachment/0/14965/GAM140GAM140Wmanual.pdf)
and the [DT410 retailer listing](https://www.multitronic.fi/en/products/3031488).
See [validation](validation.md) for feature-specific test coverage.

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
