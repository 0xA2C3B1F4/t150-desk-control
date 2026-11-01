# Panel lock

CL01 combines DT02 direct height control, the sleep fix and a persistent lock
for the physical panel buttons. Remote controls and remote Stop remain available.
**Locked panel buttons cannot stop remote movement or perform a reset.**

## Enable and use

1. In a supplied ESP32/Stamp-C3 example, uncomment the `panel_lock` package
   include. Keep `packages/child-lock.yaml` beside the device YAML in that
   subdirectory when copying the configuration.
2. Install CL01 through [firmware maintenance](firmware-update.md). Its example
   already includes lock controls. Keep the package enabled when returning to
   the normal ESPHome build so you can unlock the panel.
3. While the desk is idle, turn **Panel lock** on to disable physical buttons,
   or off to enable them. Release all panel buttons after unlocking.

For a custom YAML, add these entities to the existing `desk_uart` block:

```yaml
  child_lock:
    name: Panel lock
  control_status:
    name: Control status
```

On first installation, an uninitialized EEPROM record starts locked with a
storage fault. Explicitly choose the desired switch state to initialize it.
Confirmed settings survive power loss; ESPHome startup never changes the lock.
A locked panel stays locked if ESPHome or UART becomes unavailable.

In local ESPHome v3 pages, the cross is red for confirmed Off and the checkmark
is green for confirmed On. The inactive button is gray. Both are gray with an
Unknown label while awaiting confirmation or after losing the state. Explicit
commands remain available for recovery attempts.

## Status and recovery

The switch changes only after a matching controller reply. Home Assistant may
retain its previous value during a disconnect. Check **Control status** for an
unknown state, a pending lock command or a storage fault before relying on it.
Lock commands still require idle controls and fresh telemetry.
The optional `child_lock_status` and `child_lock_available` entities remain
available for custom configurations and automations.

A timeout does not confirm a change. Read-only status queries retry while idle,
but lock/unlock writes never retry automatically. After a storage fault, the
state after a power cycle is uncertain. Explicitly retry the desired setting
while idle to verify and repair the stored record.

For automations, use the confirmed switch state; `command_result` may arrive
before it. Restoring DT02 or the original firmware removes the lock feature,
but retains its EEPROM record for a later CL01 installation.

See [test coverage](validation.md#hardware-validation) and the
[CL01 technical reference](protocol.md#cl01-panel-lock-extension).
