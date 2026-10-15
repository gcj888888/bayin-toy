#include "piano_fsm.h"

#include <assert.h>
#include <stdio.h>

/*
 * 八音琴五态状态机的宿主测试（纯逻辑，不依赖 ESP-IDF，gcc + ctest 直接跑）。
 * 覆盖：开机在主目录 / 五态切换 / 键位映射 / 空闲灯"亮几颗=数量" /
 *       短按上下文动作 / 播放子态。
 */

/* 1) 开机即在主目录；不变量初始为空闲、未记谱、未选槽 */
static void test_boot_in_menu(void)
{
    piano_init();
    assert(piano_get_mode() == PIANO_MODE_MENU);
    assert(piano_get_play() == PIANO_PLAY_IDLE);
    assert(piano_get_selected() == -1);
    assert(!piano_is_recording());
}

/* 2) 主目录 S1~S4 → 4 个子目录；S5~S8 无功能 */
static void test_menu_keys(void)
{
    assert(piano_menu_key_mode(0) == PIANO_MODE_AUDITION);
    assert(piano_menu_key_mode(1) == PIANO_MODE_SELF);
    assert(piano_menu_key_mode(2) == PIANO_MODE_GAME);
    assert(piano_menu_key_mode(3) == PIANO_MODE_PLAYBACK);
    assert(piano_menu_key_mode(4) == PIANO_MODE_COUNT);
    assert(piano_menu_key_mode(5) == PIANO_MODE_COUNT);
    assert(piano_menu_key_mode(6) == PIANO_MODE_COUNT);
    assert(piano_menu_key_mode(7) == PIANO_MODE_COUNT);
    assert(piano_menu_key_mode(-1) == PIANO_MODE_COUNT);
}

/* 3) 试听/回放 S1~S5 → 槽 0~4；S6~S8 无功能 */
static void test_slot_keys(void)
{
    assert(piano_slot_key(0) == 0);
    assert(piano_slot_key(1) == 1);
    assert(piano_slot_key(2) == 2);
    assert(piano_slot_key(3) == 3);
    assert(piano_slot_key(4) == 4);
    assert(piano_slot_key(5) == -1);
    assert(piano_slot_key(6) == -1);
    assert(piano_slot_key(7) == -1);
    assert(piano_slot_key(-1) == -1);
}

/* 4) 自弹 S1~S8 → 0~7（哆~高音哆） */
static void test_note_keys(void)
{
    for (int i = 0; i < 8; ++i) {
        assert(piano_note_key(i) == i);
    }
    assert(piano_note_key(8) == -1);
    assert(piano_note_key(-1) == -1);
}

/* 5) 空闲灯"亮几颗 = 数量"：主目录 4；试听/回放 min(used,5)；自弹/游戏 0 */
static void test_idle_leds(void)
{
    assert(piano_idle_leds(PIANO_MODE_MENU, 0) == 4);
    assert(piano_idle_leds(PIANO_MODE_MENU, 5) == 4);

    assert(piano_idle_leds(PIANO_MODE_AUDITION, 0) == 0);
    assert(piano_idle_leds(PIANO_MODE_AUDITION, 3) == 3);
    assert(piano_idle_leds(PIANO_MODE_AUDITION, 5) == 5);
    assert(piano_idle_leds(PIANO_MODE_AUDITION, 9) == 5);
    assert(piano_idle_leds(PIANO_MODE_AUDITION, -2) == 0);

    assert(piano_idle_leds(PIANO_MODE_PLAYBACK, 2) == 2);
    assert(piano_idle_leds(PIANO_MODE_SELF, 5) == 0); /* 未记谱不亮音色灯 */
    assert(piano_idle_leds(PIANO_MODE_GAME, 5) == 0); /* 由网页呈现 */
}

/* 6) 只有试听/自弹/回放是"只亮当前那颗" */
static void test_single_led_modes(void)
{
    assert(piano_shows_single_led(PIANO_MODE_AUDITION));
    assert(piano_shows_single_led(PIANO_MODE_SELF));
    assert(piano_shows_single_led(PIANO_MODE_PLAYBACK));
    assert(!piano_shows_single_led(PIANO_MODE_MENU));
    assert(!piano_shows_single_led(PIANO_MODE_GAME));
}

/* 7) 短按上下文动作 */
static void test_short_press_actions(void)
{
    assert(piano_short_press_action(PIANO_MODE_MENU, PIANO_PLAY_IDLE, false) ==
           PIANO_SHORT_MENU_REPROMPT);
    assert(piano_short_press_action(PIANO_MODE_AUDITION, PIANO_PLAY_PLAYING, false) ==
           PIANO_SHORT_TOGGLE_PAUSE);
    assert(piano_short_press_action(PIANO_MODE_PLAYBACK, PIANO_PLAY_IDLE, false) ==
           PIANO_SHORT_TOGGLE_PAUSE);
    assert(piano_short_press_action(PIANO_MODE_SELF, PIANO_PLAY_IDLE, false) ==
           PIANO_SHORT_TOGGLE_RECORD);
    assert(piano_short_press_action(PIANO_MODE_SELF, PIANO_PLAY_IDLE, true) ==
           PIANO_SHORT_TOGGLE_RECORD);
    assert(piano_short_press_action(PIANO_MODE_GAME, PIANO_PLAY_IDLE, false) ==
           PIANO_SHORT_INVALID_PROMPT);
}

