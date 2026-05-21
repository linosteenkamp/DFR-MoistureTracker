/**
 * @file display.c
 * @brief Waveshare 2.13" e-paper (SSD1680) — driver + layout.
 *
 * Pinout (matches the spec):
 *   MOSI  GPIO22   CS    GPIO1
 *   SCK   GPIO23   DC    GPIO19
 *   BUSY  GPIO4    RST   GPIO18
 *
 * Layout: portrait 122 x 250 px. Framebuffer is 16 bytes wide
 * (ceil(122 / 8)) by 250 tall = 4000 bytes.
 */

#ifndef TEST_HOST
#include "display.h"
#include "display_assets.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#endif

// ============================================================================
// Pure helpers (host-testable)
// ============================================================================

int display_battery_v_to_pct(float v) {
    if (v <= 3.3f) return 0;
    if (v >= 4.2f) return 100;
    float pct = (v - 3.3f) * (100.0f / 0.9f);
    int i = (int)(pct + 0.5f);
    if (i < 0) return 0;
    if (i > 100) return 100;
    return i;
}

#ifndef TEST_HOST

// ============================================================================
// Hardware configuration
// ============================================================================

static const char *TAG = "DISPLAY";

#define PIN_MOSI    22
#define PIN_SCK     23
#define PIN_CS       1
#define PIN_DC      19
#define PIN_RST     18
#define PIN_BUSY     4

#define DISPLAY_W    122
#define DISPLAY_H_PX 250
#define DISPLAY_BPR  ((DISPLAY_W + 7) / 8)        // 16
#define FB_SIZE      (DISPLAY_BPR * DISPLAY_H_PX) // 4000

// SSD1680 command opcodes (only the ones we use)
#define CMD_DRIVER_OUTPUT_CTRL    0x01
#define CMD_DEEP_SLEEP            0x10
#define CMD_DATA_ENTRY_MODE       0x11
#define CMD_SW_RESET              0x12
#define CMD_TEMP_SENSOR           0x18
#define CMD_MASTER_ACTIVATE       0x20
#define CMD_DISPLAY_UPDATE_CTRL_1 0x21
#define CMD_DISPLAY_UPDATE_CTRL_2 0x22
#define CMD_WRITE_RAM_BW          0x24
#define CMD_BORDER_WAVEFORM       0x3C
#define CMD_SET_RAM_X_RANGE       0x44
#define CMD_SET_RAM_Y_RANGE       0x45
#define CMD_SET_RAM_X_ADDR        0x4E
#define CMD_SET_RAM_Y_ADDR        0x4F

static spi_device_handle_t s_spi = NULL;
static uint8_t s_fb[FB_SIZE];

// ============================================================================
// Low-level: SPI + DC/CS/RST/BUSY
// ============================================================================

static void wait_busy(void) {
    // BUSY is HIGH while panel is mid-operation. Poll every 10 ms, with a
    // generous 5-second ceiling (full refresh takes ~3 s).
    int waited = 0;
    while (gpio_get_level(PIN_BUSY) == 1 && waited < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited++;
    }
    if (waited >= 500) {
        ESP_LOGW(TAG, "BUSY timeout after 5 s");
    }
}

