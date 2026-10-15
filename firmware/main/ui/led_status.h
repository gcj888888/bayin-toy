#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_status_init(void);
esp_err_t led_status_set_solid_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * 逐像素整帧刷新（八音琴用）。
 * rgb15 为 BOARD_WS2812_COUNT*3 字节的逻辑 RGB 缓冲：第 i 颗 = [i*3],[i*3+1],[i*3+2]，
 * (0,0,0) 表示该颗熄灭。用于实现"只亮当前槽位 D_N 颜色随音符变 / 空闲亮几颗=数量"。
 */
esp_err_t led_status_set_frame(const uint8_t *rgb15);

/**
 * Trigger the next beat position on the five-pixel ping-pong path.
 */
void led_status_on_beat(uint8_t step, bool is_accent, int64_t now_us);

/** Render a smooth light-transfer animation; call roughly every 10–20 ms. */
esp_err_t led_status_update(int64_t now_us, uint16_t bpm, bool running);

/**
 * Runtime LED peak brightness (0-255) for the beat animation.
 * Lets the host UI dim the strip without rebuilding or reflashing firmware.
 */
void led_status_set_brightness(uint8_t brightness);
uint8_t led_status_get_brightness(void);

/** Briefly visualize the BPM range after an encoder turn. */
void led_status_show_tempo(uint16_t bpm, int64_t now_us);

esp_err_t led_status_clear(void);

#ifdef __cplusplus
}
#endif
