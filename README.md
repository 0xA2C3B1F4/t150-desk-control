# T150 Desk Control

Control a T150 standing desk from ESPHome, a browser or Home Assistant.
Developed on a Deltaco Gaming DT410; see [hardware and compatibility](docs/hardware.md)
for controller markings, related models and wiring.

![T150 control panel with up, down, preset, M and T buttons](docs/images/t150-control-panel.png)

## What it does

- Displays height in cm or inches, recalls presets and edits their heights.
- Moves to a target height using preset 3, or directly with DT02/CL01 firmware.
- Provides controlled Stop, UART status and preset recovery.
- Offers optional keep-awake, raw load telemetry, load settings and timer controls.
- Supports [firmware backup, patching and recovery](docs/firmware-update.md)
  and the optional [panel lock](docs/child-lock.md).

See [supported features and testing](docs/validation.md) for completed hardware and host checks.

## Install

You need a compatible controller, a Wi-Fi ESP32 and three UART wires.
[Connect TX, RX and ground](docs/hardware.md#uart-wiring); power the ESP32
separately through USB.

| Board | Configuration | ESP32 TX / RX | Coverage |
|---|---|---|---|
| M5Stack Stamp-C3, 4 MB | [stamp-c3.yaml](examples/esphome/stamp-c3.yaml) | GPIO4 / GPIO5 | Hardware tested |
| ESP32-WROOM-32, 4 MB | [esp32.yaml](examples/esphome/esp32.yaml) | GPIO17 / GPIO16 | Build tested |

### ESPHome Device Builder

1. Create a device and replace its YAML with the board configuration above.
   Uncomment `source: github://...` under `external_components`.
2. Set `device_name`. Add the keys from
   [secrets.example.yaml](examples/esphome/secrets.example.yaml) to Device
   Builder's `secrets.yaml`, beside the device YAML. Fill in your credentials;
   retain the wizard's API encryption key or generate one with
   `openssl rand -base64 32`.
3. Validate and install over USB. Later normal-firmware updates can use Wi-Fi.
4. Open `http://<device_name>.local/` or the ESP32's IP address. Sign in with
   `web_username` and `web_password`.
5. For Home Assistant, add the **ESPHome** integration using the device address
   and API encryption key.

The GitHub source downloads both the component and its portable headers.
For use without Home Assistant, follow [standalone install](docs/esphome.md#standalone-install).
For a cloned repository, follow [local build and install](docs/esphome.md#local-build-and-install).
Both use ESPHome 2026.4.2. Copied YAML files need the
[configuration paths](docs/esphome.md#configuration-paths) described there.

## Use and configuration

[ESPHome setup and controls](docs/esphome.md) covers units, target modes,
keep-awake, settings and troubleshooting. Use the physical panel for held
Up/Down movement and waking a sleeping controller.

Firmware tools are separate optional builds:

| Tool | Guide |
|---|---|
| Read and download only | [Firmware reader](docs/firmware-export.md) |
| Read, patch, install and verify | [Firmware maintenance](docs/firmware-update.md) |
| Enable CL01 lock controls | [Panel lock](docs/child-lock.md) |

## Use the C++ library elsewhere

Add `lib/desk_protocol` from the checkout with CMake and link `desk_protocol`,
or add `lib/desk_protocol/include` to your include path. The header-only C++11
library leaves transport, timing and storage to the caller. See the
[protocol reference](docs/protocol.md) for parsing and transactions.

## Tests

Run from the repository root with CMake and a C++11 compiler. Set `TMPDIR` to a
writable temporary directory first. These tests do not access hardware:

```sh
desk_build_dir=$(mktemp -d "${TMPDIR:?Set TMPDIR to a writable temporary directory}/t150-tests.XXXXXX")
cmake -S . -B "$desk_build_dir" -DDESK_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$desk_build_dir"
ctest --test-dir "$desk_build_dir" --output-on-failure
```

See [testing](docs/validation.md#automated-tests) for ESPHome and patch tests.

## Repository

Paths relative to the repository root:

```text
lib/desk_protocol/                portable header-only C++11 library
components/desk_uart/             ESPHome adapter
components/desk_firmware_export/  optional firmware reader
components/desk_firmware_update/  optional patch and recovery interface
examples/esphome/                 monitoring and control configurations
tests/                            native tests and UART fixtures
tests/esphome/                    host/PTY integration tests
docs/                             setup and technical references
```

Source is MIT licensed. Manufacturer firmware, credentials and private research
logs are not distributed. The project is independent of the hardware manufacturers.
