# Firmware maintenance

The optional maintenance build serves `/desk-firmware` on the ESPHome device. It reads the controller application, prepares DT02, installs a validated image, and verifies every application byte by a separate UART readback. It is not part of the normal desk configuration.

The interface has host simulation and Stamp-C3 build coverage. Its complete browser-to-controller workflow still needs supervised hardware validation. Keep an ST-Link recovery route available when testing it.

## Build and open

Run from the checkout root, with `secrets.yaml` beside the example YAML files:

```sh
esphome run examples/esphome/stamp-c3-firmware-update.yaml
```

Open `http://<device-address>/desk-firmware`. It uses the normal web-server authentication. `web_server.auth` is required. Open the device IP address or its `.local` name. Use a trusted local network.

The example includes `stamp-c3.yaml` and `packages/firmware-update.yaml`. For Device Builder, copy these files and `secrets.yaml` together, preserving the package subdirectory. Replace both local external-component sources with the same GitHub revision, listing `desk_uart` in the base YAML and `desk_firmware_export, desk_firmware_update` in the maintenance package. For a relocated local checkout, change the base YAML's source path and override `firmware_components_path` in the main YAML. Both paths resolve from the main device YAML, not the package.

Use this package instead of `firmware-export.yaml`; both cannot own UART. The read-only package remains available separately. Maintenance disables ESP32 OTA to avoid interrupting a controller write; return to the normal ESPHome build over USB.

## Read, patch and verify

1. With controller power off, disconnect the motor cable. Keep the ESP32 powered and UART connected. Confirm motor isolation and enter maintenance.
2. Select **Read controller firmware**, then power-cycle only the controller once. Wait for `read complete`.
3. Download the original backup and confirm that it is saved on your computer. Browser memory does not survive closing the page.
4. Select **Prepare DT02 patch**, or select a saved application file and **Prepare selected file**. You can download the prepared image too.
5. Confirm installation, select **Arm installation**, then power-cycle only the controller once. Leave power connected throughout writing.
6. At `readback required`, select **Verify by full readback** and power-cycle the controller once more. Only `verified` means all application bytes and the closing handshake matched.
7. Select **Finish maintenance** after verification. Normal UART operation resumes. Reconnect the motor only with controller power off.

Maintenance keeps UART ownership through browser disconnects. A second tab must explicitly take over an idle session. There are no automatic write retries, abort bytes, software resets or resumed movement requests.

## Accepted images and storage

Reads use the loader's header length, bounded to 128 KiB and available RAM. Writing accepts only the exact original or DT02 55,296-byte application. Other revisions, including scalar-only variants, need their own reviewed patch; profile recognition alone never permits writing.

| Image | Application SHA-256 |
| --- | --- |
| Original | `bf52d596c81d6d499480a9dd3173e6991de5ee0c72364e0b53bc91ea89053faf` |
| DT02 | `e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff` |

The ESP32 independently checks hashes. It patches one buffer in place after backup confirmation, or receives a complete selected file in bounded chunks before arming a write. Allocation requires a contiguous block and retains 48 KiB of free heap. Verification compares against the staged buffer without a second image allocation. The browser holds temporary copies and saves downloads to the computer. No controller firmware image is distributed here.

The pinned loader erases 53 application pages, writes 25 blocks, and leaves the final flash page untouched. Admission requires that page to be erased in a fresh read. Verification covers all 54 KiB, including the unwritten tail. The first 10 KiB bootloader is neither exported nor verified.

## Interrupted installation

A recovery record containing source and target block fingerprints is saved on the ESP32 before installation. After failure or ESP32 restart, desk commands remain paused. Read the controller again, save that read, select the known original backup, prepare it, and explicitly arm restoration. A partial image is accepted only after installation was armed, the final page is erased, and every 1 KiB block matches the recorded source, target, or an erased block. Other partial blocks require external recovery. Unknown images without that history cannot be written. A clean read matching the recorded original or intended complete target also clears the recovery requirement without another write.

Do not clear ESP32 preferences or replace its firmware during unresolved installation: that can lose the recovery marker. Keep the computer backup; external recovery may be necessary. The UI shows the five-minute preflight countdown; expiration requires another read before preparing a write.

## Unknown versions

The optional `research_url` connects to the existing private firmware inbox. **Send firmware for research** requires a downloaded image and separate consent. The browser sends it directly over HTTPS without ESPHome credentials. The operator stores it privately until removal. A receipt confirms storage, not analysis or patch approval. Upload is never automatic, and the service cannot enable an unreviewed patch on the ESP32. Remove `research_url` to hide this action.

## Reproduce the patch code

The replacement assembly is in `tools/patches/dt02.S`. With LLVM installed, run from the checkout root:

```sh
python3 tools/verify_dt02_assembly.py --llvm-bin /path/to/llvm/bin
```

Set `TMPDIR` to a writable temporary directory. This rebuilds only our hook and replacement code, and checks their bytes against the embedded patch. It neither needs nor distributes a controller image. The separate sleep fix replaces the four bytes at application offset `0x1656`; the complete patched application must still match the DT02 hash above.
