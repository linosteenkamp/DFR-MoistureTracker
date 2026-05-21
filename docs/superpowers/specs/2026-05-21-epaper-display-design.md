# Spec: E-paper Display Module

## Context

A Waveshare 2.13" e-paper hat (250 × 122 px, 1-bit monochrome, SSD1680 controller) is now wired to the ESP32-C6. We want the screen to show telemetry at a glance during normal operation and portal info during configuration. The display is bistable — it retains the image without power, which suits the deep-sleep model: refresh once per wake, then the image stays visible for the full hour.

The screen will be mounted in portrait orientation (122 × 250) for a dashboard-style layout. During portal mode (GPIO7 wake or first-boot), the screen shows the SoftAP SSID, the portal URL, and a QR code so the user can join without re-typing credentials.

## Goals

1. Telemetry is visible at a glance without opening the MQTT subscriber — the device communicates its state via the screen.
2. Portal mode shows AP SSID + URL + QR so a user holding the device can configure it without prior knowledge of the SSID.
3. Once-per-hour refresh fits inside the existing wake window with negligible battery impact.
4. The display module follows the existing module pattern (single responsibility, clear public API, host-testable pure logic where possible) and depends only on what's already in ESP-IDF.

## Non-goals

- Partial refresh modes (always full refresh — once per hour means ghosting accumulates otherwise).
- Wall-clock time / SNTP (user opted to drop the timestamp).
- Touch input or any input from the display itself.
- Multi-page UI — single view per mode (telemetry view, portal view).
- Dynamic QR for varying URLs (the portal URL is a constant).
- External e-paper / graphics libraries — a focused custom driver keeps the dep surface zero.

## User flows

### Normal telemetry wake

1. Timer wake fires hourly. Device boots, runs `init_system`, takes the telemetry path.
2. WiFi connects, MQTT publishes the telemetry payload.
3. **Display updates** with the dashboard view (device ID, moisture %, sensor mV, battery V, battery %, WiFi dBm).
4. Device deep-sleeps. Screen retains the image for the next hour.

### Portal mode

1. User presses GPIO7 (or device boots unprovisioned). Device wakes, runs `init_system`, takes the portal path.
2. **Display updates** with the portal view (CONFIGURE label, AP SSID `FireBeetle_C6_Prov`, URL `http://192.168.4.1`, QR code for that URL).
3. User scans the QR with their phone, joins the AP, opens the portal in a browser, does what they came to do.
4. Portal exits via save-and-restart (telemetry path on next boot will overwrite the screen with the dashboard) or via 10-min idle timeout (device sleeps; portal screen remains visible).

## Requirements

### Functional

- **R1.** A new `display` module exposes `display_init`, `display_show_telemetry(const display_telemetry_t *)`, `display_show_portal`, `display_deinit`.
- **R2.** `display_show_telemetry` renders the six chosen fields in the portrait dashboard layout: device ID, moisture % (hero), raw sensor mV, battery V, battery %, WiFi signal in dBm.
- **R3.** `display_show_portal` renders the SoftAP SSID, the portal URL, and a QR code that decodes to `http://192.168.4.1`.
- **R4.** Battery percentage is derived from voltage via a linear map (3.3 V = 0 %, 4.2 V = 100 %, clamped to [0, 100]).
- **R5.** WiFi RSSI is fetched via a new `wifi_manager_get_rssi()` helper that wraps `esp_wifi_sta_get_ap_info()`. Returns 0 when not connected.
- **R6.** The display refresh is fired once per wake — after the MQTT publish completes in the telemetry path, after the SoftAP comes up in the portal path. Both happen before the long blocking phase (deep sleep / portal idle loop).
- **R7.** All refreshes are full refreshes (no partial mode), regardless of mode.

### Non-functional

- **N1.** Module depends only on existing ESP-IDF SPI + GPIO APIs. No new external libraries.
- **N2.** Per-wake additional active current draw is bounded: ~20 mA × ~3 s for the e-paper refresh, plus ~5 ms of SPI bus activity. Negligible against the existing 4–5 s wake window.
- **N3.** No power switching for the e-paper — VCC stays on the 3V3 rail; bistable retention means zero sleep current contribution.
- **N4.** Fonts, icons, and the QR bitmap are embedded as `const uint8_t` arrays in flash. No runtime QR encoder.
- **N5.** Pure-logic units (battery V→% mapping, font glyph indexing, QR bitmap byte counts) are covered by host-native unit tests.
- **N6.** All driver code lives inside `#ifndef TEST_HOST` so the pure-logic functions remain host-testable via the existing direct-include pattern.

