# Stamp-C3 distribution

`stamp-c3.yaml` builds the USB factory image with Improv Serial, device-generated
credentials and a local web interface. It shares its desk controls with the
normal Stamp example. Follow [binary installation](../docs/binary-install.md)
for USB setup, Home Assistant and updates.

Development builds use the ESPHome version in `requirements-dev.txt` and the
native ESP-IDF toolchain. Put build output outside the checkout:

```sh
esphome -s build_directory "${TMPDIR:?}/t150-distribution" compile distribution/stamp-c3.yaml
```

Release images are built once from the selected clean main commit and retained
through testing and publication. Use the [release workflow](../docs/releases.md).
