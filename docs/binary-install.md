# Install the Stamp-C3 binary

The release factory image is for the M5Stack Stamp-C3 C056-B with 4 MB flash
and the [tested DT410 controller](hardware.md#desk-and-controller-compatibility).
It includes the normal Stamp controls, a local web interface and Home Assistant
support. You do not need a compiler or YAML edits.

Use [YAML installation](../README.md#esphome-device-builder) for other pin layouts,
firmware maintenance, optional controller-extension controls or OTA updates.
The factory image updates the ESP32, not the desk controller.
Until a release is published, use the development YAML configurations.

## USB and Wi-Fi

1. Download `t150-stamp-c3-<version>.factory.bin` and `SHA256SUMS` from the same
   [published release](https://github.com/0xA2C3B1F4/t150-desk-control/releases).
   Compare the file's SHA-256 with its entry in `SHA256SUMS`.
2. Keep the desk controller powered off. Connect the Stamp to your computer
   with a USB data cable. Follow the [UART wiring](hardware.md#uart-wiring);
   leave the boards' 5V and 3V3 pins unconnected.
3. Open [ESPHome Web](https://web.esphome.io/) in a browser with Web Serial
   support, such as Chrome or Edge. Connect to the Stamp's serial port, choose
   **Install**, select the factory file and wait for completion.
4. Choose **Configure Wi-Fi** and enter your 2.4 GHz network credentials.
   If connecting fails, correct the credentials and try again over USB.
5. Open **Logs**. Hold the Stamp's G3 user button for two seconds, then release.
   This is the user button, not Reset. Read the web username, web password and
   Home Assistant API key. Keep them private. The button can show them again
   later without changing them, including when Wi-Fi is unavailable.

On macOS or Linux, calculate the downloaded file's hash with:

```sh
shasum -a 256 t150-stamp-c3-2026.11.0.factory.bin
```

On Windows PowerShell, use `Get-FileHash -Algorithm SHA256 <file>`.

## Web and Home Assistant

Open the device address shown after Wi-Fi setup and sign in with its web
credentials. The interface is served from the Stamp and uses HTTP Basic
authentication. Web traffic is not encrypted; use a trusted local network.

Add the device through Home Assistant's **ESPHome** integration and enter the
API encryption key. Each Stamp generates its own credentials and gets a name
with a MAC suffix. Restore controller power when the wiring is complete and
check the reported height and control status before moving the desk.

The binary uses the same base controls as the Stamp YAML example. Optional
panel lock, motion speed, trim, custom sound, display and maintenance controls
are available through the [YAML packages](esphome.md#github-configurations).

## Updates and recovery

Update this image over USB with ESPHome Web. Installation erases stored Wi-Fi,
web/API credentials and ESPHome recovery data. First resolve any
[preset restoration](esphome.md#recovering-a-borrowed-preset) that is pending.
Then install the new factory image, configure Wi-Fi, read the new credentials
and update the API key in Home Assistant. This image does not provide OTA.

A normal restart or Wi-Fi change keeps the web and API credentials. If credential
storage fails, web, API and desk control stay disabled; USB remains the recovery
path. Reinstalling the factory image replaces credentials.

To return to a custom configuration, install the [Stamp YAML example](../README.md#esphome-device-builder)
over USB with your chosen device name and secrets. Its later updates can use OTA.
