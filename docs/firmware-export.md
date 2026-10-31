# Optional desk firmware reader

This read-only package downloads the controller application through its UART
bootloader. The normal desk configuration excludes it. For patching and recovery,
use [firmware maintenance](firmware-update.md) instead; the two packages cannot
own UART together.

## Enable only when needed

From the repository root, with `secrets.yaml` in `examples/esphome/`, run:

```sh
esphome run examples/esphome/stamp-c3-firmware-export.yaml
```

This includes `stamp-c3.yaml` and `packages/firmware-export.yaml`. If copying the
configuration, keep both device YAMLs and `secrets.yaml` together and preserve
`packages/`. See [configuration paths](esphome.md#configuration-paths).

For a relocated local checkout, set `firmware_components_path` in the main YAML
and update the base YAML's local component path. Both point to the checkout's
`components/`, relative to the main YAML rather than `packages/`.

For Device Builder or downloaded YAML, enable the GitHub source in `stamp-c3.yaml`
and replace the package's component source with the same repository revision:

```yaml
external_components:
  - source: github://0xA2C3B1F4/t150-desk-control@main
    components: [desk_firmware_export]
```

The package requires ESP32/ESP-IDF, UART 115200 8N1, `desk_uart` ID `desk`, a single
UART dictionary with ID `desk_serial`, and the authenticated web server. Reader
buttons and diagnostics appear in its Firmware group and in Home Assistant.
Install the normal device YAML again to remove the reader.

## Read and download

1. Leave the desk idle with fresh UART telemetry and no pending recovery.
2. Press **Read firmware**. Normal controls and polling pause.
3. At **Waiting for desk power cycle**, turn only the controller off and on.
   Keep the ESP32 powered. The reader waits up to two minutes.
4. Wait for completion, then open
   `http://<device-address>/desk-firmware/download` with the normal web login.
   Save `UploadedFlashImage.bin` on your computer.
5. Use **Clear firmware image** to free its RAM when finished. ESP32 restart or
   update also loses the image; downloading does not clear it.

A previous completed image remains available during a new read or after failure.
Check the read status and **Firmware SHA256** before downloading. If RAM is
insufficient for a second image, clear the first after saving it.

**Cancel firmware read** discards the partial read. If the controller remains
in its bootloader after cancellation or completion, power-cycle it again.
Controls require fresh settled telemetry before resuming. A closing-handshake
warning can leave complete CRC-checked data downloadable; it is not a clean
transfer result or proof of firmware authenticity.

## Firmware profile diagnostics

**Firmware profile** and **Firmware profile values** describe the downloaded
image. The recognizer accepts the known application structure with changes to
five scalar values; code changes and other layouts remain unknown but can still
be downloaded. Unknown therefore does not necessarily mean a corrupt image.

Extracted bounds and display values do not establish the active height scale
or a usable travel range. They never change control settings or approve an image
for writing. Diagnostics remain attached to the last completed image until Clear.

## File and memory limits

The bootloader header supplies the length, bounded to 128 KiB and available RAM.
The tested controller exports 55,296 application bytes from `0x08002800`; the first
10 KiB bootloader, EEPROM and option bytes are excluded. Allocation retains at
least 48 KiB of free heap. Sequence and CRC checks cover received blocks; padding
beyond the declared size is not saved or hashed.

The authenticated GET routes are `/desk-firmware/status` and
`/desk-firmware/download`. Status `size` describes the current read;
`image_size` describes the downloadable image. `image_profile` contains any
recognized metadata, with `control_approved: false`. Actions use ESPHome buttons.
