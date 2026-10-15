#include "enc_dial.h"

#include <stddef.h>

/* 纯逻辑实现：不依赖任何 ESP-IDF 组件，可直接在电脑上编译测试。 */

enc_dial_target_t enc_dial_target(bool in_menu)
{
    return in_menu ? ENC_DIAL_BRIGHTNESS : ENC_DIAL_VOLUME;
}

uint8_t enc_brightness_clamp(int value)
{
    if (value < ENC_BRIGHTNESS_MIN) {
        return ENC_BRIGHTNESS_MIN;
    }
    if (value > ENC_BRIGHTNESS_MAX) {
        return ENC_BRIGHTNESS_MAX;
    }
    return (uint8_t)value;
}

void enc_flush_init(enc_flush_t *f)
{
    if (f == NULL) {
        return;
    }
    f->dirty = false;
    f->last_change_ms = 0;
}

void enc_flush_mark(enc_flush_t *f, uint32_t now_ms)
{
    if (f == NULL) {
        return;
    }
    f->dirty = true;
    f->last_change_ms = now_ms;
}

bool enc_flush_poll(enc_flush_t *f, uint32_t now_ms, uint32_t settle_ms)
{
    if (f == NULL || !f->dirty) {
        return false;
    }
    /* 无符号回绕安全：(now - last) 天然可取模比较 */
    if ((uint32_t)(now_ms - f->last_change_ms) >= settle_ms) {
        f->dirty = false;
        return true;
    }
    return false;
}

bool enc_flush_on_leave_menu(enc_flush_t *f)
{
    if (f == NULL || !f->dirty) {
        return false;
    }
    f->dirty = false;
    return true;
}

bool enc_beep_poll(enc_flush_t *st, uint32_t now_ms, uint32_t settle_ms, enc_beep_emit_fn emit)
{
    if (st == NULL || !st->dirty) {
        return false;
    }
    /* 停转够久（节流窗口）：清脏并恰好触发一声 */
    if ((uint32_t)(now_ms - st->last_change_ms) >= settle_ms) {
        st->dirty = false;
        if (emit != NULL) {
            emit();
        }
        return true;
    }
    return false;
}

void feedback_gate_init(feedback_gate_t *g)
{
    if (g == NULL) {
        return;
    }
    g->next_allowed_ms = 0; /* 0 → 首次立即允许 */
}

bool feedback_gate_allow(feedback_gate_t *g, uint32_t now_ms, uint32_t min_ms)
{
    if (g == NULL) {
        return false;
    }
    if ((int32_t)(now_ms - g->next_allowed_ms) >= 0) {
        g->next_allowed_ms = now_ms + min_ms;
        return true;
    }
    return false;
}
