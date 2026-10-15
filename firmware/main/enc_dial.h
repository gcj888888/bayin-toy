#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 旋钮语义（规格变更后）与亮度落盘时机（纯逻辑，宿主测试覆盖）。
 *
 *   - 主目录(PIANO_MODE_MENU)：旋转 = 灯亮度（对 WS2812 D1~D5 与状态灯同时生效）
 *   - 其它模式：旋转 = 音量
 *   - 亮度掉电记忆：复用 NVS "piano"/"brightness"，但**绝不每格写**——
 *     脏标记 + 停转 settle_ms 后一次性落盘（或离开主目录时立即落盘）。
 */

/* 每格亮度步长 / 亮度范围（集中定义） */
#define ENC_BRIGHTNESS_STEP 16
#define ENC_BRIGHTNESS_MIN 8 /* 最低也留一点亮，避免全灭看不见 */
#define ENC_BRIGHTNESS_MAX 255
#define ENC_BRIGHTNESS_SETTLE_MS 1500u /* 停转多久后落盘 */
#define ENC_VOLUME_BEEP_SETTLE_MS 300u /* 音量反馈：停转 ~300ms 后只播一声 */

typedef enum {
    ENC_DIAL_VOLUME = 0,
    ENC_DIAL_BRIGHTNESS,
} enc_dial_target_t;

/* 主目录 → 亮度；其它模式 → 音量 */
enc_dial_target_t enc_dial_target(bool in_menu);

/* 亮度钳位到 [ENC_BRIGHTNESS_MIN, ENC_BRIGHTNESS_MAX] */
uint8_t enc_brightness_clamp(int value);

/* ---- 亮度落盘时机状态机 ---- */
typedef struct {
    bool dirty;             /* 有未落盘的亮度改动 */
    uint32_t last_change_ms; /* 最后一次改动的时刻 */
} enc_flush_t;

void enc_flush_init(enc_flush_t *f);

/* 亮度被改动：置脏并刷新时刻（不落盘） */
void enc_flush_mark(enc_flush_t *f, uint32_t now_ms);

/* 停转 ≥ settle_ms：返回 true 且清脏（调用方此时落盘一次）；否则 false */
bool enc_flush_poll(enc_flush_t *f, uint32_t now_ms, uint32_t settle_ms);

/* 离开主目录：有脏则返回 true 且清脏（立即落盘）；无脏返回 false */
bool enc_flush_on_leave_menu(enc_flush_t *f);

/* ---- 音量反馈音（节流）----
 * 用与落盘同形的 enc_flush_t 计时：每次音量改动 enc_flush_mark 一下（只标脏），
 * 停转 ≥ settle_ms 后由 enc_beep_poll 恰好触发一声——**不逐格、不在余音中重触发**。
 * emit 由上层注入：**必须是"合成短音"通道**（audio_click_play_pitch），
 * **不得走 play_note**（那条会改 s_last_play_note → LED 跟着闪，属副作用）。
 * 返回本次是否发声（true = 已调用 emit 一次）。 */
typedef void (*enc_beep_emit_fn)(void);
bool enc_beep_poll(enc_flush_t *st, uint32_t now_ms, uint32_t settle_ms, enc_beep_emit_fn emit);

/* ---- 「按键因语音忙被丢弃」的反馈节流（纯逻辑）----
 * 方案 A：丢弃时 D3 快闪一次（不碰音频，故不会经"让位"规则掐断正在播的提示语音；
 * 也不用短音——那会打断提示语音）。用时间闸限制"每 min_ms 最多闪一次"，连按不刷屏。 */
#define FEEDBACK_DROP_MIN_MS 200u

typedef struct {
    uint32_t next_allowed_ms;
} feedback_gate_t;

void feedback_gate_init(feedback_gate_t *g);
/* 距上次 ≥ min_ms（首次立即允许）→ 返回 true 并把闸门推到 now+min_ms；否则 false */
bool feedback_gate_allow(feedback_gate_t *g, uint32_t now_ms, uint32_t min_ms);

#ifdef __cplusplus
}
#endif
