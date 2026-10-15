/* 宿主测试用的 led_status 替身：learning.c 在“目标灯/演示”里调用
 * led_status_set_solid_rgb。测试顺手记录最后一次点亮的目标 RGB，用来核对
 * “学习模式亮的是下一个音的颜色”。 */
#include "led_status.h"

#include <stdint.h>

uint8_t g_last_r;
uint8_t g_last_g;
uint8_t g_last_b;
int g_led_called;

/* 逐像素整帧：记录最后帧，供测试核对（未使用则为全 0）。 */
uint8_t g_last_frame[15];

esp_err_t led_status_init(void) { return 0; }

esp_err_t led_status_set_solid_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    g_last_r = r;
    g_last_g = g;
    g_last_b = b;
    g_led_called = 1;
    return 0;
}

esp_err_t led_status_set_frame(const uint8_t *rgb15)
{
    if (rgb15 == NULL) {
        return 0;
    }
    for (int i = 0; i < 15; ++i) {
        g_last_frame[i] = rgb15[i];
    }
    return 0;
}

void led_status_on_beat(uint8_t step, bool is_accent, int64_t now_us)
{
    (void)step; (void)is_accent; (void)now_us;
}

esp_err_t led_status_update(int64_t now_us, uint16_t bpm, bool running)
{
    (void)now_us; (void)bpm; (void)running;
    return 0;
}

void led_status_set_brightness(uint8_t brightness) { (void)brightness; }
uint8_t led_status_get_brightness(void) { return 80; }

void led_status_show_tempo(uint16_t bpm, int64_t now_us)
{
    (void)bpm; (void)now_us;
}

esp_err_t led_status_clear(void) { return 0; }
