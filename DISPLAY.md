# E-paper Display

A Waveshare 2.13" e-paper hat (SSD1680 controller, 122 × 250 px portrait, 1-bit monochrome) shows telemetry on every wake and portal info during configuration.

## Hardware

| E-paper pin | ESP32-C6 GPIO | Direction |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| DIN (MOSI) | GPIO22 | output |
| CLK (SCK) | GPIO23 | output |
| CS | GPIO1 | output |
| DC | GPIO19 | output |
| RST | GPIO14 | output |
| BUSY | GPIO4 | input |

VCC stays on the 3V3 rail at all times — e-paper retains its image with no current, so there is no need to gate power.

## What's shown

### Telemetry view (after each MQTT publish)

- Device ID (header)
- Moisture % (large hero number)
- Raw sensor reading (mV)
- Battery voltage and percentage (linear 3.3 V = 0 %, 4.2 V = 100 %)
- WiFi signal (dBm) — `--` if disconnected

### Portal view (on GPIO7 wake or first boot)

- `CONFIGURE` header
- SoftAP SSID `FireBeetle_C6_Prov`
- URL `http://192.168.4.1`
- QR code that decodes to the URL — scan with a phone to join + open

## Refresh strategy

Always full refresh, once per wake. The bistable e-paper retains the image while the device is in deep sleep, so the screen stays accurate for the full hour between wakes.

## Regenerating assets

Fonts, icons, and the portal QR bitmap live in the committed header `include/display_assets.h`. They are produced by `tools/gen_display_assets.py`. Regenerate only when the portal URL changes or you want to swap fonts:

```bash
python3 -m pip install --user qrcode Pillow
python3 tools/gen_display_assets.py
git add include/display_assets.h
git commit -m "Regenerate display assets"
```
