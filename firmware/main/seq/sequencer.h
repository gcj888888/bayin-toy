#pragma once

#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sequencer_note_fn)(uint8_t note, uint8_t velocity);

esp_err_t sequencer_init(sequencer_note_fn note_cb);

/**
 * Called from the audio render loop on every internal 96 PPQN tick.
 * Must remain real-time safe: no malloc, no blocking, no USB I/O.
 */
void sequencer_on_tick(uint16_t tick_in_bar);

#ifdef __cplusplus
}
#endif
