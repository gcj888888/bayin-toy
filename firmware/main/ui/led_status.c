#include "led_status.h"

#include "board_pins.h"
#include "board_power.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led_status";
static led_strip_handle_t s_strip;
static int64_t s_last_beat_us;
static int64_t s_tempo_preview_until_us;
static uint8_t s_step;
static bool s_accent;
static uint16_t s_preview_bpm;
/* Peak scale for the beat animation AND the solid note colour; runtime
 * adjustable from the host UI (webpage / PC). Default softened after user
 * feedback that the keys' D1-D5 were too bright. */
static uint8_t s_brightness = 80;

static uint8_t scale_u8(uint8_t value, uint8_t scale)
{
    return (uint8_t)(((uint16_t)value * scale) / 255u);
}

static esp_err_t set_rgb(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t scale)
{
    return led_strip_set_pixel(s_strip, index, scale_u8(r, scale), scale_u8(g, scale),
                               scale_u8(b, scale));
}

/* 0 → 1 → 2 → 3 → 4 → 3 → 2 → 1, then repeat. */
static uint8_t path_index(uint8_t step)
{
    const uint8_t phase = step & 0x07;
    return phase <= 4 ? phase : (uint8_t)(8 - phase);
}

esp_err_t led_status_init(void)
{
    ESP_RETURN_ON_FALSE(board_power_peripherals_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "enable GPIO8 before LEDs");

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_GPIO_LED_DIN,
        .max_leds = BOARD_WS2812_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG,
                        "led_strip_new_rmt_device failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear failed");
    ESP_LOGI(TAG, "WS2812 ready (%d pixels, spring + breath animation)", BOARD_WS2812_COUNT);
    return ESP_OK;
}

esp_err_t led_status_set_solid_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    for (int i = 0; i < BOARD_WS2812_COUNT; ++i) {
        /* Apply the master brightness so the note colour is dimmable from
         * the host UI, not just the beat animation. */
        ESP_RETURN_ON_ERROR(set_rgb(i, r, g, b, s_brightness), TAG, "set_pixel");
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_status_set_frame(const uint8_t *rgb15)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    if (rgb15 == NULL) {
        return led_status_clear();
    }
    for (int i = 0; i < BOARD_WS2812_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(set_rgb(i, rgb15[i * 3 + 0], rgb15[i * 3 + 1], rgb15[i * 3 + 2],
                                    s_brightness),
                            TAG, "set_pixel");
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_status_clear(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    return led_strip_clear(s_strip);
}

void led_status_on_beat(uint8_t step, bool is_accent, int64_t now_us)
{
    s_step = step;
    s_accent = is_accent;
    s_last_beat_us = now_us;
}

void led_status_show_tempo(uint16_t bpm, int64_t now_us)
{
    s_preview_bpm = bpm;
    s_tempo_preview_until_us = now_us + 450000;
}

esp_err_t led_status_update(int64_t now_us, uint16_t bpm, bool running)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear");

    if (now_us < s_tempo_preview_until_us) {
        /* One short amber marker; never turn the whole strip into a bar. */
        uint16_t normalized = s_preview_bpm > 60 ? (uint16_t)(s_preview_bpm - 60) : 0;
        if (normalized > 180) {
            normalized = 180;
        }
        const uint8_t marker = (uint8_t)((normalized * 4u + 90u) / 180u);
        ESP_RETURN_ON_ERROR(set_rgb(marker, 255, 64, 0, 72), TAG, "tempo marker");
        return led_strip_refresh(s_strip);
    }

    if (!running) {
        /* A stopped instrument is visually quiet. */
        return led_strip_refresh(s_strip);
    }

    const int64_t period_us = 60000000LL / (bpm < 60 ? 60 : bpm);
    int64_t elapsed = now_us - s_last_beat_us;
    if (elapsed < 0) {
        elapsed = 0;
    }
    if (elapsed > period_us) {
        elapsed = period_us;
    }

    /*
     * Smoothstep crossfade preserves perceived energy: light appears to move
     * between adjacent pixels instead of one pixel snapping off and another
     * snapping on. Only those two pixels are lit.
     */
    const float phase = (float)elapsed / (float)period_us;
    const float transfer = phase * phase * (3.0f - 2.0f * phase);
    const uint8_t from_level = (uint8_t)((float)s_brightness * (1.0f - transfer));
    const uint8_t to_level = (uint8_t)((float)s_brightness * transfer);
    const uint8_t from = path_index(s_step);
    const uint8_t to = path_index((uint8_t)(s_step + 1));

    if (s_accent && phase < 0.35f) {
        ESP_RETURN_ON_ERROR(set_rgb(from, 255, 105, 10, from_level), TAG, "accent transfer");
    } else {
        ESP_RETURN_ON_ERROR(set_rgb(from, 255, 38, 0, from_level), TAG, "from transfer");
    }
    ESP_RETURN_ON_ERROR(set_rgb(to, 255, 38, 0, to_level), TAG, "to transfer");
    return led_strip_refresh(s_strip);
}

void led_status_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

uint8_t led_status_get_brightness(void)
{
    return s_brightness;
}
