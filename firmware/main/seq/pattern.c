#include "pattern.h"

#include <string.h>

static pattern_bank_t s_banks[BEATBOX_BANK_COUNT];
static uint32_t s_revision = 1;
static uint8_t s_swing = 50;
static uint8_t s_var = 0;
static uint8_t s_pending_var = 0;
static bool s_var_pending = false;
static bool s_fill = false;
static bool s_click = true;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

esp_err_t pattern_init(void)
{
    memset(s_banks, 0, sizeof(s_banks));
    s_revision = 1;
    s_swing = 50;
    s_var = 0;
    s_pending_var = 0;
    s_var_pending = false;
    s_fill = false;
    s_click = true;
    pattern_load_default();
    return ESP_OK;
}

uint32_t pattern_revision(void)
{
    return s_revision;
}

uint8_t pattern_swing(void)
{
    return s_swing;
}

uint8_t pattern_variation(void)
{
    return s_var;
}

bool pattern_fill_active(void)
{
    return s_fill;
}

bool pattern_click_enabled(void)
{
    return s_click;
}

void pattern_set_swing(uint8_t swing)
{
    s_swing = beatbox_clamp_swing(swing);
}

void pattern_set_click(bool enabled)
{
    s_click = enabled;
}

void pattern_set_fill(bool held)
{
    s_fill = held;
}

void pattern_request_variation(uint8_t var)
{
    s_pending_var = var ? 1 : 0;
    if (s_pending_var == s_var) {
        s_var_pending = false;
        return;
    }
    s_var_pending = true;
}

void pattern_poll_variation(uint16_t tick_in_bar)
{
    if (!s_var_pending) {
        return;
    }
    if ((tick_in_bar % BEATBOX_TICKS_PER_16TH) != 0) {
        return;
    }
    s_var = s_pending_var;
    s_var_pending = false;
}

const pattern_bank_t *pattern_active_bank(void)
{
    if (s_fill) {
        return &s_banks[BEATBOX_BANK_FILL];
    }
    return &s_banks[s_var ? BEATBOX_BANK_B : BEATBOX_BANK_A];
}

const pattern_bank_t *pattern_bank(uint8_t bank)
{
    if (bank >= BEATBOX_BANK_COUNT) {
        return &s_banks[BEATBOX_BANK_A];
    }
    return &s_banks[bank];
}

esp_err_t pattern_set_bank(uint8_t bank, uint32_t expected_rev, const uint8_t *bytes)
{
    if (bank >= BEATBOX_BANK_COUNT || bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Last-write-wins: revision is informational for hosts, never blocks updates. */
    (void)expected_rev;

    pattern_bank_t *dst = &s_banks[bank];
    for (int t = 0; t < BEATBOX_TRACK_COUNT; ++t) {
        for (int s = 0; s < BEATBOX_STEPS_PER_BAR; ++s) {
            uint8_t v = bytes[t * BEATBOX_STEPS_PER_BAR + s];
            if (v > 127) {
                v = 127;
            }
            dst->vel[t][s] = v;
        }
    }
    s_revision++;
    if (s_revision == 0) {
        s_revision = 1;
    }
    return ESP_OK;
}

void pattern_encode_hex(const pattern_bank_t *bank, char *out, size_t out_len)
{
    static const char *hex = "0123456789abcdef";
    if (bank == NULL || out == NULL || out_len < (BEATBOX_PATTERN_HEX_CHARS + 1)) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    size_t o = 0;
    for (int t = 0; t < BEATBOX_TRACK_COUNT; ++t) {
        for (int s = 0; s < BEATBOX_STEPS_PER_BAR; ++s) {
            const uint8_t v = bank->vel[t][s];
            out[o++] = hex[(v >> 4) & 0x0f];
            out[o++] = hex[v & 0x0f];
        }
    }
    out[o] = '\0';
}

esp_err_t pattern_decode_hex(const char *hex, uint8_t *bytes)
{
    if (hex == NULL || bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < BEATBOX_PATTERN_BYTES; ++i) {
        const int hi = hex_nibble(hex[i * 2]);
        const int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

void pattern_load_default(void)
{
    memset(s_banks, 0, sizeof(s_banks));
    /* Basic house groove on A. */
    s_banks[BEATBOX_BANK_A].vel[0][0] = 120;
    s_banks[BEATBOX_BANK_A].vel[0][4] = 110;
    s_banks[BEATBOX_BANK_A].vel[0][8] = 120;
    s_banks[BEATBOX_BANK_A].vel[0][12] = 110;
    s_banks[BEATBOX_BANK_A].vel[1][4] = 118;
    s_banks[BEATBOX_BANK_A].vel[1][12] = 118;
    /* Quiet 8th-note hats; downbeats a touch stronger, ghosts softer. */
    for (int s = 0; s < BEATBOX_STEPS_PER_BAR; s += 2) {
        s_banks[BEATBOX_BANK_A].vel[2][s] = (s % 4 == 0) ? 58 : 42;
    }

    /* Sparse B variation. */
    s_banks[BEATBOX_BANK_B].vel[0][0] = 120;
    s_banks[BEATBOX_BANK_B].vel[0][8] = 120;
    s_banks[BEATBOX_BANK_B].vel[1][4] = 118;
    s_banks[BEATBOX_BANK_B].vel[1][10] = 100;
    s_banks[BEATBOX_BANK_B].vel[1][12] = 118;
    for (int s = 0; s < BEATBOX_STEPS_PER_BAR; s += 2) {
        s_banks[BEATBOX_BANK_B].vel[2][s] = (s % 4 == 0) ? 52 : 38;
    }

    /*
     * Fill: eight consecutive 8th-note snares (countable roll).
     * A 16th-note snare roll blurs to ~4 hits here: the snare one-shot is
     * ~140 ms, almost one 16th at 120 BPM, so adjacent 16ths smear together.
     */
    s_banks[BEATBOX_BANK_FILL].vel[0][0] = 120;
    s_banks[BEATBOX_BANK_FILL].vel[0][8] = 110;
    for (int i = 0; i < 8; ++i) {
        const int s = i * 2;
        s_banks[BEATBOX_BANK_FILL].vel[1][s] = (uint8_t)(100 + i * 3);
    }
    for (int s = 0; s < BEATBOX_STEPS_PER_BAR; s += 2) {
        s_banks[BEATBOX_BANK_FILL].vel[2][s] = 40;
    }
    s_banks[BEATBOX_BANK_FILL].vel[5][6] = 105;
    s_banks[BEATBOX_BANK_FILL].vel[5][10] = 100;
    s_banks[BEATBOX_BANK_FILL].vel[5][14] = 110;
}
