#include "tempo.h"

#define TEMPO_BPM_MIN 60
#define TEMPO_BPM_MAX 240

/*
 * Product transport state only. Timing lives exclusively in audio_click.c,
 * where it is derived from the I2S sample clock.
 */
static uint16_t s_bpm = 120;
static bool s_running;

static uint16_t clamp_bpm(uint16_t bpm)
{
    if (bpm < TEMPO_BPM_MIN) {
        return TEMPO_BPM_MIN;
    }
    if (bpm > TEMPO_BPM_MAX) {
        return TEMPO_BPM_MAX;
    }
    return bpm;
}

esp_err_t tempo_init(uint16_t bpm)
{
    s_bpm = clamp_bpm(bpm);
    s_running = false;
    return ESP_OK;
}

void tempo_set_bpm(uint16_t bpm)
{
    s_bpm = clamp_bpm(bpm);
}

uint16_t tempo_get_bpm(void)
{
    return s_bpm;
}

void tempo_set_running(bool running)
{
    s_running = running;
}

bool tempo_is_running(void)
{
    return s_running;
}
