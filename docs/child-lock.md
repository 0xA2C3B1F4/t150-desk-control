# Panel lock

CL01 adds a persistent lock for the seven physical panel buttons to the supported
DT02 firmware. Remote controls and remote Stop continue to work. **While locked,
physical buttons cannot stop remotely started movement or perform a reset.**
This controls panel input, not access to the UART or network interface.

## Enable and use

For the supplied ESP32/Stamp-C3 examples, uncomment the `panel_lock` package
include near the top of the device YAML. Keep `packages/child-lock.yaml` beside
the YAML in that subdirectory when copying it elsewhere. The firmware-maintenance
example includes these controls so an installed CL01 can be unlocked. Keep the
package enabled when returning to the normal build.

Alternatively, add these entities under `desk_uart` in your own device YAML:

```yaml
desk_uart:
  # Keep your existing UART and height settings.
  child_lock:
    name: Panel lock
  child_lock_status:
    name: Panel lock status
  child_lock_available:
    name: Panel lock available
```

Install CL01 through [firmware maintenance](firmware-update.md). It includes
DT02 direct height control and the sleep fix. The component separately probes
for CL01; a DT02 response alone does not enable the lock. The switch sends no
lock or unlock command at startup. With this package enabled, a failed identity
or DT02 prerequisite check is retried every five seconds; a conclusive stock
result is not. This is separate from target-backend auto detection.

Switch **Panel lock** on to disable physical buttons, or off to enable them.
Change it while the desk is idle. The controller rejects changes during motion
or an unfinished button action. After unlocking, release all physical buttons
before pressing one again. The release check uses one GPIO sample, so contact
bounce while a button is held has not been ruled out.

On first installation, an uninitialized EEPROM record starts locked and reports
a storage fault. Choose the desired switch state explicitly to write and verify
the record. Successful lock and unlock states survive controller power cycles.
If ESPHome or UART is unavailable, a locked panel remains locked.

The switch waits for a matching controller reply. For automations that depend on
the lock state, trigger from the confirmed switch state; `command_result` is a
diagnostic and can arrive first. Home Assistant may retain its
last confirmed on/off value while disconnected. The availability sensor is true
only when the state is confirmed and the desk is ready for another lock request.
It is false during movement, other operations or a storage fault. The status text
explains the reason; a storage fault can still be resolved by explicitly retrying
the desired setting while idle. A timeout is not confirmation
that the requested state was applied. A storage fault means the state after a
power cycle is uncertain, even when physical buttons are currently blocked.
With DT02 confirmed, the component queries CL01 about every five seconds while
idle, including after a missed or invalid reply. It never repeats a lock/unlock
write automatically. Once
CL01 is confirmed again, retry the desired setting explicitly; reading status
alone does not verify EEPROM contents. Restoring DT02 or the original
firmware removes the panel lock. Its EEPROM record remains, so reinstalling CL01
loads the previously stored state.

## Validation

The exact CL01 R3 image was installed and verified by a complete 55,296-byte UART
readback. A supervised device test confirmed blocked physical buttons, remote
movement and Stop while locked, persistence of both states through power cycles,
and working physical controls after unlocking. EEPROM brownouts during a write,
contact bounce and exhaustive button combinations have not been tested on hardware.

The offline suite executes the patched Thumb code and includes 1,536 interrupted
write cases. EEPROM transport and board initialization are modeled; those tests
do not establish electrical behavior.

## Patch source

The builder accepts only the exact saved DT02 full or application image and
verifies the generated CL01 hash. It changes the GPIO reader, boot initialization
call and UART dispatcher, plus reserved patch code. It preserves the first
10 KiB bootloader. The source is [child_lock.S](../tools/patches/child_lock.S);
no manufacturer firmware image is distributed.

CL01 uses eight RAM bytes at `0x20000FC8` through `0x20000FCF`, above the original
stack start, and an erased 2 KiB code reservation at `0x08008C00`. Its GPIO hook
suppresses PA8 through PA12 and PB14 through PB15 only. UART overrides retain
their stock path. Physical keys may still wake the controller without performing
an action. Lock changes clear pending scanner/handler events only after idle
checks, under a saved interrupt mask.

## Persistence contract

EEPROM bytes `0x60` and `0x61` hold an exact complementary record. `A5 5A` means
locked, `5A A5` means unlocked, and every other pair means locked with a storage
fault. The stock image reads those addresses into unused startup cache fields;
its save paths write `0x00` through `0x2F` or `0x40` through `0x5F` and do not
overwrite the CL01 record.

