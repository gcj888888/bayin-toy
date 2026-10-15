#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "prompt_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 八音琴五态状态机（纯逻辑，可直接在电脑上编译测试，见 host_test/piano_fsm_tests.c）。
 *
 * 心智模型 = 文件夹 / 子文件夹：
 *   - 开机即在主目录（MENU）
 *   - S1~S4 进 4 个子目录：试听 / 自弹 / 游戏 / 回放
 *   - 长按旋钮 ≥5s = 回主目录（任意场景通用，"终极按钮"）
 *   - 旋钮旋转 = 音量（所有场景统一），不在这里实现
 *
 * 本模块只做"模式 + 选择 + 子态"的判定与纯映射，具体发声/点灯/语音由 main 执行。
 */

/* 八音琴五态 */
typedef enum {
    PIANO_MODE_MENU = 0, /* 主目录（开机即在此） */
    PIANO_MODE_AUDITION, /* 试听：S1~S5 选曲 */
    PIANO_MODE_SELF,     /* 自弹：S1~S8 弹音 */
    PIANO_MODE_GAME,     /* 游戏：由网页判定 */
    PIANO_MODE_PLAYBACK, /* 回放：S1~S5 选槽 */
    PIANO_MODE_COUNT
} piano_mode_t;

/* 试听 / 回放的播放子态 */
typedef enum {
    PIANO_PLAY_IDLE = 0, /* 空闲 / 选曲 */
    PIANO_PLAY_PLAYING,  /* 播放中 */
    PIANO_PLAY_PAUSED,   /* 暂停 */
    PIANO_PLAY_COUNT
} piano_play_t;

/* 旋钮短按的上下文动作 */
typedef enum {
    PIANO_SHORT_NONE = 0,
    PIANO_SHORT_MENU_REPROMPT, /* 主目录：重播提示音 */
    PIANO_SHORT_TOGGLE_PAUSE,  /* 试听 / 回放：暂停 / 继续 */
    PIANO_SHORT_TOGGLE_RECORD, /* 自弹：开始记谱 / 再按结束并保存 */
    PIANO_SHORT_INVALID_PROMPT,/* 游戏：无效 → 提示 */
    PIANO_SHORT_COUNT
} piano_short_action_t;

/* ---- 纯映射（宿主测试覆盖） ---- */

/* 主目录 S1~S4 → 试听/自弹/游戏/回放；其它（S5~S8）→ PIANO_MODE_COUNT（无功能） */
piano_mode_t piano_menu_key_mode(int key_index);

/* 试听/回放 S1~S5 → 槽 0~4；其它（S6~S8）→ -1（无功能） */
int piano_slot_key(int key_index);

/* 自弹 S1~S8 → 音号 0~7（哆~高音哆）；越界 → -1 */
int piano_note_key(int key_index);

/*
 * 空闲态应点亮的灯数（灯铁律②："亮几颗 = 数量"）：
 *   MENU → 4（有 4 个入口）；AUDITION / PLAYBACK → min(used, 5)；SELF / GAME → 0
 */
int piano_idle_leds(piano_mode_t mode, int used_count);

/* 是否"只亮当前那颗"（灯铁律①：任何播放/弹奏态）= 试听/自弹/回放 */
bool piano_shows_single_led(piano_mode_t mode);

/* 旋钮短按在给定模式/子态下的上下文动作 */
piano_short_action_t piano_short_press_action(piano_mode_t mode, piano_play_t play,
                                              bool recording);

/* ---- Pad(S1~S8) 按键路由（纯函数；main 与场景宿测共用，避免逻辑分叉）---- */
typedef enum {
    PIANO_PAD_NONE = 0,     /* 无动作 */
    PIANO_PAD_ENTER_MODE,   /* 进子目录：目标 = piano_menu_key_mode(key) */
    PIANO_PAD_PLAY_SLOT,    /* 试听/回放选槽播放：槽 = key */
    PIANO_PAD_PLAY_NOTE,    /* 自弹/游戏弹音：音号 = piano_note_key(key) */
    PIANO_PAD_NO_FUNCTION,  /* 无功能键（上层登记"本键无功能"语音） */
    PIANO_PAD_DROPPED,      /* 语音播报中：本按键被丢弃（无动作、无语音） */
} piano_pad_action_t;

/*
 * 判定某 Pad 按下（上升沿已由上层确认）应做什么。纯函数，不产生副作用。
 * @param voice_busy 语音声道是否仍在播；true 时一律返回 DROPPED（丢弃、不排队）。
 */
piano_pad_action_t piano_pad_event(int key_index, bool voice_busy);

/* ---- 状态 ---- */

void piano_init(void);                 /* 复位 → 主目录、空闲、未记谱 */
piano_mode_t piano_get_mode(void);
void piano_enter_menu(void);           /* 长按旋钮 ≥5s → 回主目录 */
bool piano_enter_mode(piano_mode_t m); /* S1~S4 进子目录；非法模式返回 false */

int piano_get_selected(void);          /* 试听/回放当前选中槽，-1 = 未选 */
void piano_set_selected(int slot);

piano_play_t piano_get_play(void);
void piano_set_play(piano_play_t st);

bool piano_is_recording(void);
void piano_set_recording(bool on);

/* ---- 语音闩锁：一次状态迁移 / 事件只播一句（宿主测试覆盖）----
 * 状态迁移（进子目录 / 回主目录）由本模块自动登记应播句；其余事件（选曲、
 * 记谱、播放结束、空槽、无功能键…）由 main 调 piano_request_voice 登记。
 * main 每次循环调一次 piano_take_voice 取走待播句（取走后即清，绝不重复）。 */
void piano_request_voice(prompt_id_t id); /* 覆盖式登记"待播句" */
int piano_take_voice(void);               /* 取走并清除；无则返回 -1 */

/* 语音播报期间：仅 Pad(S1~S8) 键被丢弃；旋钮（旋转/短按/长按≥5s回主目录）不受影响
 * ——长按回主目录是"保命"手势，不该被语音卡住。 */
bool piano_pad_blocked_by_voice(bool voice_busy);

#ifdef __cplusplus
}
#endif
