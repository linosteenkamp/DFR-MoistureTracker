/**
 * @file display.c
 * @brief Waveshare 2.13" e-paper (SSD1680) — driver + layout.
 *
 * Pure helpers live above the TEST_HOST guard so they can be unit-tested
 * by including this file directly from test code. Hardware code is below
 * the guard and only compiles for the ESP-IDF target build.
 */

#ifndef TEST_HOST
#include "display.h"
#endif

// ============================================================================
// Pure helpers (host-testable)
// ============================================================================

/**
 * @brief Linear voltage -> battery percentage for a single Li-ion cell.
 *
 * 3.3 V = 0 %, 4.2 V = 100 %. Clamped to [0, 100].
 */
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
// Hardware-dependent code lands here in Task 4.
#endif
