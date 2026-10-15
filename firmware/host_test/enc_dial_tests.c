#include "enc_dial.h"

#include <assert.h>
#include <stdio.h>

/*
 * 八音琴：旋钮语义分流 + 亮度钳位 + NVS 脏标记落盘时机（纯逻辑）。
 * 规格：主目录旋转=亮度（掉电记忆，但绝不每格写 NVS），其它模式旋转=音量。
 */

/* 1) 模式 → 旋转目标：主目录=亮度，其它=音量 */
static void test_target_by_mode(void)
{
    assert(enc_dial_target(true) == ENC_DIAL_BRIGHTNESS);   /* PIANO_MODE_MENU */
    assert(enc_dial_target(false) == ENC_DIAL_VOLUME);      /* 试听/自弹/游戏/回放 */
}

/* 2) 亮度钳位 + 步长/范围常量合理 */
static void test_brightness_clamp_and_step(void)
{
    assert(ENC_BRIGHTNESS_STEP > 0);
    assert(ENC_BRIGHTNESS_MIN > 0);                 /* 留一点亮 */
    assert(ENC_BRIGHTNESS_MIN <= ENC_BRIGHTNESS_MAX);

    assert(enc_brightness_clamp(-100) == ENC_BRIGHTNESS_MIN);
    assert(enc_brightness_clamp(0) == ENC_BRIGHTNESS_MIN);
    assert(enc_brightness_clamp(ENC_BRIGHTNESS_MIN) == ENC_BRIGHTNESS_MIN);
    assert(enc_brightness_clamp(100) == 100);
    assert(enc_brightness_clamp(ENC_BRIGHTNESS_MAX) == ENC_BRIGHTNESS_MAX);
    assert(enc_brightness_clamp(300) == ENC_BRIGHTNESS_MAX);

    /* 模拟连续旋转：单调逼近上限/下限并被钳位 */
    int v = 80;
    for (int i = 0; i < 20; ++i) {
        v = enc_brightness_clamp(v + ENC_BRIGHTNESS_STEP);
    }
    assert(v == ENC_BRIGHTNESS_MAX);
    for (int i = 0; i < 20; ++i) {
        v = enc_brightness_clamp(v - ENC_BRIGHTNESS_STEP);
    }
    assert(v == ENC_BRIGHTNESS_MIN);
}

/* 3) 脏标记：停转 settle_ms 后一次性落盘（绝不每格写） */
static void test_flush_settle(void)
{
    enc_flush_t f;
    enc_flush_init(&f);
    assert(!f.dirty);
    assert(!enc_flush_poll(&f, 12345, ENC_BRIGHTNESS_SETTLE_MS)); /* 无脏 → 不落盘 */

    enc_flush_mark(&f, 1000);
    assert(f.dirty);
    /* 停转未够：不落盘 */
    assert(!enc_flush_poll(&f, 1000 + ENC_BRIGHTNESS_SETTLE_MS - 1, ENC_BRIGHTNESS_SETTLE_MS));
    assert(f.dirty);
    /* 恰好够：落盘一次并清脏 */
    assert(enc_flush_poll(&f, 1000 + ENC_BRIGHTNESS_SETTLE_MS, ENC_BRIGHTNESS_SETTLE_MS));
    assert(!f.dirty);
    /* 已清脏：不再重复落盘 */
    assert(!enc_flush_poll(&f, 1000 + ENC_BRIGHTNESS_SETTLE_MS + 100, ENC_BRIGHTNESS_SETTLE_MS));

    /* 连续旋转：每次都刷新计时，只在最后一次停转后落盘（1 次） */
    enc_flush_init(&f);
    enc_flush_mark(&f, 0);
    enc_flush_mark(&f, 200);
    enc_flush_mark(&f, 400);
    assert(!enc_flush_poll(&f, 400 + ENC_BRIGHTNESS_SETTLE_MS - 1, ENC_BRIGHTNESS_SETTLE_MS));
    assert(enc_flush_poll(&f, 400 + ENC_BRIGHTNESS_SETTLE_MS, ENC_BRIGHTNESS_SETTLE_MS));
    assert(!f.dirty);
}

/* 4) 离开主目录：有脏立即落盘一次并清脏 */
static void test_flush_on_leave_menu(void)
{
    enc_flush_t f;
    enc_flush_init(&f);
    assert(!enc_flush_on_leave_menu(&f)); /* 无脏 → 不落盘 */

    enc_flush_mark(&f, 500);
    assert(enc_flush_on_leave_menu(&f));  /* 有脏 → 立即落盘一次 */
    assert(!f.dirty);
    assert(!enc_flush_on_leave_menu(&f)); /* 已清脏，不再落盘 */

    /* NULL 安全 */
    assert(!enc_flush_on_leave_menu(NULL));
    assert(!enc_flush_poll(NULL, 0, 100));
    enc_flush_mark(NULL, 0);
    enc_flush_init(NULL);
}

