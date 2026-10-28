# Optional desk firmware reader

The normal desk firmware does not include the reader. The optional
`desk_firmware_export` component reads the T150 application through its own UART
bootloader into ESP32 RAM. The file header supplies its size, from 1 byte to
128 KiB. The inspected controller exports 55,296 bytes from `0x08002800`, excluding
its first 10 KiB bootloader, EEPROM and option bytes. There are no firmware upload,
erase, protection-change or software-reset commands.

## Enable only when needed

From the repository root, run
`esphome compile examples/esphome/stamp-c3-firmware-export.yaml` instead of the
normal `examples/esphome/stamp-c3.yaml`. Use the same
`examples/esphome/secrets.yaml` for both. The optional configuration includes the
normal one and `packages/firmware-export.yaml`, so credentials, device identity and normal
controls stay the same. Installing either configuration reboots the ESP32.
Use the normal configuration again to remove the reader entirely.

For an existing device YAML in `examples/esphome/`, add this to that file:

```yaml
packages:
  firmware_export: !include packages/firmware-export.yaml
```

The package expects `desk_uart` with ID `desk`, a single UART dictionary with
ID `desk_serial`, and the existing `web_server`. It merges a 4096-byte RX buffer,
64-byte receive threshold and two-symbol timeout into that UART. The reader
requires ESP32 with ESP-IDF and 115200 8N1. Tested with ESPHome 2026.4.2.

`!include` paths are relative to the YAML file containing the include. If you
copy this setup, keep `stamp-c3-firmware-export.yaml`, `stamp-c3.yaml` and
`secrets.yaml` together, with `firmware-export.yaml` in a `packages/` subdirectory.
Adjust the includes if you use a different layout.

The package's `firmware_components_path` defaults to `../../components`, relative
to the main YAML's directory, not the `packages/` directory. For a moved local
setup, set this substitution in the main YAML and update the normal profile's
local source path to the same checkout's `components/` directory.

For Device Builder or standalone GitHub use, enable the GitHub source in
`stamp-c3.yaml` and replace the `external_components` entry in your copied
`packages/firmware-export.yaml` with:

```yaml
external_components:
  - source: github://0xA2C3B1F4/t150-desk-control@main
    components: [desk_firmware_export]
```

Use the same repository/ref for both components. The YAML package and its
included profile still need to be present locally; the component download does
not install those configuration files.

The package uses standard ESPHome template buttons and sensors, visible in the
stock web interface and native API. The package enables web-server version 3
and places all reader entities in a separate Firmware group. It adds no JavaScript, root-page override or
custom interface card. Keep the existing web-server authentication enabled.

## Read and download

1. Wait for fresh telemetry and settled height. Preset, timer, recovery,
   parameter and button-release transactions must have finished.
2. Press **Read firmware**. Normal UART polling, keep-awake
   and remote controls pause while the reader owns the UART.
3. When **Firmware read status** says **Waiting for desk power cycle**, power-cycle
   the desk controller while keeping the ESP32 powered. The reader waits up to
   two minutes. Opening the web page or pressing Read alone does not reset the desk.
4. Follow **Firmware read progress**, **Firmware read status** and
   **Firmware read error**. Once the read completes, open `http://<device-address>/desk-firmware/download` in the browser.
   It downloads `UploadedFlashImage.bin` using the existing web login.
   **Firmware SHA256** identifies the downloadable image. **Firmware image ready**
   stays on for a previous completed image during a new read. Check the read
   status before downloading if you need the new image.
5. **Clear firmware image** releases the RAM copy. Rebooting or updating the
   ESP32 also loses it. Downloading keeps it available for another download.

There is no native ESPHome file-download entity. The binary is served by the
component's HTTP download route; a button press runs on the device and cannot
start a download in the browser. The normal firmware has no download route.

**Cancel firmware read** and timeouts discard the partial new image and retain
the previous completed image, if any. Normal polling
resumes after a three-second loader-exit grace period; fresh settled telemetry
is required again before movement. If an interrupted transfer leaves the desk
waiting in its bootloader, power-cycle the desk again.

The header must contain a valid bounded decimal size. A new downloadable image
must pass all block size, sequence and CRC checks. The final packet is checked
in full, but padding beyond the advertised file size is not saved or hashed.
Closing-handshake failure is reported separately: a complete CRC-checked image
can still be downloaded with a warning. CRC and SHA-256 identify received data;
authenticity requires comparison to a trusted image.

## Firmware profile diagnostics

**Firmware profile** reports whether the completed image matches the known T150
application or its structure with changed scalar values. Recognition covers the
55,296-byte application and checks every byte except five extracted 32-bit
values. Other layouts, code changes and sizes remain unrecognized; they can
still be downloaded.

**Firmware profile values** shows the lower/upper bound seeds, metric display
offset, upper-correction addend and upper display value. Seeds are firmware
inputs, not centimetre travel limits. The image does not establish the active
boot-time A/B coefficients, so the reader does not calculate a usable travel
range or change the configured control profile.

Both diagnostics describe the same completed image as **Firmware SHA256** and
remain available through a new, failed or cancelled read. Clear removes them
with the image. A closing-handshake warning does not prevent recognition of
complete data. GET `/desk-firmware/status` includes these results in
`image_profile`; `control_approved` stays false, and `scalars` appears only when
extraction succeeds. Offset and display values in JSON use tenths of a centimetre.

## Memory and HTTP behavior

The reader allocates exactly the advertised size after checking the header,
before acknowledging it. It rejects zero, malformed and oversized lengths.
Allocation requires a large enough contiguous block and at least 48 KiB of
free heap remaining; the heap is checked again after allocation. The receiver
also uses about 2 KiB for packet state.

The previous completed image stays available until a new read succeeds. Clear
it first if RAM cannot hold both images plus the reserve. Progress uses the
current header size; SHA-256 and HTTP download use the completed image's exact
length. The status endpoint reports `size` for the current transfer and
`image_size` for the downloadable image. Before the header, `size` is zero.

HTTP download uses the IDF server's synchronous pointer response without a
second image-sized string. Shared ownership keeps the data alive until the
response finishes, even if Clear or a new read completes concurrently.

The component registers only GET `/desk-firmware/status` and GET
`/desk-firmware/download` through `web_server_base.add_handler`, including the
existing web authentication. Actions use standard ESPHome button entities.
The browser is not involved in UART acknowledgement timing.

## Validation

Portable C++11 tests cover ordering, bounded retries, duplicates, metadata,
partial packets, early EOT, timeouts, cancellation, UART write failure and clock
wrap. Size tests cover malformed/overflowing headers, allocation refusal, padded
final packets, mixed packet lengths and sequence-number wrap. ESPHome host tests cover exclusive ownership, busy handoff rejection,
public command guards, shutdown and fresh telemetry after release. Configuration
tests verify that only the optional package adds the reader and its entities.
Profile tests cover normalized hashing, scalar validation and rejection of
unrecognized code. The ESPHome host test uses the production SHA-256 helper;
a locally supplied reference image enables additional extraction checks.
Physical results and proprietary firmware images stay in the research project.
