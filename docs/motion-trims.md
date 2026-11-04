# Motion trims

TR01 stores four independent stopping adjustments: Normal up, Normal down,
Slow up and Slow down. Each accepts an integer from -3 to +3. Zero keeps the
original compensation. Positive values extend the target farther in the
direction of travel; negative values reduce that extension. These are controller
units, not millimetres.

The trims apply to direct height commands and presets. Switching speed selects
the corresponding pair automatically. Calibration and manual movement keep
their original behavior. Changing a trim does not move the desk.

Tune with enough travel for the desk to reach steady speed. The same trim may
leave a small positioning difference on short moves or with a different load.

Install TR01 with **Prepare motion trims** in [firmware maintenance](firmware-update.md).
Keep the panel-lock controls enabled. Then add the optional
[motion-trims package](../examples/esphome/packages/motion-trims.yaml) to an
ESPHome desk configuration. The four controls appear under **Motion trims** in
the web interface and as number entities in Home Assistant.

Each trim saves independently and survives a power cycle. The controls confirm
the saved value after each change. An invalid or missing saved value defaults
to zero for that trim; the other trims keep their values.

## Storage errors

**Default** means at least one trim uses zero because its saved value is missing
or invalid. **Verified** means all four saved values have been checked.
**Fault** or **Uncertain** means the saved values could not be confirmed.
Trim editing is unavailable during a fault; some storage faults also block
movement. Do not repeatedly submit a change to clear the status.

For a persistent storage error, stop using the desk and follow the
[wiring and recovery precautions](firmware-update.md). Switch off the controller
and disconnect the motor before a cold restart. Require healthy controller
status and a fresh Default or Verified trim read before reconnecting the motor
with power off. Complete supervised [calibration](reset-speed.md) and
read the trims again before returning to normal use. An ESPHome restart alone
does not recover the controller's storage interface.
