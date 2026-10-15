#include "piano_fsm.h"

/* 纯逻辑实现：不依赖任何 ESP-IDF 组件，可直接在电脑上编译测试。 */

static piano_mode_t s_mode = PIANO_MODE_MENU;
static piano_play_t s_play = PIANO_PLAY_IDLE;
static int s_selected = -1;
static bool s_recording = false;

/* 语音闩锁：待播句（一次迁移/事件只播一句） */
static prompt_id_t s_pending_voice = PROMPT_COUNT;
static bool s_has_voice = false;

/* 进入某模式应播的句子 */
static prompt_id_t mode_voice(piano_mode_t m)
{
    switch (m) {
    case PIANO_MODE_AUDITION:
        return PROMPT_MODE_TRIAL;
    case PIANO_MODE_SELF:
        return PROMPT_MODE_SELF;
    case PIANO_MODE_GAME:
        return PROMPT_MODE_GAME;
    case PIANO_MODE_PLAYBACK:
        return PROMPT_MODE_PLAYBACK;
    default:
        return PROMPT_SELECT_MODE;
    }
}

void piano_request_voice(prompt_id_t id)
{
    if ((int)id < 0 || (int)id >= PROMPT_COUNT) {
        return;
    }
    s_pending_voice = id; /* 覆盖式：只保留最后一次登记 */
    s_has_voice = true;
}

int piano_take_voice(void)
{
    if (!s_has_voice) {
        return -1;
    }
    s_has_voice = false; /* 一次性：取走即清，绝不重复播 */
    return (int)s_pending_voice;
}

bool piano_pad_blocked_by_voice(bool voice_busy)
{
    return voice_busy;
}

piano_pad_action_t piano_pad_event(int key_index, bool voice_busy)
{
    if (piano_pad_blocked_by_voice(voice_busy)) {
        return PIANO_PAD_DROPPED; /* 语音播报中：丢弃，不排队 */
    }
    switch (s_mode) {
    case PIANO_MODE_MENU:
        /* S1~S4 进子目录；S5~S8 无功能 */
        if (key_index >= 0 && key_index < 4) {
            return PIANO_PAD_ENTER_MODE;
        }
        return PIANO_PAD_NO_FUNCTION;
    case PIANO_MODE_AUDITION:
    case PIANO_MODE_PLAYBACK:
        /* S1~S5 选曲/选槽；S6~S8 无功能 */
        if (piano_slot_key(key_index) >= 0) {
            return PIANO_PAD_PLAY_SLOT;
        }
        return PIANO_PAD_NO_FUNCTION;
    case PIANO_MODE_SELF:
    case PIANO_MODE_GAME:
        /* S1~S8 = 八音 */
        if (piano_note_key(key_index) >= 0) {
            return PIANO_PAD_PLAY_NOTE;
        }
        return PIANO_PAD_NONE;
    default:
        return PIANO_PAD_NONE;
    }
}

void piano_init(void)
{
    s_mode = PIANO_MODE_MENU;
    s_play = PIANO_PLAY_IDLE;
    s_selected = -1;
    s_recording = false;
    s_has_voice = false;
    piano_request_voice(PROMPT_SELECT_MODE); /* 开机在主目录：请选择模式 */
}

piano_mode_t piano_get_mode(void)
{
    return s_mode;
}

void piano_enter_menu(void)
{
    s_mode = PIANO_MODE_MENU;
    s_play = PIANO_PLAY_IDLE;
    s_selected = -1;
    s_recording = false;
    piano_request_voice(PROMPT_SELECT_MODE);
}

bool piano_enter_mode(piano_mode_t m)
{
    if (m < 0 || m >= PIANO_MODE_COUNT) {
        return false;
    }
    s_mode = m;
    s_play = PIANO_PLAY_IDLE;
    s_selected = -1;
    s_recording = false;
    piano_request_voice(mode_voice(m)); /* 进子目录：播报模式名 */
    return true;
}

int piano_get_selected(void)
{
    return s_selected;
}

void piano_set_selected(int slot)
{
    s_selected = slot;
}

piano_play_t piano_get_play(void)
{
    return s_play;
}

void piano_set_play(piano_play_t st)
{
    if (st < 0 || st >= PIANO_PLAY_COUNT) {
        return;
    }
    s_play = st;
}

bool piano_is_recording(void)
{
    return s_recording;
}

void piano_set_recording(bool on)
{
    s_recording = on;
}

piano_mode_t piano_menu_key_mode(int key_index)
{
    switch (key_index) {
    case 0:
        return PIANO_MODE_AUDITION;
    case 1:
        return PIANO_MODE_SELF;
    case 2:
        return PIANO_MODE_GAME;
    case 3:
        return PIANO_MODE_PLAYBACK;
    default:
        return PIANO_MODE_COUNT; /* S5~S8：本键无功能 */
    }
}

int piano_slot_key(int key_index)
{
    if (key_index < 0 || key_index >= 5) {
        return -1; /* S6~S8：无功能 */
    }
    return key_index;
}

int piano_note_key(int key_index)
{
    if (key_index < 0 || key_index >= 8) {
        return -1;
    }
    return key_index;
}

int piano_idle_leds(piano_mode_t mode, int used_count)
{
    switch (mode) {
    case PIANO_MODE_MENU:
        return 4; /* 有 4 个入口 */
    case PIANO_MODE_AUDITION:
    case PIANO_MODE_PLAYBACK:
        if (used_count < 0) {
            used_count = 0;
        }
        return used_count > 5 ? 5 : used_count; /* 亮几颗 = 已存数量 */
    default:
        return 0; /* 自弹：未记谱不亮音色灯；游戏：由网页呈现 */
    }
}

bool piano_shows_single_led(piano_mode_t mode)
{
    return mode == PIANO_MODE_AUDITION || mode == PIANO_MODE_SELF ||
           mode == PIANO_MODE_PLAYBACK;
}

piano_short_action_t piano_short_press_action(piano_mode_t mode, piano_play_t play,
                                              bool recording)
{
    switch (mode) {
    case PIANO_MODE_MENU:
        return PIANO_SHORT_MENU_REPROMPT;
    case PIANO_MODE_AUDITION:
    case PIANO_MODE_PLAYBACK:
        return PIANO_SHORT_TOGGLE_PAUSE;
    case PIANO_MODE_SELF:
        (void)play;
        return PIANO_SHORT_TOGGLE_RECORD;
    case PIANO_MODE_GAME:
        return PIANO_SHORT_INVALID_PROMPT;
    default:
        (void)play;
        (void)recording;
        return PIANO_SHORT_NONE;
    }
}