## Architecture

### Modules

| Module | Status | Responsibility |
|---|---|---|
| `display` | New | SSD1680 driver (SPI init, command/data, framebuffer, full refresh), two-font text renderer, bitmap blit, layout composition for dashboard + portal views. |
| `display_assets` | New (generated header) | Embedded `const uint8_t` arrays: small font 6×10, large font 24×32, battery icon, WiFi icon, pre-baked QR bitmap of the portal URL. |
| `tools/gen_display_assets.py` | New (build-time helper) | Takes BDF/PCF font files and the portal URL constant, emits `display_assets.h`. Run manually when assets change; the output is committed. |
| `wifi_manager` | Modified | New getter `wifi_manager_get_rssi(void)` that returns the AP info RSSI in dBm, or 0 if not connected. |
| `main` | Modified | Two new call sites: after `publish_telemetry_once()` and before `enter_deep_sleep()` in the telemetry path; at the start of `run_portal_then_sleep()` before `config_portal_run()`. |

### Public API

```c
// include/display.h
typedef struct {
    const char *device_id;
    float       moisture_pct;
    int         raw_mv;
    float       battery_v;
    int         battery_pct;     // computed from V via linear 3.3=0%, 4.2=100%
    int         wifi_rssi_dbm;   // 0 = unknown / not connected
} display_telemetry_t;

esp_err_t display_init(void);                                  // SPI bus, GPIOs, wake panel from sleep
void      display_show_telemetry(const display_telemetry_t *t);
void      display_show_portal(void);                           // SSID + URL + QR
void      display_deinit(void);                                // put panel in deep sleep, release SPI bus
```

A pure helper, exposed for host tests:

```c
// Pure linear map, no hardware. Clamped to [0, 100].
int display_battery_v_to_pct(float v);
```

### Hardware pinout

All pins are non-strapping and free of conflicts with the existing battery (GPIO0), soil moisture (GPIO2/3), and wake button (GPIO7).

| E-paper pin | ESP32-C6 GPIO | Direction | Notes |
|---|---|---|---|
| VCC | 3V3 | — | Always-on; bistable retention means no power gating |
| GND | GND | — | |
| DIN (MOSI) | GPIO22 | output | SPI2 MOSI |
| CLK (SCK) | GPIO23 | output | SPI2 SCK |
| CS | GPIO1 | output | active LOW |
| DC | GPIO19 | output | re-pinned from initial GPIO8 to avoid the strapping-pin risk |
| RST | GPIO14 | output | active LOW pulse during `display_init` |
| BUSY | GPIO4 | input | active HIGH while panel is mid-refresh |

### Refresh strategy

- **Full refresh every time.** A partial-refresh discipline would require ghost-tracking state across boots (RTC memory) and a periodic full refresh anyway. Once per hour, the visual flash of a full refresh is acceptable and the ghosting story is simpler.
- **One refresh per wake.** In the telemetry path: after `mqtt_publisher_publish_telemetry` completes (success or fail) and before `enter_deep_sleep`. In the portal path: after `start_softap` succeeds and before `config_portal_run`'s blocking loop.
- **Portal view does not re-render during the portal session.** The user can stay in the portal for up to 10 minutes; the screen content (AP SSID + URL + QR) is invariant during that time, so no point updating it.

### Layout — dashboard view (portrait 122 × 250)

The composition is hand-coded in `display.c` as a sequence of `draw_text`, `draw_hline`, and `draw_bitmap` calls. Layout is fixed at compile time; only the values change per wake.

```
┌──────────────────────────┐
│   device-id (centered)   │  small font, 12 px tall, header
├──────────────────────────┤  hline
│                          │
│         67.4%            │  large font, hero
│        moisture          │  small font, label
│                          │
├──────────────────────────┤  hline
│ Sensor       1140 mV     │  small font, label/value rows
│ Battery      4.19 V      │
│ Bat %         86 %       │
│ WiFi        −61 dBm      │
└──────────────────────────┘
```

### Layout — portal view (portrait 122 × 250)

```
┌──────────────────────────┐
│       CONFIGURE          │  large-ish, centered header
├──────────────────────────┤
│   FireBeetle_C6_Prov     │  small font, SSID
│   http://192.168.4.1     │  small font, URL
│                          │
│      ┌───────────┐       │
│      │  QR CODE  │       │  pre-baked bitmap, ~80 × 80 px
│      └───────────┘       │
│                          │
│   scan to configure      │  small font, hint
└──────────────────────────┘
```

