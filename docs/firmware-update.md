# Firmware maintenance

The optional maintenance build serves `/desk-firmware` on the ESPHome device. It reads the controller application, prepares DT02 or CL01, installs a validated image, and verifies every application byte by a separate UART readback. It is not part of the normal desk configuration.

Original-image restoration, DT02 patching, installation and full readback have been tested through the browser on Stamp-C3. A power-interrupted write with a partially programmed block was recovered over UART and verified by full readback. Partial-block recovery was tested through the maintenance API. Browser tests also verified recovery and retry after cancelling a write.

## Build and open

Run from the checkout root, with `secrets.yaml` beside the example YAML files:

```sh
esphome run examples/esphome/stamp-c3-firmware-update.yaml
```

Use the **Open firmware maintenance** link in the main page's **Firmware** group, or go directly to `http://<device-address>/desk-firmware`. It uses the normal web-server authentication. `web_server.auth` is required. Open the device IP address or its `.local` name. Use a trusted local network.

Both pages use a left-aligned layout and expand to the available width at 1024 px and below. With local ESPHome v3 and gzip compression (tested with 2026.4.2), a small local script places the link in the package's **Firmware** group. It stays at the bottom of the page if the group is unavailable. No external assets or new browser libraries are needed. Other web-server configurations keep the standard main page; use the direct maintenance URL. Reload the page after updating the ESP32.

The example includes `stamp-c3.yaml`, `packages/firmware-update.yaml` and
`packages/child-lock.yaml`. The last package provides remote unlocking after CL01
installation; keep it enabled in the normal build if you install CL01. For Device Builder, copy these files and `secrets.yaml` together, preserving the package subdirectory. Replace both local external-component sources with the same GitHub revision, listing `desk_uart` in the base YAML and `desk_firmware_export, desk_firmware_update` in the maintenance package. For a relocated local checkout, change the base YAML's source path and override `firmware_components_path` in the main YAML. Both paths resolve from the main device YAML, not the package.

Use this package instead of `firmware-export.yaml`; both cannot own UART. The read-only package remains available separately. Maintenance disables ESP32 OTA to avoid interrupting a controller write; return to the normal ESPHome build over USB.

## Read, patch and verify

A highlighted instruction stays at the top of the viewport as you scroll. It tells you when to turn controller power **OFF, then ON**, when to leave power connected, and which action comes next. The current confirmations, controls, transfer progress, timeout and feedback are shown in the same panel. Long receipt identifiers wrap within it. Each new power instruction is brought into view. Keep the ESP32 powered throughout. If status cannot be read, the panel marks the state as unknown and disables actions until it reconnects; keep both devices powered.

1. With controller power off, disconnect the motor cable. Keep the ESP32 powered and UART connected. Confirm motor isolation and enter maintenance.
2. Select **Read controller firmware**, then power-cycle only the controller once. Wait for `read complete`.
3. Download the original backup and confirm that it is saved on your computer. Browser memory does not survive closing the page.
4. Select **Prepare DT02 patch** for direct height control and the sleep fix, or **Prepare panel lock (CL01)** to include CL01. You can also select a saved application file and **Prepare selected file**. Download the prepared image if needed. CL01 initially locks physical buttons, including local stopping; configure the [Panel lock controls](child-lock.md) before installation.
5. Confirm installation, select **Arm installation**, then power-cycle only the controller once. Leave power connected throughout writing.
6. At `readback required`, select **Verify by full readback** and power-cycle the controller once more. Only `verified` means all application bytes and the closing handshake matched.
7. Select **Finish maintenance** after verification, or after saving a read without installing anything. UART ownership is released. Turn only the controller off and on once more to start its application. Reconnect the motor only with controller power off.

Maintenance keeps UART ownership through browser disconnects. A new, duplicated, or reloaded tab must explicitly take over an idle session. After reloading during a transfer, wait until it finishes or times out before taking over; the new tab cannot cancel that transfer. There are no automatic write retries, abort bytes, software resets or resumed movement requests.

## Accepted images and storage