/* 5) 音量反馈音：停转后恰好一声（节流），不逐格/不在余音中重触发 */
static int g_beeps;
static void beep_cb(void)
{
    g_beeps++;
}

static void test_volume_beep_throttle(void)
{
    assert(ENC_VOLUME_BEEP_SETTLE_MS >= 250u && ENC_VOLUME_BEEP_SETTLE_MS <= 500u);

    enc_flush_t b;
    enc_flush_init(&b);
    g_beeps = 0;

    /* 快速旋转 5 格（间隔 40ms）——每次都只标脏，绝不逐格发声 */
    for (uint32_t t = 0; t <= 160; t += 40) {
        enc_flush_mark(&b, t);
        assert(!enc_beep_poll(&b, t, ENC_VOLUME_BEEP_SETTLE_MS, beep_cb)); /* 旋转中：0 声 */
    }
    assert(g_beeps == 0);
    /* 停转未够：仍不发声 */
    assert(!enc_beep_poll(&b, 160 + ENC_VOLUME_BEEP_SETTLE_MS - 1, ENC_VOLUME_BEEP_SETTLE_MS,
                          beep_cb));
    assert(g_beeps == 0);
    /* 停转够：恰好一声 */
    assert(enc_beep_poll(&b, 160 + ENC_VOLUME_BEEP_SETTLE_MS, ENC_VOLUME_BEEP_SETTLE_MS, beep_cb));
    assert(g_beeps == 1);
    /* 已清脏：不再重复 */
    assert(!enc_beep_poll(&b, 160 + ENC_VOLUME_BEEP_SETTLE_MS + 1000, ENC_VOLUME_BEEP_SETTLE_MS,
                          beep_cb));
    assert(g_beeps == 1);

    /* 第二轮旋转：settle 后再来一声（累计 2） */
    enc_flush_mark(&b, 1000);
    enc_flush_mark(&b, 1040);
    assert(!enc_beep_poll(&b, 1040 + 100, ENC_VOLUME_BEEP_SETTLE_MS, beep_cb));
    assert(g_beeps == 1);
    assert(enc_beep_poll(&b, 1040 + ENC_VOLUME_BEEP_SETTLE_MS, ENC_VOLUME_BEEP_SETTLE_MS, beep_cb));
    assert(g_beeps == 2);

    /* emit 可为 NULL（不崩、仍返回 true）；无脏时不触发 */
    enc_flush_mark(&b, 5000);
    assert(enc_beep_poll(&b, 5000 + ENC_VOLUME_BEEP_SETTLE_MS, ENC_VOLUME_BEEP_SETTLE_MS, NULL));
    assert(!enc_beep_poll(&b, 9999, ENC_VOLUME_BEEP_SETTLE_MS, beep_cb));
    assert(g_beeps == 2);

    /*
     * 反馈音"无 LED 副作用"由**实现选择**保证：main 的 emit 走
     * audio_click_play_pitch（合成短音通道），**不走 play_note**（play_note 会写
     * s_last_play_note → LED 变色）。本用例通过"注入式 emit 回调"覆盖发声路径，
     * 并断言其只在 settle 时被调用 1 次。
     */
}

/* 6) 丢弃反馈节流：busy 期间连按 → 恰好 1 次反馈（D3 闪） */
static void test_drop_feedback_gate(void)
{
    assert(FEEDBACK_DROP_MIN_MS >= 150u && FEEDBACK_DROP_MIN_MS <= 500u);

    feedback_gate_t g;
    feedback_gate_init(&g);
    assert(feedback_gate_allow(&g, 0, FEEDBACK_DROP_MIN_MS)); /* 首次立即允许 */
    assert(!feedback_gate_allow(&g, 50, FEEDBACK_DROP_MIN_MS));  /* 窗口内 */
    assert(!feedback_gate_allow(&g, FEEDBACK_DROP_MIN_MS - 1, FEEDBACK_DROP_MIN_MS));
    assert(feedback_gate_allow(&g, FEEDBACK_DROP_MIN_MS, FEEDBACK_DROP_MIN_MS)); /* 到期 */
    assert(!feedback_gate_allow(&g, FEEDBACK_DROP_MIN_MS + 10, FEEDBACK_DROP_MIN_MS));

    /* 模拟"busy 期间 8 次连按（每 20ms 一次）"→ 恰好 1 次反馈 */
    feedback_gate_init(&g);
    int fires = 0;
    for (uint32_t t = 1000; t < 1000 + 8 * 20; t += 20) {
        if (feedback_gate_allow(&g, t, FEEDBACK_DROP_MIN_MS)) {
            fires++;
        }
    }
    assert(fires == 1);

    assert(!feedback_gate_allow(NULL, 0, 100));
    feedback_gate_init(NULL);
}

int main(void)
{
    test_target_by_mode();
    test_brightness_clamp_and_step();
    test_flush_settle();
    test_flush_on_leave_menu();
    test_volume_beep_throttle();
    test_drop_feedback_gate();

    printf("enc_dial_tests: all passed (target/clamp/flush/beep/drop-gate)\n");
    return 0;
}
