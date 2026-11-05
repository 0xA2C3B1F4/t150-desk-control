# Reset and motion speed

RS01 R7 includes DT02 and CL01 and adds guarded desk calibration, a Normal/Slow
speed choice and the UART receive fix. Install it through
[firmware maintenance](firmware-update.md) and use the ESPHome adapter from
the same version. See [test coverage and known limits](validation.md).

## Enable the controls

In a [GitHub control example](esphome.md#github-configurations), add the remote
`panel_lock` and `reset_speed` packages shown there. For a local checkout's
Stamp-C3 or ESP32 example, enable both includes:

```yaml
packages:
  panel_lock: !include packages/panel-lock.yaml
  reset_speed: !include packages/reset-speed.yaml
```

Keep the `packages/` directory beside the device YAML. The maintenance example
already includes these controls. The component detects RS01 through UART;
other firmware keeps its existing controls without enabling reset or speed writes.
An unanswered RS01 discovery probe does not replace normal Control status. A
communication failure after RS01 has replied remains visible.

## Reset desk

Reset is a calibration movement, not an ESP32 restart. Stay beside the desk
with its travel clear. Unlock the panel before resetting. The controller admits
Reset only when calibration can start or continue. **Control status** shows
progress and errors.

Select **Reset** and follow **Control status**. The controller moves downward
to its reference point, then rises slightly to finish calibration. **Stop**
pauses the operation. If continuation is available, select **Reset** again to
resume the saved phase; a connection returning never resumes it automatically.
The panel remains inactive while the remote reset is paused or faulted, until
continuation or a controller restart.
When continuation is blocked, fresh controller diagnostics distinguish held
physical keys, the panel lock and other controller guards.

Custom configurations can expose `continue_reset` as a separate continuation
button and add reset/speed status and availability entities. The supplied package
uses one Reset button for both actions and the existing Control status row.

Normal movement commands are blocked during calibration. A short, renewable
controller-side command lease releases remote control if ESP32/UART communication
stops. This lease is separate from the time needed to descend from the current
height. Do not use the displayed height to judge progress during calibration.

## Motion speed

While idle, choose **Normal** for the original speed control or **Slow** for an
additional upper limit on the normal movement speed request. The controller's
existing lower limits and approach braking still apply. Reset uses its original
speed regardless of this selection.

Target-position differences were observed with both speeds. Slow upward moves
stopped below the requested height in preset and direct-target tests, including
after calibration and with longer travel. Preset values remained unchanged.

The selection changes only after a matching controller reply. It is stored in
RAM and returns to Normal when the desk controller restarts. ESPHome does not
restore or resend a saved selection at startup. These choices are not calibrated
speeds in centimetres per second.
