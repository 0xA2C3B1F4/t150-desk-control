# Panel lock

Panel lock disables ordinary panel controls while keeping remote control available.
The current controller firmware, LK01, also lets you lock, unlock and stop movement
from the panel without ESPHome or a network connection. The lock setting survives
power loss and is shared with the web interface and Home Assistant.

## Use the panel

With the desk stationary, press **M + T**. Press the second button within
1.5 seconds of the first, then hold both together for three seconds. The same
combination locks or unlocks the panel. Release all buttons before using it again.
If the panel is asleep, wake it with a brief press and release before starting
the combination. Ordinary M and T functions remain available when unlocked.

The display shows `Loc` after locking and `UnL` after unlocking. Watch for this
brief feedback while holding the buttons. `bSY` means the change was blocked;
release the buttons and try again when idle. `Err` means the lock setting could
not be saved. The panel remains locked after a failed save.

While locked, pressing any panel button stops movement without unlocking the
panel. New movement is blocked until all buttons are released, including remote
requests. A press used to stop movement cannot also unlock the panel; release it
and use M + T again.

Earlier firmware requires remote control to unlock the panel. Its locked buttons
cannot stop ordinary remote height moves. Install LK01 for local operation.

## Enable web and Home Assistant controls

1. In a [GitHub control example](esphome.md#github-configurations), add the remote
   `panel_lock` package. For a local checkout example, uncomment its `panel_lock`
   include and keep the `packages/` directory beside the YAML.
2. Install the current controller firmware through
   [firmware maintenance](firmware-update.md). Its example includes lock controls.
3. While the desk is idle, turn **Panel lock** on or off. Release all panel buttons
   after unlocking. A panel change appears in the controls after the next status
   query, normally within five seconds while UART is connected.

For a custom YAML, add these entities to the existing `desk_uart` block:

```yaml
  panel_lock:
    name: Panel lock
  control_status:
    name: Control status
```

For older configurations, rename `child_lock`, `child_lock_status` and
`child_lock_available` to their `panel_lock` equivalents. The old keys still work
with a configuration warning; do not set both names for the same option.
Replace package references to `child-lock.yaml` with `panel-lock.yaml`.

An uninitialized EEPROM record starts locked with a storage fault. Use M + T to
unlock locally, or explicitly choose the desired state in the remote controls.
ESPHome startup does not change the saved lock setting.

## Status and recovery

The switch changes only after a matching controller reply. Home Assistant may
retain its previous value during a disconnect. Check **Control status** before
relying on that value. Remote writes require idle controls and fresh telemetry;
status queries retry, but writes never retry automatically.

A timeout does not confirm a change. A failed write leaves the panel locked, but
the saved state is uncertain. Explicitly retry the desired setting while idle to
verify and repair the record. For automations, use the confirmed switch state;
`command_result` may arrive before it.

Restoring DT02 or the original firmware removes the lock feature but retains its
EEPROM record. Restoring a firmware version before LK01 removes local unlocking
and the new local Stop behavior; keep remote lock controls available.

See [test coverage](validation.md), the
[CL01 protocol](protocol.md#cl01-panel-lock-extension) and its optional diagnostic
entities `panel_lock_status` and `panel_lock_available`.