### Assets

| Asset | Source | Embedded size | Use |
|---|---|---|---|
| Small font (6 × 10) | Public-domain bitmap font (e.g. Tom Thumb extended or similar) | ~2 KB | Labels, headers, footer text |
| Large font (24 × 32) | Same family scaled, digits + `%` + `.` only | ~800 B | Hero moisture number |
| Battery icon | Hand-drawn 16 × 10 bitmap | ~20 B | Battery row |
| WiFi icon | Hand-drawn 12 × 10 bitmap | ~16 B | WiFi row |
| QR bitmap | Pre-encoded `http://192.168.4.1` at version-2/L (~25 × 25 modules), scaled 3× to ~75 × 75 px | ~800 B | Portal view |

Total asset flash footprint: ~4 KB. Generator script lives in `tools/gen_display_assets.py`; the generated header is committed so the build doesn't depend on Python at compile time.

### Integration points in `main.c`

Two changes, both purely additive — they wrap existing call sites:

```c
// publish_telemetry_once() — append display update before returning
publish_telemetry_once() {
    /* ...existing reads + publish... */
    display_telemetry_t t = {
        .device_id     = device_id_buffer,
        .moisture_pct  = soil_moisture,
        .raw_mv        = soil_moisture_read_raw_mv(),
        .battery_v     = voltage,
        .battery_pct   = display_battery_v_to_pct(voltage),
        .wifi_rssi_dbm = wifi_manager_get_rssi(),
    };
    display_init();
    display_show_telemetry(&t);
    display_deinit();
    return ESP_OK;
}

// run_portal_then_sleep() — show portal view before blocking
run_portal_then_sleep() {
    display_init();
    display_show_portal();
    display_deinit();
    config_portal_run();
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
}
```

### Power and timing

- **Active draw added per telemetry wake:** ~20 mA × ~3 s for the refresh = 60 mAs = 17 µAh.
- **Active draw added per portal entry:** same.
- **Sleep draw added:** 0 µA. E-paper retains the image with no current.
- **Wake time added:** ~3 s. Existing telemetry wake is ~5 s; new total ~8 s. Battery impact at one wake per hour is ~0.4 mAh/day — negligible.
- **SPI bus:** 10 MHz, half-duplex, ~5 ms to push the 3812-byte framebuffer.

## Verification

### Automated tests

Host-native (`pio test -e native -f test_display`):

- `test_battery_v_to_pct` — boundaries (3.3 V → 0, 4.2 V → 100, midpoint, below/above clamps).
- `test_qr_bitmap_sanity` — generated QR header has the expected byte count for the chosen version/scale; known pixels (timing patterns, finder patterns) match.
- `test_font_glyph_indexing` — pure helpers that map ASCII → glyph offset return correct values for boundaries and common chars.

On-device tests are not added — the SSD1680 driver and SPI plumbing are tested manually.

### Manual verification

1. **First-boot portal view.** Flash fresh, power on, confirm the e-paper shows the CONFIGURE / SSID / URL / QR view within ~10 s of boot. Scan the QR with a phone — phone joins the AP and opens the portal page.
2. **Telemetry view.** Provision the device, wait for the next telemetry wake (or force-wake via reset). Confirm the dashboard view appears with the six fields populated with real values matching the MQTT payload.
3. **Image retention.** Cut power to the device entirely after a telemetry refresh. Confirm the image remains visible.
4. **GPIO7 mode switch.** From a telemetry-view screen, press GPIO7. Confirm the screen transitions to the portal view within ~10 s.
5. **Boot integrity.** With DC re-pinned to GPIO19, confirm normal boot from cold and from deep-sleep wake. Strap pin reads should be unaffected.

## Open questions

None at time of writing. Decisions resolved during brainstorming:

- Orientation: portrait (122 × 250).
- Layout: dashboard (Layout B from the visual mockups).
- Fields: device ID, moisture %, sensor mV, battery V, battery %, WiFi dBm. No timestamp, no publish status.
- Portal display: full portal info + QR code (not text-only, not unchanged-from-telemetry).
- Driver: custom minimal SSD1680, no external library.
- QR: pre-baked bitmap embedded at build time, no runtime encoder.
- DC pin re-pinned to GPIO19 to avoid GPIO8 strapping-pin risk.
