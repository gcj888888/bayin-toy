#include "piano_fsm.h"

#include <assert.h>
#include <stdio.h>

/*
 * 八音琴：按键场景级宿主测试。
 *
 * 用**合成按键序列**驱动 piano_fsm（含一次性语音闩锁）+ **假 audio 层计数器**
 * （把"实际播出的语音"记下来，模拟 main 的 prompt_play），断言"每个场景恰好播 N 句"。
 * Pad 路由与 main 共用同一个纯函数 piano_pad_event()，避免测试与固件逻辑分叉。
 *
 * 覆盖用户实测缺陷对应的回归：曲名反复播报、暂停/继续重播曲名、
 * 无功能键重复播报、语音忙时 Pad 丢弃、长按回主目录不受 busy 限制。
 */

static int g_played[PROMPT_COUNT];
static int g_total;

/* 假 audio 层：取走待播句并计数（模拟 main 主循环每轮 prompt_play） */
static void fake_audio_tick(void)
{
    const int id = piano_take_voice();
    if (id >= 0) {
        g_played[id]++;
        g_total++;
    }
}

static void reset_all(void)
{
    piano_init();
    for (int i = 0; i < PROMPT_COUNT; ++i) {
        g_played[i] = 0;
    }
    g_total = 0;
    fake_audio_tick(); /* 消费开机"请选择模式"，让计数器从干净状态起算 */
    for (int i = 0; i < PROMPT_COUNT; ++i) {
        g_played[i] = 0;
    }
    g_total = 0;
}

/* 模拟 main 的 handle_pads（边沿已由上层确认）：路由用共享的 piano_pad_event */
static void sim_pad(int key, bool voice_busy)
{
    switch (piano_pad_event(key, voice_busy)) {
    case PIANO_PAD_ENTER_MODE:
        piano_enter_mode(piano_menu_key_mode(key));
        break;
    case PIANO_PAD_PLAY_SLOT: {
        const int slot = piano_slot_key(key);
        piano_set_selected(slot);
        /* main 的 start_slot_playback：内置 3 首报歌名；其它/空槽报"暂无记录" */
        if (slot >= 0 && slot < 3) {
            piano_request_voice((prompt_id_t)(PROMPT_SONG_TWINKLE + slot));
        } else {
            piano_request_voice(PROMPT_NO_RECORD);
        }
        piano_set_play(PIANO_PLAY_PLAYING);
        break;
    }
    case PIANO_PAD_PLAY_NOTE:
        break; /* 只发声，不登记语音 */
    case PIANO_PAD_NO_FUNCTION:
        piano_request_voice(PROMPT_NO_FUNCTION);
        break;
    default: /* NONE / DROPPED */
        break;
    }
    fake_audio_tick();
}

/* 模拟 main 的旋钮短按（上下文动作） */
static void sim_short_press(void)
{
    switch (piano_short_press_action(piano_get_mode(), piano_get_play(), piano_is_recording())) {
    case PIANO_SHORT_MENU_REPROMPT:
        piano_request_voice(PROMPT_SELECT_MODE);
        break;
    case PIANO_SHORT_TOGGLE_PAUSE: {
        const piano_play_t st = piano_get_play();
        if (st == PIANO_PLAY_PLAYING) {
            piano_set_play(PIANO_PLAY_PAUSED);
        } else if (st == PIANO_PLAY_PAUSED) {
            piano_set_play(PIANO_PLAY_PLAYING);
        }
        break;
    }
    case PIANO_SHORT_TOGGLE_RECORD:
        if (piano_is_recording()) {
            piano_set_recording(false);
            piano_request_voice(PROMPT_SAVED);
        } else {
            piano_set_recording(true);
            piano_request_voice(PROMPT_START_RECORD);
        }
        break;
    case PIANO_SHORT_INVALID_PROMPT:
        piano_request_voice(PROMPT_NO_FUNCTION);
        break;
    default:
        break;
    }
    fake_audio_tick();
}

/* 模拟 main 的长按旋钮≥5s → 回主目录（不查 busy，保命手势） */
static void sim_long_press(void)
{
    piano_set_recording(false);
    piano_enter_menu();
    piano_request_voice(PROMPT_BACK_HOME);
    fake_audio_tick();
}

/* 1) 主目录按 S1 → 进试听：恰好播 1 句（模式名） */
static void test_menu_to_trial_exactly_one(void)
{
    reset_all();
    sim_pad(0, false); /* 主目录 S1 → 试听 */
    assert(piano_get_mode() == PIANO_MODE_AUDITION);
    assert(g_played[PROMPT_MODE_TRIAL] == 1);
    assert(g_total == 1);
    /* 再空跑几帧，不得重复播 */
    for (int i = 0; i < 5; ++i) {
        fake_audio_tick();
    }
    assert(g_played[PROMPT_MODE_TRIAL] == 1 && g_total == 1);
}

