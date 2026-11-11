# Firmware maintenance

The optional maintenance build reads, patches, installs and verifies the
controller application through UART. The normal desk configuration excludes it.
No manufacturer firmware image is distributed.

## Build and open

Use ESPHome 2026.10.1. For Device Builder or a standalone YAML, copy
[the GitHub maintenance example](../examples/esphome/github/stamp-c3-firmware-update.yaml)
and keep your `secrets.yaml` beside it. Preserve the installed device name and
credentials. The example fetches all packages and components at `t150_ref`.

For a local checkout, with `secrets.yaml` beside the example YAMLs, run:

```sh
esphome run examples/esphome/stamp-c3-firmware-update.yaml
```

Open **Open firmware maintenance** in the main page's Firmware group, or
`http://<device-address>/desk-firmware`. Use the normal web login on your local
network; `web_server.auth` is required. Reload after updating the ESP32.

The local example includes `stamp-c3.yaml` and the `firmware-update.yaml`,
`panel-lock.yaml` and `reset-speed.yaml` files under `packages/`. Keep that layout
and place `secrets.yaml` beside the device YAML when copying a local configuration.
For a moved local checkout, update the base source and `firmware_components_path`;
both resolve from the main device YAML. See [configuration paths](esphome.md#configuration-paths).

Use this package instead of `firmware-export.yaml`. ESPHome OTA stays available
between controller operations. Reads, uploads, writes, verification, queued actions
and the three-second transfer exit period block OTA before it starts. An OTA
transfer blocks new maintenance actions until it ends or fails. An idle maintenance
session or a saved recovery record does not block OTA. Only `platform: esphome` is
supported with firmware maintenance. `captive_portal` also enables web-server OTA
and cannot be combined with this build. A rejected OTA attempt logs
`OTA refused: firmware operation active or queued`; retry after the operation ends.
The OTA guard checks the reviewed ESPHome source. If a different ESPHome version
fails with `Unreviewed ESPHome OTA source`, use the supported version above.
Keep the [panel-lock package](panel-lock.md) enabled for remote lock controls.
Firmware before the local panel lock version (LK01) requires these controls to
unlock the panel. See [firmware versions](#firmware-versions) for each code.

## Read, patch and verify

The calibration and speed firmware (RS01 R7) requires the ESPHome adapter from
the same version for UART frame recovery.
See [test coverage and known limits](validation.md).

Follow the active instruction panel. Keep the ESP32 powered throughout.

1. With controller power off, disconnect the motor. Keep UART connected,
   confirm motor isolation and enter maintenance.
2. Select **Read controller firmware**, then power-cycle only the controller.
   Wait for `read complete`.
3. Download the backup and confirm it is saved on your computer. Closing the
   page loses its browser-memory copy.
4. Select **Prepare current firmware**. This includes direct height control,
   panel locking with local unlocking and Stop, calibration, Normal/Slow speeds,
   timer and buzzer controls,
   [motion trims](motion-trims.md), and [display timing](display.md). The page shows the target version.
   If it is already installed, the same action prepares a reinstall.
   To restore an older supported image, use **Prepare selected file**.
   Without a valid saved lock setting, the panel starts locked. Use
   [M + T](panel-lock.md#use-the-panel) or the remote controls to unlock it.
   Local Stop remains available while locked in LK01.
5. Confirm and select **Arm installation**, then power-cycle only the controller.
   Leave power connected throughout writing.
6. At `readback required`, select **Verify by full readback** and power-cycle
   the controller again. Only `verified` confirms every application byte and
   the closing handshake.
7. Select **Finish maintenance**, then power-cycle the controller to start its
   application. Reconnect the motor only with controller power off.

If the page keeps waiting for a power cycle, leave controller power off for
30 seconds before turning it back on. Keep the ESP32 powered.

If the browser disconnects, the ESP32 continues the transfer. Reopen the page
and wait for completion or timeout before taking over the idle session. A new
tab cannot cancel the previous tab's transfer. Unknown status disables actions;
keep both devices powered until status returns. Writes never retry automatically.

## Firmware versions

The maintenance page, example files and test notes name controller firmware
versions by short codes. Each version includes everything in the rows above it.
**Prepare current firmware** installs LK01.

| Code     | Name                     | Adds                                                                                | Guide                                       |
| -------- | ------------------------ | ----------------------------------------------------------------------------------- | ------------------------------------------- |
| Original | Original firmware        | The manufacturer's T150-PJ-V1.5 application, without patches                        | [Hardware](hardware.md)                     |
| DT02     | Direct height            | Direct height commands and the [sleep fix](validation.md#controller-firmware-fixes) | [Direct backend](esphome.md#direct-backend) |
| CL01     | Panel lock               | Remote panel locking; the lock survives power loss                                  | [Panel lock](panel-lock.md)                 |
| RS01 R7  | Calibration and speed    | Guarded calibration, Normal/Slow speeds and the UART receive fix                    | [Reset and motion speed](reset-speed.md)    |
| TB01     | Timer and buzzer         | Custom timer and buzzer commands                                                    | [Timer and buzzer](custom-timer-buzzer.md)  |
| FP01     | Shared firmware platform | A shared base for the later patches                                                 | [Controller firmware](firmware-platform.md) |
| TR01     | Motion trims             | Stopping adjustments for each speed and direction                                   | [Motion trims](motion-trims.md)             |
| BZ01     | Sound profiles           | Buzzer volume, 5 ms pulses and a separate timer alarm sound                         | [Timer and buzzer](custom-timer-buzzer.md)  |
| DS01     | Display timeout          | Blanks the idle height digits after a set time                                      | [Display timeout](display.md)               |
| LK01     | Local panel lock         | Locking and unlocking with M + T on the panel, and physical Stop while locked       | [Panel lock](panel-lock.md#use-the-panel)   |

## Accepted images and storage

Writing accepts only the exact original, DT02, CL01, RS01 R7, TB01, FP01, TR01,
BZ01, DS01 and LK01 applications. **Prepare current firmware** accepts all ten
as starting points.
The current image requires the documented STM32F030C8T6 controller with 8 KiB
SRAM. A fresh read identifies the installed image.
Other images need a separately reviewed patch; profile recognition alone does
not authorize writing.

Restoring DS01 or an earlier image removes local unlocking and the new local
Stop behavior. Keep remote lock controls enabled when reverting.
To remove trims, install and verify a saved FP01 or earlier supported image.
The saved trim bytes remain available for a later TR01 installation.
To remove FP01, install and verify a saved TB01, R7, CL01, DT02 or original image.
To remove custom timer and buzzer support as well, choose R7 or earlier.
To remove reset and speed support, install a saved CL01, DT02 or original image.
To remove panel locking as well, install and verify your saved original or DT02 application.
Its EEPROM lock record remains for a later CL01 installation.

Before reverting the ESP32 software to an older project version, use the current
maintenance build to restore and verify a controller image that version supports.
Finish any recovery first; an older build may not recognize newer images or their
recovery records.

The ESP32 validates and stages the complete image in RAM before writing,
retaining 48 KiB of free heap. The browser can save backups and prepared images
to your computer. Readback covers all 55,296 application bytes; it excludes the
first 10 KiB bootloader, EEPROM and option bytes. See the
[maintenance reference](protocol.md#firmware-maintenance) for exact hashes.

## Interrupted installation

See [validation](validation.md#hardware-validation) for tested recovery paths.

Keep the ESP32's current firmware and preferences intact; they contain the
recovery record. Read the controller again, save that read, select the known
original backup, prepare it and explicitly arm restoration. Follow with full
readback. If restoration is interrupted, repeat this process.

Recovery accepts only damage consistent with the saved installation record,
including at most one partially programmed block. If it rejects the image, stop
and preserve the backup and ESP32 state for investigation; external recovery may
be necessary. A complete matching image can resolve the recovery state without
rewriting. A five-minute preflight timeout requires a fresh read before preparing
another write.

## Unknown versions

An optional `research_url` enables **Send firmware for research**. After you save
a backup and consent, the browser sends the image over HTTPS to a private inbox,
without ESPHome credentials. A receipt confirms storage, not patch approval.
The inbox retains uploads until deletion. Upload is never automatic; omit the URL
to hide it. The service cannot enable an unreviewed patch on the ESP32.

See [compatibility research and submission details](hardware.md#help-develop-support)
for how contributed firmware can help extend support to other controllers.

Before downgrading ESPHome, restore and verify a controller image that the older
ESPHome version supports. Do not downgrade ESPHome with an unfinished recovery;
the older version may not recognize its saved recovery record.
