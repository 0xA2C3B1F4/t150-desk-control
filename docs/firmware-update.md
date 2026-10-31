# Firmware maintenance

The optional maintenance build reads, patches, installs and verifies the
controller application through UART. The normal desk configuration excludes it.
No manufacturer firmware image is distributed.

## Build and open

From the checkout root, with `secrets.yaml` beside the example YAMLs, run:

```sh
esphome run examples/esphome/stamp-c3-firmware-update.yaml
```

Open **Open firmware maintenance** in the main page's Firmware group, or
`http://<device-address>/desk-firmware`. Use the normal web login on your local
network; `web_server.auth` is required. Reload after updating the ESP32.

The example includes `stamp-c3.yaml`, `packages/firmware-update.yaml` and
`packages/child-lock.yaml`. Keep those files and `secrets.yaml` in the same layout
when copying. For Device Builder, replace both local component sources with the
same GitHub revision: `desk_uart` in the base YAML and
`desk_firmware_export, desk_firmware_update` in the maintenance package.
For a moved local checkout, update the base source and `firmware_components_path`;
both resolve from the main device YAML. See [configuration paths](esphome.md#configuration-paths).

Use this package instead of `firmware-export.yaml`. Maintenance disables ESP32
OTA; return to the normal build over USB. After installing CL01, keep the
[panel-lock package](child-lock.md) enabled so you can unlock the panel.

## Read, patch and verify

Follow the active instruction panel. Keep the ESP32 powered throughout.

1. With controller power off, disconnect the motor. Keep UART connected,
   confirm motor isolation and enter maintenance.
2. Select **Read controller firmware**, then power-cycle only the controller.
   Wait for `read complete`.
3. Download the backup and confirm it is saved on your computer. Closing the
   page loses its browser-memory copy.
4. Select **Prepare DT02 patch** for direct height control and the sleep fix,
   or **Prepare panel lock (CL01)** for both plus persistent panel locking.
   Alternatively, select a saved application and **Prepare selected file**.
   CL01 initially locks physical buttons, including local stopping; configure
   its controls before installation.
5. Confirm and select **Arm installation**, then power-cycle only the controller.
   Leave power connected throughout writing.
6. At `readback required`, select **Verify by full readback** and power-cycle
   the controller again. Only `verified` confirms every application byte and
   the closing handshake.
7. Select **Finish maintenance**, then power-cycle the controller to start its
   application. Reconnect the motor only with controller power off.

If the browser disconnects, the ESP32 continues the transfer. Reopen the page
and wait for completion or timeout before taking over the idle session. A new
tab cannot cancel the previous tab's transfer. Unknown status disables actions;
keep both devices powered until status returns. Writes never retry automatically.

## Accepted images and storage

Writing accepts only the exact supported original, DT02 and CL01 applications.
CL01 can be prepared from the original or DT02. A fresh read identifies an
already installed patch. Other revisions need a separately reviewed patch;
profile recognition alone does not authorize writing.

To remove CL01, install and verify your saved original or DT02 application.
Its EEPROM lock record remains for a later CL01 installation.

The ESP32 validates and stages the complete image in RAM before writing,
retaining 48 KiB of free heap. The browser can save backups and prepared images
to your computer. Readback covers all 55,296 application bytes; it excludes the
first 10 KiB bootloader, EEPROM and option bytes. See the
[maintenance reference](protocol.md#firmware-maintenance) for exact hashes.

## Interrupted installation

An interrupted write has been recovered over UART and verified by full readback.
For coverage and remaining limits, see [validation](validation.md#hardware-validation).

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
The operator retains it until removal. Upload is never automatic; omit the URL
to hide it. The service cannot enable an unreviewed patch on the ESP32.
