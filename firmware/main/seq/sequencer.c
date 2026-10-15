#include "sequencer.h"

#include "pattern.h"

static sequencer_note_fn s_note_cb;

esp_err_t sequencer_init(sequencer_note_fn note_cb)
{
    s_note_cb = note_cb;
    return ESP_OK;
}

void sequencer_on_tick(uint16_t tick_in_bar)
{
    pattern_poll_variation(tick_in_bar);

    const uint8_t delay = beatbox_swing_delay_ticks(pattern_swing());
    const pattern_bank_t *bank = pattern_active_bank();
    if (bank == NULL || s_note_cb == NULL) {
        return;
    }

    for (uint8_t step = 0; step < BEATBOX_STEPS_PER_BAR; ++step) {
        uint16_t target = (uint16_t)step * BEATBOX_TICKS_PER_16TH;
        if (step & 1u) {
            target = (uint16_t)(target + delay);
        }
        if (target != tick_in_bar) {
            continue;
        }
        for (uint8_t track = 0; track < BEATBOX_TRACK_COUNT; ++track) {
            const uint8_t vel = bank->vel[track][step];
            if (vel == 0) {
                continue;
            }
            s_note_cb(beatbox_track_note(track), vel);
        }
    }
}
