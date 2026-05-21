/**
 * @file display.c
 * @brief Waveshare 2.13" e-paper (SSD1680) — driver + layout.
 *
 * Pinout (matches the spec):
 *   MOSI  GPIO22   CS    GPIO1
 *   SCK   GPIO23   DC    GPIO19
 *   BUSY  GPIO4    RST   GPIO14
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
#define PIN_RST     14
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

    gpio_config_t busy = {
        .pin_bit_mask = (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
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

    panel_init();
    return ESP_OK;
}

void display_show_telemetry(const display_telemetry_t *t) {
    (void)t;  // Layout rendering lands in Task 6.
    fb_clear(0xFF);  // all-white for now
    panel_refresh_full();
}

void display_show_portal(void) {
    // Layout rendering lands in Task 7.
    fb_clear(0xFF);
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