/* 2) 试听按 S1 选曲：曲名恰好 1 次；短按(暂停)→短按(继续) 不得重播曲名 */
static void test_select_song_once_no_replay_on_pause(void)
{
    reset_all();
    sim_pad(0, false); /* 进试听（播"试听模式"） */
    assert(g_played[PROMPT_MODE_TRIAL] == 1);

    sim_pad(0, false); /* 试听 S1 → 选 slot0，报曲名"小星星" */
    assert(g_played[PROMPT_SONG_TWINKLE] == 1);
    assert(g_total == 2);

    sim_short_press(); /* 暂停 */
    assert(piano_get_play() == PIANO_PLAY_PAUSED);
    assert(g_played[PROMPT_SONG_TWINKLE] == 1 && g_total == 2); /* 不重播曲名 */

    sim_short_press(); /* 继续 */
    assert(piano_get_play() == PIANO_PLAY_PLAYING);
    assert(g_played[PROMPT_SONG_TWINKLE] == 1 && g_total == 2); /* 仍不重播 */

    /* 长时间空跑也不得冒出曲名 */
    for (int i = 0; i < 10; ++i) {
        fake_audio_tick();
    }
    assert(g_played[PROMPT_SONG_TWINKLE] == 1);
}

/* 3) 自弹：短按开始记谱 → 按键 → 短按结束保存：「开始记谱」「已保存」各恰好 1 次 */
static void test_self_record_start_saved_once(void)
{
    reset_all();
    sim_pad(1, false); /* 进自弹（播"自弹模式"） */
    assert(g_played[PROMPT_MODE_SELF] == 1);

    sim_short_press(); /* 开始记谱 */
    assert(piano_is_recording());
    assert(g_played[PROMPT_START_RECORD] == 1);

    sim_pad(3, false); /* 弹一个音（不登记语音） */
    sim_pad(5, false);
    sim_pad(7, false);
    assert(g_total == 2); /* 自弹模式 + 开始记谱 */

    sim_short_press(); /* 结束并保存 */
    assert(!piano_is_recording());
    assert(g_played[PROMPT_SAVED] == 1);
    assert(g_played[PROMPT_START_RECORD] == 1);
    assert(g_total == 3);
}

/* 4) 无功能键："本键无功能"恰好 1 次（不是每帧/每键重复） */
static void test_no_function_exactly_once(void)
{
    reset_all();
    sim_pad(4, false); /* 主目录 S5 → 无功能 */
    assert(g_played[PROMPT_NO_FUNCTION] == 1 && g_total == 1);
    for (int i = 0; i < 5; ++i) {
        fake_audio_tick();
    }
    assert(g_played[PROMPT_NO_FUNCTION] == 1);

    /* 试听里 S6~S8（key5~7）同样各只播一次 */
    reset_all();
    sim_pad(0, false); /* 进试听 */
    sim_pad(5, false); /* S6 → 无功能 */
    assert(g_played[PROMPT_NO_FUNCTION] == 1);
    sim_pad(6, false); /* S7 → 无功能（又一次按下，允许再播一次） */
    assert(g_played[PROMPT_NO_FUNCTION] == 2);
}

/* 5) busy 时 Pad 被丢弃且不排队；长按回主目录不受 busy 限制 */
static void test_busy_drop_and_long_press(void)
{
    reset_all();
    sim_pad(0, false); /* 进试听（此刻"语音忙"由假 audio 层模拟） */
    const int before = g_total;

    /* 语音播报中：Pad 一律 DROPPED（无动作、无语音、不排队） */
    assert(piano_pad_event(0, true) == PIANO_PAD_DROPPED);
    sim_pad(0, true);
    sim_pad(1, true);
    assert(g_total == before);
    assert(piano_take_voice() == -1); /* 没有任何"排队等待"的语音 */

    /* 长按回主目录不查 busy → 照常生效 */
    sim_long_press();
    assert(piano_get_mode() == PIANO_MODE_MENU);
    assert(g_played[PROMPT_BACK_HOME] == 1);

    /* busy 解除后 Pad 恢复可用 */
    reset_all();
    sim_pad(0, false); /* 进试听 */
    assert(g_played[PROMPT_MODE_TRIAL] == 1);
    sim_pad(1, false); /* busy=false → 选 slot1，报"两只老虎" */
    assert(g_played[PROMPT_SONG_TWO_TIGERS] == 1);
}

/* 6) Pad 路由表（与 main 共用 piano_pad_event） */
static void test_pad_routing(void)
{
    reset_all(); /* 主目录 */
    assert(piano_pad_event(0, false) == PIANO_PAD_ENTER_MODE);
    assert(piano_pad_event(3, false) == PIANO_PAD_ENTER_MODE);
    assert(piano_pad_event(4, false) == PIANO_PAD_NO_FUNCTION);
    assert(piano_pad_event(7, false) == PIANO_PAD_NO_FUNCTION);
    assert(piano_pad_event(0, true) == PIANO_PAD_DROPPED);

    piano_enter_mode(PIANO_MODE_AUDITION);
    assert(piano_pad_event(0, false) == PIANO_PAD_PLAY_SLOT);
    assert(piano_pad_event(4, false) == PIANO_PAD_PLAY_SLOT);
    assert(piano_pad_event(5, false) == PIANO_PAD_NO_FUNCTION);

    piano_enter_mode(PIANO_MODE_SELF);
    assert(piano_pad_event(0, false) == PIANO_PAD_PLAY_NOTE);
    assert(piano_pad_event(7, false) == PIANO_PAD_PLAY_NOTE);
}

int main(void)
{
    test_menu_to_trial_exactly_one();
    test_select_song_once_no_replay_on_pause();
    test_self_record_start_saved_once();
    test_no_function_exactly_once();
    test_busy_drop_and_long_press();
    test_pad_routing();

    printf("piano_scene_tests: all passed (6 scenarios; voice-once, no-replay, busy-drop)\n");
    return 0;
}
