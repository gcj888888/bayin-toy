#pragma once

#include "clock.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t vel[BEATBOX_TRACK_COUNT][BEATBOX_STEPS_PER_BAR];
} pattern_bank_t;

esp_err_t pattern_init(void);

uint32_t pattern_revision(void);
uint8_t pattern_swing(void);
uint8_t pattern_variation(void);
bool pattern_fill_active(void);
bool pattern_click_enabled(void);

void pattern_set_swing(uint8_t swing);
void pattern_set_click(bool enabled);
void pattern_set_fill(bool held);
/** Request A/B change; applied on the next 16th grid boundary. */
void pattern_request_variation(uint8_t var);
/** Apply any pending variation when tick is on a 16th grid. */
void pattern_poll_variation(uint16_t tick_in_bar);

const pattern_bank_t *pattern_active_bank(void);
const pattern_bank_t *pattern_bank(uint8_t bank);

/**
 * Replace one bank. expected_rev must match current revision (or 0 to force).
 * On success revision is incremented.
 */
esp_err_t pattern_set_bank(uint8_t bank, uint32_t expected_rev, const uint8_t *bytes);

/** Encode pattern velocity bytes to hex + NUL. out_len >= BEATBOX_PATTERN_HEX_CHARS + 1. */
void pattern_encode_hex(const pattern_bank_t *bank, char *out, size_t out_len);
/** Decode hex chars into BEATBOX_PATTERN_BYTES velocity bytes. */
esp_err_t pattern_decode_hex(const char *hex, uint8_t *bytes);

/** Seed a simple default groove into bank A; B/Fill empty. */
void pattern_load_default(void);

#ifdef __cplusplus
}
#endif
