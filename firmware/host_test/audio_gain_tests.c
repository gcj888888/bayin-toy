#include "audio_gain.h"

#include <assert.h>
#include <stdio.h>

/*
 * 音量增益单极点平滑的纯逻辑测试（消除调音量的 zipper 阶跃杂音）。
 * 断言：目标映射正确、平滑步单调且不过冲、有限步内收敛到目标、首步非瞬跳。
 */

static void test_target_map(void)
{
    assert(audio_gain_target_q15(0, 127) == 0);
    assert(audio_gain_target_q15(127, 127) == 32768); /* 最大音量 → 1.0 */
    assert(audio_gain_target_q15(-5, 127) == 0);      /* 下溢 */
    assert(audio_gain_target_q15(300, 127) == 32768); /* 上溢 */
    const int32_t t = audio_gain_target_q15(100, 127);
    assert(t > 25000 && t < 27000); /* ≈0.79 */
    assert(audio_gain_target_q15(50, 0) == 0); /* max 非法 → 0，不崩 */
}

/* 上行：0 → 目标，单调不减、不过冲、约几十步内收敛到 ±1 LSB */
static void test_single_pole_up(void)
{
    const int32_t target = audio_gain_target_q15(100, 127);
    const int32_t first = audio_gain_step_q15(0, target, AUDIO_GAIN_SMOOTH_K_Q15);
    assert(first > 0 && first < target / 2); /* 平滑：首步不是瞬跳到目标 */

    int32_t g = 0;
    int32_t prev = -1;
    int steps = 0;
    for (; steps < 500; ++steps) {
        g = audio_gain_step_q15(g, target, AUDIO_GAIN_SMOOTH_K_Q15);
        assert(g >= 0 && g <= target); /* 单调上行且不过冲 */
        if (g == prev) {
            break; /* 收敛 */
        }
        prev = g;
    }
    assert(steps > 1 && steps < 200);
    assert(target - g <= 4); /* 收敛到目标附近（定点 stall 残差 ≤ 1/k ≈ 3 LSB） */
}

/* 下行：大 → 0，单调不增、不下穿、收敛 */
static void test_single_pole_down(void)
{
    const int32_t start = 32768;
    int32_t g = start;
    int32_t prev = -1;
    int steps = 0;
    for (; steps < 500; ++steps) {
        g = audio_gain_step_q15(g, 0, AUDIO_GAIN_SMOOTH_K_Q15);
        assert(g >= 0 && g <= start);
        if (g == prev) {
            break;
        }
        prev = g;
    }
    assert(steps > 1 && steps < 200);
    assert(g <= 4); /* 收到 0 附近（定点 stall 残差 ≤ ~3 LSB） */
}

int main(void)
{
    test_target_map();
    test_single_pole_up();
    test_single_pole_down();

    printf("audio_gain_tests: all passed (target map + single-pole convergence)\n");
    return 0;
}
