# Config Portal

The device exposes a configuration web portal via SoftAP for first-use setup and on-demand reconfiguration after deployment.

## When the portal opens

1. **First boot:** No WiFi credentials in NVS — portal opens automatically.
2. **GPIO7 button press during deep sleep:** Wakes the device into portal mode (telemetry is skipped for that cycle).

In both cases the device hosts SSID `FireBeetle_C6_Prov` (open WiFi). Connect a phone/laptop and navigate to `http://192.168.4.1`.

The portal auto-exits to deep sleep after 10 minutes idle.

## Pages

- **/wifi** — set WiFi SSID, password, and device ID.
- **/calibrate** — live mV readout; *Capture DRY* (sensor in air) + *Capture WET* (sensor submerged to MAX line) + *Save*.
- **/status** — current stored calibration, last live mV, last percentage, last cal timestamp.
- **/factory-reset** — wipes WiFi credentials *and* calibration; restarts.

## Hardware

- **GPIO7** — momentary push button to GND. Internal pull-up enabled at wake-config time.
- The legacy GPIO20 factory-reset button is removed; factory reset lives in the portal.

## Calibration procedure

1. Press the GPIO7 button. Wait a few seconds for the SoftAP to come up.
2. Connect to `FireBeetle_C6_Prov` and open `http://192.168.4.1/calibrate`.
3. With sensor in dry air, click **Capture DRY**.
4. Submerge sensor to the MAX line, click **Capture WET**.
5. Click **Save & Restart**.

Defaults if calibration is missing in NVS: `dry_mv = 2800`, `wet_mv = 0`. Telemetry still publishes, just less accurate.