static void send_cmd(uint8_t cmd) {
    gpio_set_level(PIN_DC, 0);  // DC LOW = command
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void send_data(const uint8_t *data, size_t n) {
    if (n == 0) return;
    gpio_set_level(PIN_DC, 1);  // DC HIGH = data
    spi_transaction_t t = {
        .length = n * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void send_data_byte(uint8_t b) {
    send_data(&b, 1);
}

// ============================================================================
// Panel init / refresh / sleep
// ============================================================================

static void panel_hw_reset(void) {
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void panel_init(void) {
    panel_hw_reset();
    wait_busy();

    send_cmd(CMD_SW_RESET);
    wait_busy();

    // 250 lines (MUX 0xF9 + 1 = 250); GD=0, SM=0, TB=0
    send_cmd(CMD_DRIVER_OUTPUT_CTRL);
    send_data_byte(0xF9);
    send_data_byte(0x00);
    send_data_byte(0x00);

    // Data entry mode: Y increment, X increment, address counter updates Y first
    send_cmd(CMD_DATA_ENTRY_MODE);
    send_data_byte(0x03);

    // RAM X range: 0..(122/8-1) = 0..15
    send_cmd(CMD_SET_RAM_X_RANGE);
    send_data_byte(0x00);
    send_data_byte(DISPLAY_BPR - 1);

    // RAM Y range: 0..249
    send_cmd(CMD_SET_RAM_Y_RANGE);
    send_data_byte(0x00);
    send_data_byte(0x00);
    send_data_byte((DISPLAY_H_PX - 1) & 0xFF);
    send_data_byte(((DISPLAY_H_PX - 1) >> 8) & 0xFF);

    // Border: black
    send_cmd(CMD_BORDER_WAVEFORM);
    send_data_byte(0x05);

    // Display update control: bypass red RAM, normal source output
    send_cmd(CMD_DISPLAY_UPDATE_CTRL_1);
    send_data_byte(0x00);
    send_data_byte(0x80);

    // Use internal temperature sensor
    send_cmd(CMD_TEMP_SENSOR);
    send_data_byte(0x80);

    // Reset RAM address pointer to (0, 0)
    send_cmd(CMD_SET_RAM_X_ADDR);
    send_data_byte(0x00);
    send_cmd(CMD_SET_RAM_Y_ADDR);
    send_data_byte(0x00);
    send_data_byte(0x00);

    wait_busy();
}

static void panel_refresh_full(void) {
    // Push framebuffer to BW RAM
    send_cmd(CMD_WRITE_RAM_BW);
    send_data(s_fb, FB_SIZE);

    // Full-update sequence: 0xF7 = Load LUT + Display
    send_cmd(CMD_DISPLAY_UPDATE_CTRL_2);
    send_data_byte(0xF7);
    send_cmd(CMD_MASTER_ACTIVATE);
    wait_busy();
}

static void panel_sleep(void) {
    send_cmd(CMD_DEEP_SLEEP);
    send_data_byte(0x01);  // Deep sleep mode 1
}

// ============================================================================
// Framebuffer
// ============================================================================

static void fb_clear(uint8_t fill) {
    memset(s_fb, fill, FB_SIZE);
}

// ============================================================================
// Drawing primitives
// ============================================================================

// Coordinate convention: (0, 0) is top-left. x in [0, 121], y in [0, 249].
// Each byte in the framebuffer holds 8 horizontal pixels, MSB on the left.
// Bit value 0 = black pixel (drawn), 1 = white (background).

static inline void draw_pixel(int x, int y, bool on) {
    if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H_PX) return;
    uint8_t mask = 0x80 >> (x & 7);
    size_t idx = y * DISPLAY_BPR + (x >> 3);
    if (on) {
        s_fb[idx] &= ~mask;  // clear bit = black
    } else {
        s_fb[idx] |= mask;
    }
}

static void draw_hline(int x, int y, int w) {
    for (int i = 0; i < w; i++) {
        draw_pixel(x + i, y, true);
    }
}

// Draw a 1-bit bitmap at (x, y). Bytes are MSB-first within a row.
// A 1 bit in the bitmap means "draw black pixel" (matches the asset
// generator's output format).
static void draw_bitmap(int x, int y, const uint8_t *bm, int w, int h) {
    int bpr = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t byte = bm[row * bpr + (col >> 3)];
            bool on = byte & (0x80 >> (col & 7));
            if (on) {
                draw_pixel(x + col, y + row, true);
            }
        }
    }
}

// Render one small-font glyph at (x, y). Unsupported chars render as blank.
static void draw_glyph_small(int x, int y, char ch) {
    int idx = (int)(unsigned char)ch - DISPLAY_FONT_SMALL_FIRST;
    if (idx < 0 || idx >= DISPLAY_FONT_SMALL_COUNT) {
        return;
    }
    int glyph_bytes = ((DISPLAY_FONT_SMALL_W + 7) / 8) * DISPLAY_FONT_SMALL_H;
    const uint8_t *bm = &display_font_small[idx * glyph_bytes];
    draw_bitmap(x, y, bm, DISPLAY_FONT_SMALL_W, DISPLAY_FONT_SMALL_H);
}

static void draw_text_small(int x, int y, const char *s) {
    int cx = x;
    for (; *s; s++) {
        draw_glyph_small(cx, y, *s);
        cx += DISPLAY_FONT_SMALL_W;
    }
}

// 2x-scaled small font: pixel-double each glyph into a 12x16 cell.
// Used for the device-name header where the 6x8 small font looks cramped.
static void draw_glyph_small_2x(int x, int y, char ch) {
    int idx = (int)(unsigned char)ch - DISPLAY_FONT_SMALL_FIRST;
    if (idx < 0 || idx >= DISPLAY_FONT_SMALL_COUNT) return;
    int bpr = (DISPLAY_FONT_SMALL_W + 7) / 8;
    int glyph_bytes = bpr * DISPLAY_FONT_SMALL_H;
    const uint8_t *bm = &display_font_small[idx * glyph_bytes];
    for (int row = 0; row < DISPLAY_FONT_SMALL_H; row++) {
        for (int col = 0; col < DISPLAY_FONT_SMALL_W; col++) {
            uint8_t byte = bm[row * bpr + (col >> 3)];
            if (byte & (0x80 >> (col & 7))) {
                int px = x + col * 2;
                int py = y + row * 2;
                draw_pixel(px,     py,     true);
                draw_pixel(px + 1, py,     true);
                draw_pixel(px,     py + 1, true);
                draw_pixel(px + 1, py + 1, true);
            }
        }
    }
}

static void draw_text_small_2x(int x, int y, const char *s) {
    int cx = x;
    for (; *s; s++) {
        draw_glyph_small_2x(cx, y, *s);
        cx += DISPLAY_FONT_SMALL_W * 2;
    }
}

static int text_small_2x_width(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n * DISPLAY_FONT_SMALL_W * 2;
}

static void draw_text_small_2x_centered(int x0, int x1, int y, const char *s) {
    int w = text_small_2x_width(s);
    int x = x0 + ((x1 - x0) - w) / 2;
    if (x < x0) x = x0;
    draw_text_small_2x(x, y, s);
}

// Large-font lookup: walk DISPLAY_FONT_LARGE_CHARS to find the index.
static int large_index_of(char ch) {
    for (int i = 0; DISPLAY_FONT_LARGE_CHARS[i]; i++) {
        if (DISPLAY_FONT_LARGE_CHARS[i] == ch) return i;
    }
    return -1;
}

static void draw_glyph_large(int x, int y, char ch) {
    int idx = large_index_of(ch);
    if (idx < 0) return;
    int glyph_bytes = ((DISPLAY_FONT_LARGE_W + 7) / 8) * DISPLAY_FONT_LARGE_H;
    const uint8_t *bm = &display_font_large[idx * glyph_bytes];
    draw_bitmap(x, y, bm, DISPLAY_FONT_LARGE_W, DISPLAY_FONT_LARGE_H);
}

static void draw_text_large(int x, int y, const char *s) {
    int cx = x;
    for (; *s; s++) {
        draw_glyph_large(cx, y, *s);
        cx += DISPLAY_FONT_LARGE_W;
    }
}

// Helper: measure small-font text width
static int text_small_width(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n * DISPLAY_FONT_SMALL_W;
}

// Helper: draw small text centered in a horizontal range [x0, x1)
static void draw_text_small_centered(int x0, int x1, int y, const char *s) {
    int w = text_small_width(s);
    int x = x0 + ((x1 - x0) - w) / 2;
    if (x < x0) x = x0;
    draw_text_small(x, y, s);
}

// ============================================================================
// Layouts
// ============================================================================

static void format_pct_1dp(char *buf, size_t n, float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    int whole = (int)v;
    int frac  = (int)((v - whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    // At 100% the "100.0%" string is 6 large-font glyphs = 144 px > 122 px
    // display width. Drop the decimal in that single case.
    if (whole >= 100) {
        snprintf(buf, n, "%d%%", whole);
    } else {
        snprintf(buf, n, "%d.%d%%", whole, frac);
    }
}

// ============================================================================
// Public API (placeholders for layout — Tasks 5–7 fill the rendering in)
// ============================================================================

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Initialising e-paper display");

    // GPIO setup for control pins
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Pull-down enabled so a missing display (BUSY pin floating) reads LOW.
    // A real SSD1680 actively drives BUSY HIGH while processing a reset; the
    // weak internal pull-down (~45 kΩ) doesn't fight the panel's drive.
    gpio_config_t busy = {
        .pin_bit_mask = (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy);

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1);

    // SPI bus + device
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_SIZE + 16,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", err);
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,   // 10 MHz
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
        .flags = 0,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d", err);
        return err;
    }

    // Probe for the panel: pulse RST and look for BUSY going HIGH. A real
    // SSD1680 holds BUSY HIGH for ~5–15 ms while processing the reset. With
    // the internal pull-down on the BUSY pin (set above), a missing display
    // reads LOW. Bail early so the 5-second wait_busy timeouts in panel_init
    // don't burn battery on display-less devices.
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    if (gpio_get_level(PIN_BUSY) == 0) {
        ESP_LOGI(TAG, "No e-paper detected — skipping display");
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    panel_init();
    return ESP_OK;
}

void display_show_telemetry(const display_telemetry_t *t) {
    fb_clear(0xFF);  // white background

    // Header: device ID in 2x small font, centered. Strip the "tree-" prefix
    // — it's the convention for all device names in this deployment and is
    // redundant on a per-device screen. Uppercase the result because the 2x
    // pixel-doubled small font has cramped descenders on lowercase letters.
    const char *src = (t && t->device_id) ? t->device_id : "";
    if (strncmp(src, "tree-", 5) == 0) src += 5;
    char upper_name[33];
    size_t i = 0;
    for (; i + 1 < sizeof(upper_name) && src[i]; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        upper_name[i] = c;
    }
    upper_name[i] = '\0';
    draw_text_small_2x_centered(0, DISPLAY_W, 4, upper_name);
    draw_hline(4, 24, DISPLAY_W - 8);

    // Hero: moisture % centered. format_pct_1dp drops the decimal at 100%
    // so the worst case is "99.9%" (5 large glyphs = 120 px, fits in 122).
    char hero[8] = {0};
    format_pct_1dp(hero, sizeof(hero), t ? t->moisture_pct : 0.0f);
    int hero_w = (int)strlen(hero) * DISPLAY_FONT_LARGE_W;
    int hero_x = (DISPLAY_W - hero_w) / 2;
    if (hero_x < 0) hero_x = 0;
    draw_text_large(hero_x, 36, hero);
    draw_text_small_centered(0, DISPLAY_W, 74, "MOISTURE");

    draw_hline(4, 90, DISPLAY_W - 8);

    // Data rows: label on the left, value on the right.
    char buf[20];
    int row_y = 100;
    int row_h = 14;

    snprintf(buf, sizeof(buf), "%d mV", t ? t->raw_mv : 0);
    draw_text_small(6, row_y, "SENSOR");
    {
        int w = text_small_width(buf);
        draw_text_small(DISPLAY_W - 6 - w, row_y, buf);
    }
    row_y += row_h;

    snprintf(buf, sizeof(buf), "%.2f V", t ? (double)t->battery_v : 0.0);
    draw_text_small(6, row_y, "BATTERY");
    {
        int w = text_small_width(buf);
        draw_text_small(DISPLAY_W - 6 - w, row_y, buf);
    }
    row_y += row_h;

    snprintf(buf, sizeof(buf), "%d %%", t ? t->battery_pct : 0);
    draw_text_small(6, row_y, "BAT %");
    {
        int w = text_small_width(buf);
        draw_text_small(DISPLAY_W - 6 - w, row_y, buf);
    }
    row_y += row_h;

    if (t && t->wifi_rssi_dbm != 0) {
        snprintf(buf, sizeof(buf), "%d dBm", t->wifi_rssi_dbm);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_text_small(6, row_y, "WIFI");
    {
        int w = text_small_width(buf);
        draw_text_small(DISPLAY_W - 6 - w, row_y, buf);
    }

    panel_refresh_full();
}

void display_show_portal(void) {
    fb_clear(0xFF);

    // Header
    draw_text_small_centered(0, DISPLAY_W, 6, "CONFIGURE");
    draw_hline(4, 18, DISPLAY_W - 8);

    // SSID + URL, two pairs of lines, both centered.
    // SSID and URL kept verbatim — phones do case-sensitive SSID matching.
    draw_text_small_centered(0, DISPLAY_W, 28, "CONNECT TO:");
    draw_text_small_centered(0, DISPLAY_W, 40, "FireBeetle_C6_Prov");
    draw_text_small_centered(0, DISPLAY_W, 56, "OPEN IN BROWSER:");
    draw_text_small_centered(0, DISPLAY_W, 68, "http://192.168.4.1");

    // QR bitmap, centered horizontally, below the URL text.
    int qr_x = (DISPLAY_W - DISPLAY_QR_W) / 2;
    int qr_y = 90;
    draw_bitmap(qr_x, qr_y, display_qr, DISPLAY_QR_W, DISPLAY_QR_H);

    // Hint at the bottom
    draw_text_small_centered(0, DISPLAY_W, qr_y + DISPLAY_QR_H + 8, "SCAN TO CONFIGURE");

    panel_refresh_full();
}

void display_deinit(void) {
    if (!s_spi) return;
    panel_sleep();
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
    // Leave the bus initialised — the device may add more SPI peripherals
    // later. spi_bus_free is fine to skip; bus stays idle.
}

#endif // TEST_HOST
