#pragma once

#include <stdint.h>

/*
 * 音量增益的单极点平滑（纯逻辑，宿主测试覆盖；audio_click.c 引用）。
 *
 * ★ 为什么：调音量时混音增益若从 A 直接跳到 B，会在阶跃处产生 "zipper" 杂音
 *   （旧实现/Beatbox 都是逐样本 `mixed*volume/MAX` 的**阶跃**应用）。
 *   这里按块推进一个平滑增益 gain += (target-gain)*k，时间常数 ~5~20ms。
 *
 * 全部用 Q15 定点：1.0 == 32768。
 */

/* 平滑系数 k（Q15）：块长 128/32000 ≈ 4ms，τ≈10ms → k ≈ 1-e^(-0.4) ≈ 0.33 */
#define AUDIO_GAIN_SMOOTH_K_Q15 10813

static inline int32_t audio_gain_step_q15(int32_t cur_q15, int32_t target_q15, int32_t k_q15)
{
    return cur_q15 + (((target_q15 - cur_q15) * k_q15) >> 15);
}

/* 目标增益：volume(0~127) → Q15 */
static inline int32_t audio_gain_target_q15(int volume, int max)
{
    if (volume < 0) {
        volume = 0;
    }
    if (max <= 0) {
        return 0;
    }
    if (volume > max) {
        volume = max;
    }
    return (int32_t)((int64_t)volume * 32768 / max);
}