Reads use the loader's header length, bounded to 128 KiB and available RAM. Writing accepts only the exact original, DT02 or CL01 55,296-byte application. Other revisions, including scalar-only variants, need their own reviewed patch; profile recognition alone never permits writing.

| Image | Application SHA-256 |
| --- | --- |
| Original | `bf52d596c81d6d499480a9dd3173e6991de5ee0c72364e0b53bc91ea89053faf` |
| DT02 | `e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff` |
| CL01, DT02 with panel lock | `81933d93b8bc28ef0eb8b29bf7ae1c7b9104f0a8e03c1c2f8096d509de0f75d3` |

A fresh read identifies an already installed DT02 or CL01 image. Preparing a patch does not mean it has been installed. To remove the panel lock, select a saved DT02 or original application and complete installation and verification. Removing CL01 does not erase its saved lock state.

The ESP32 independently checks hashes. It patches one buffer in place after backup confirmation, or receives a complete selected file in bounded chunks before arming a write. Allocation requires a contiguous block and retains 48 KiB of free heap. Verification compares against the staged buffer without a second image allocation. The browser holds temporary copies and saves downloads to the computer. No controller firmware image is distributed here.

The pinned loader erases 53 application pages, writes 25 blocks for original/DT02 or 26 for CL01, and leaves the final flash page untouched. Admission requires that page to be erased in a fresh read. Verification covers all 54 KiB, including the unwritten tail. The first 10 KiB bootloader is neither exported nor verified.

## Interrupted installation

A recovery record containing source and target block fingerprints is saved on the ESP32 before installation. After failure or ESP32 restart, desk commands remain paused. Read the controller again, save that read, select the known original backup, prepare it, and explicitly arm restoration. A partial image is accepted only after installation was armed, the final page is erased, and every 1 KiB block matches the recorded source, target, or an erased block. If an original-image
recovery is itself interrupted, a third set of original-image block fingerprints
is retained alongside the source and target. One partially programmed block within the recorded image's write payload can also be recovered. The updater retains that block until the exact original file has been uploaded and hashed. It reconstructs the original, DT02 and CL01 candidates from that file, checks each candidate against its recorded identity and block hash, and requires a matching halfword-aligned prefix followed by erased bytes. Only the original image can be installed through this path. More than one partial block, unexpected programmed bytes, a partially programmed halfword or damage to the untouched last page still requires external recovery. Unknown images without that history cannot be written. A full read matching a complete image recorded for the attempt also clears the recovery requirement without another write.

Do not clear ESP32 preferences or replace its firmware during unresolved installation: that can lose the recovery marker. Keep the computer backup; external recovery may be necessary. The UI shows the five-minute preflight countdown; expiration requires another read before preparing a write.

## Unknown versions

The optional `research_url` connects to the existing private firmware inbox. **Send firmware for research** requires a downloaded image and separate consent. The browser sends it directly over HTTPS without ESPHome credentials. The operator stores it privately until removal. A receipt confirms storage, not analysis or patch approval. Upload is never automatic, and the service cannot enable an unreviewed patch on the ESP32. Remove `research_url` to hide this action.

## Reproduce the patch code

The replacement assembly is in `tools/patches/dt02.S`. With LLVM installed, run from the checkout root:

```sh
python3 tools/verify_dt02_assembly.py --llvm-bin /path/to/llvm/bin
```

Set `TMPDIR` to a writable temporary directory. This rebuilds only our hook and replacement code, and checks their bytes against the embedded patch. It neither needs nor distributes a controller image. The separate sleep fix replaces the four bytes at application offset `0x1656`; the complete patched application must still match the DT02 hash above.

For the CL01 source and standalone builder, see [panel lock](child-lock.md).
Verify its generated replacement-only header with
`python3 tools/embed_child_lock_patch.py --check --llvm-bin /path/to/llvm/bin`.

UI state checks can be run from the checkout root with `node tests/esphome/firmware_ui_test.cjs` (Node.js 18 or later). They simulate status and HTTP responses without contacting a device.