/* 8) 进子目录 / 回主目录 的切换与复位 */
static void test_transitions(void)
{
    piano_init();
    assert(piano_enter_mode(PIANO_MODE_SELF));
    assert(piano_get_mode() == PIANO_MODE_SELF);
    piano_set_recording(true);
    piano_set_selected(2);
    assert(piano_is_recording());

    /* 长按 ≥5s = 回主目录：清空子态 */
    piano_enter_menu();
    assert(piano_get_mode() == PIANO_MODE_MENU);
    assert(!piano_is_recording());
    assert(piano_get_selected() == -1);
    assert(piano_get_play() == PIANO_PLAY_IDLE);

    /* 非法模式不切换 */
    assert(!piano_enter_mode(PIANO_MODE_COUNT));
    assert(piano_get_mode() == PIANO_MODE_MENU);
}

/* 9) 播放子态往返 */
static void test_play_states(void)
{
    piano_init();
    piano_enter_mode(PIANO_MODE_PLAYBACK);
    piano_set_selected(1);
    piano_set_play(PIANO_PLAY_PLAYING);
    assert(piano_get_play() == PIANO_PLAY_PLAYING);
    piano_set_play(PIANO_PLAY_PAUSED);
    assert(piano_get_play() == PIANO_PLAY_PAUSED);
    piano_set_play(PIANO_PLAY_IDLE);
    assert(piano_get_play() == PIANO_PLAY_IDLE);
    /* 非法子态被忽略 */
    piano_set_play((piano_play_t)99);
    assert(piano_get_play() == PIANO_PLAY_IDLE);
    assert(piano_get_selected() == 1);
}

/* 10) 语音闩锁：一次状态迁移/事件恰好播 1 句（取走即清，绝不重复） */
static void test_voice_latch(void)
{
    piano_init();
    /* 开机在主目录 → 恰好一句"请选择模式" */
    assert(piano_take_voice() == PROMPT_SELECT_MODE);
    assert(piano_take_voice() == -1); /* 已取走，不再重复 */

    /* 进子目录 → 恰好一句模式名 */
    assert(piano_enter_mode(PIANO_MODE_SELF));
    assert(piano_take_voice() == PROMPT_MODE_SELF);
    assert(piano_take_voice() == -1);

    /* 回主目录 → 恰好一句"请选择模式" */
    piano_enter_menu();
    assert(piano_take_voice() == PROMPT_SELECT_MODE);
    assert(piano_take_voice() == -1);

    /* 事件登记（选曲/记谱/无功能…）：恰好一句 */
    piano_request_voice(PROMPT_NO_RECORD);
    assert(piano_take_voice() == PROMPT_NO_RECORD);
    assert(piano_take_voice() == -1);

    /* 覆盖式：连续登记只保留最后一句（song_play 先进模式再报歌名） */
    piano_request_voice(PROMPT_MODE_TRIAL);
    piano_request_voice(PROMPT_SONG_TWINKLE);
    assert(piano_take_voice() == PROMPT_SONG_TWINKLE);
    assert(piano_take_voice() == -1);

    /* 非法 id 忽略 */
    piano_request_voice((prompt_id_t)PROMPT_COUNT);
    assert(piano_take_voice() == -1);

    /* 暂停/继续不登记任何语音 → 不重播曲名 */
    piano_enter_mode(PIANO_MODE_AUDITION);
    assert(piano_take_voice() == PROMPT_MODE_TRIAL); /* 进模式那一句 */
    piano_set_play(PIANO_PLAY_PLAYING);
    piano_set_play(PIANO_PLAY_PAUSED);
    piano_set_play(PIANO_PLAY_PLAYING); /* 暂停→继续 */
    assert(piano_take_voice() == -1);   /* 没有任何新语音 */
}

/* 11) 语音忙时只丢弃 Pad；旋钮（含长按回主目录）不受影响 */
static void test_pad_blocked_by_voice(void)
{
    assert(piano_pad_blocked_by_voice(true));  /* 语音播报中 → Pad 丢弃 */
    assert(!piano_pad_blocked_by_voice(false)); /* 空闲 → Pad 可用 */
    /* 长按回主目录不查询本函数（见 main 的 handle_encoder_gesture），故 busy 时仍生效 */
}

int main(void)
{
    test_boot_in_menu();
    test_menu_keys();
    test_slot_keys();
    test_note_keys();
    test_idle_leds();
    test_single_led_modes();
    test_short_press_actions();
    test_transitions();
    test_play_states();
    test_voice_latch();
    test_pad_blocked_by_voice();

    printf("piano_fsm_tests: all passed (boot/menu/keys/leds/short/transitions/play/voice)\n");
    return 0;
}
