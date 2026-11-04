# Display timeout

**Display mode** controls the panel's height digits. **Auto blank** hides them
after **Display timeout**, which defaults to 60 seconds and accepts 1–65535 seconds.
**Normal** uses the controller's original display behavior and ignores the timeout.
Neither mode prevents the controller from sleeping.

Movement, calibration, menus, errors and timer alarms keep their normal display.
A panel press restores the display and still performs its usual action. The timer
indicator is unaffected. Playing a custom sound alone does not reset the display timeout.

Install the current [controller firmware](firmware-update.md), then add this
package after your existing desk package:

```yaml
packages:
  display: github://0xA2C3B1F4/t150-desk-control/examples/esphome/packages/display.yaml@${t150_ref}
```

For a local checkout, use `!include packages/display.yaml` instead. The controls
appear in the web page's Display group and as Home Assistant entities.

The controls show acknowledged settings. ESPHome saves the confirmed mode and
interval. At startup and when the UART connection returns, it reads the controller
and restores the saved pair once if needed. A missing reply leaves the state
unknown; it does not repeatedly send the change. Without an acknowledgement, the
previous saved choice remains the startup setting. Unsupported firmware leaves
these controls unavailable.
