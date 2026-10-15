#include "voice_src.h"

#include <assert.h>
#include <stdio.h>

/*
 * 混音声道"源选择"规则的宿主测试（纯逻辑）。
 *
 * ★ 这是真机"语音反复播报"根因的回归闸门：只要有人把判定改回 "只看 ext"，
 *   本用例立即变红（见 test_reused_slot_no_pcm）。
 */

/* 1) 真值表：只有 PCM 且确实有外部缓冲时才走 PCM 路径 */
static void test_truth_table(void)
{
    assert(voice_src_uses_external(true, true));    /* PCM + 有缓冲 → 播缓冲 */
    assert(!voice_src_uses_external(true, false));  /* PCM 但无缓冲 → 不播（退回采样） */
    assert(!voice_src_uses_external(false, true));  /* ★ 非 PCM：即使残留 ext 也绝不播缓冲 */
    assert(!voice_src_uses_external(false, false));
}

/* 2) 复现真机 bug 场景：PCM 声道归还后被音符复用（残留 ext 指针） */
static void test_reused_slot_no_pcm(void)
{
    /* 语音声道播放：is_pcm=true, has_ext=true → 正常播语音 */
    bool is_pcm = true;
    bool has_ext = true;
    assert(voice_src_uses_external(is_pcm, has_ext));

    /* 语音播完，声道归还；旧实现不清 ext（has_ext 仍为 true）。
     * 音符复用该声道：is_pcm 变 false。 */
    is_pcm = false;
    /* has_ext 仍 true —— 模拟"残留指针" */
    assert(!voice_src_uses_external(is_pcm, has_ext)); /* ★ 必须不把音符当语音播 */

    /* 采样音启动应把 ext 清空 → 状态归零 */
    has_ext = false;
    assert(!voice_src_uses_external(is_pcm, has_ext));
}

int main(void)
{
    test_truth_table();
    test_reused_slot_no_pcm();

    printf("voice_src_tests: all passed (source-selection invariant; guards PCM-reuse bug)\n");
    return 0;
}