A changing request first locks RAM and enables the physical release gate. It
then writes byte `0x60` as zero and verifies it twice, writes the desired second
byte and verifies it twice, verifies the first byte is still zero, and finally
commits the first byte. The complete record must read back identically twice
before success. Stock writer `0x08004938` takes value in R0 and address in R1;
reader `0x0800488C` takes address in R0. Each write is followed by stock delay
`0x08007B18(3)`. That delay polls a hardware timer counter and does not depend on
SysTick interrupts. The patch saves and restores PRIMASK around the operation.
Interrupts remain masked through the idle/protection checks, up to three writes,
their delays and up to fourteen reads. The stock I2C timeouts count polling-loop
iterations. Worst-case interrupt latency, actual timer duration, EEPROM write
timing and false storage faults have not been measured on hardware.

Power loss after invalidation and before final commit leaves an invalid record,
which boots locked. Power loss before the first write can retain the previously
committed state; no success reply has been sent. A same-state request rereads the
record twice and avoids writes when both RAM and durable state already match.
GET performs no EEPROM I/O.

A readback failure keeps RAM locked and reports `storage_fault`. The persistent
contents are uncertain after a reported failure. A failed LOCK may retain an
older unlocked record if invalidation never lands. A failed UNLOCK may leave a
complete unlocked record if the final write lands but readback fails; a later
reset then loads that unlocked record. Status 3 never proves a persistent lock.
GET reports cached runtime state and cannot resolve this uncertainty. Repeating
the desired SET rereads and verifies the persistent record before success.
The stock I2C routines have no reliable error result, and repeated reads do not
prove immunity to arbitrary bus
corruption. The offline tests model byte writes and interrupted operations;
physical brownout behavior and EEPROM endurance still need device validation.

## UART protocol

The normal XOR/escaping frame format applies. Byte offsets below refer to the
decoded body without checksum or delimiters.

Request body is five bytes: `D3 01 op nonce_hi nonce_lo`. Operations are `00` GET,
`01` LOCK and `02` UNLOCK. There is no retry behavior in the firmware extension.
Malformed length/checksum frames or insufficient TX queue space cause no
mutation and no reply. Invalid version or operation returns status `01`.

Reply body is fifteen bytes:

| Offset | Meaning |
| --- | --- |
| 0 through 1 | `D4 01` |
| 2 | Requested operation |
| 3 | Status: `00` success, `01` bad request, `02` busy, `03` storage failure |
| 4 | Effective lock, 0 or 1 |
| 5 | Physical release gate, 0 or 1 |
| 6 | Storage fault, 0 or 1 |
| 7 through 10 | ASCII `CL01` |
| 11 through 12 | Echoed nonce, big-endian |
| 13 through 14 | Controller sequence, big-endian |

[child_lock_protocol.py](../tools/child_lock_protocol.py) constructs requests and
decodes supplied replies without opening a device. The nonce binds a reply to
a request and provides no authentication.

Run these offline commands from the checkout root:

```sh
python3 tools/child_lock_protocol.py request get --nonce 0x1234
python3 tools/child_lock_protocol.py request lock --nonce 0x1235
python3 tools/child_lock_protocol.py request unlock --nonce 0x1236
python3 tools/child_lock_protocol.py reply '<captured reply hex>' --nonce 0x1236
```

## Reproduce

Run from the checkout root with LLVM installed and `TMPDIR` pointing to a writable
temporary directory. Output paths must not already exist.

```sh
python3 tools/verify_dt02_assembly.py --llvm-bin /path/to/llvm/bin
python3 tools/build_child_lock_patch.py /path/to/saved-dt02.bin \
  /path/to/new-cl01.bin --manifest /path/to/new-cl01.json \
  --llvm-bin /path/to/llvm/bin
PYTHONDONTWRITEBYTECODE=1 python3 tools/test_child_lock_builder.py
```

The builder accepts either a 65,536-byte full image or its 55,296-byte application.
Application hashes are listed in [firmware maintenance](firmware-update.md).
To run the firmware emulator tests, install Unicorn 2.1.4 in a virtual environment
or a dependency directory under `TMPDIR`, then use the full images:

```sh
python3 tools/test_child_lock_patch.py /path/to/saved-full-dt02.bin \
  /path/to/new-full-cl01.bin /path/to/new-cl01.json --output /path/to/new-report.json
```
