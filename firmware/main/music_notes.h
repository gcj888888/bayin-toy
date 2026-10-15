#pragma once

/*
 * 八音琴：八个音的频率与颜色对照表（do re mi fa sol la si 高音哆）。
 *
 * 这里是纯逻辑层——只依赖 stdint/stdbool，不碰任何 ESP-IDF 头文件，
 * 因此可以直接在电脑上编译并测试（见 host_test/music_notes_tests.c）。
 *
 * 设计约定（见 easyinput-piano/docs/八音琴-需求与决策记录.md）：
 * - 前七个音固定对应红橙黄绿青蓝紫，颜色就是"音的身份证"；第 8 音（高音哆）
 *   取与 do 同色系的亮红，保持可区分
 * - 弹奏/试听/回放共用同一套颜色，方便形成肌肉记忆
 * - 硬件 WS2812 的颜色顺序是 GRB，逻辑层统一用 RGB，由驱动层转换
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 八个音，顺序即 do re mi fa sol la si 高音哆（八音琴） */
typedef enum {
    MUSIC_NOTE_DO = 0,
    MUSIC_NOTE_RE = 1,
    MUSIC_NOTE_MI = 2,
    MUSIC_NOTE_FA = 3,
    MUSIC_NOTE_SOL = 4,
    MUSIC_NOTE_LA = 5,
    MUSIC_NOTE_SI = 6,
    MUSIC_NOTE_DO_HIGH = 7, /* 高音哆（S8） */
    MUSIC_NOTE_COUNT = 8
} music_note_t;

/* 逻辑颜色（RGB 顺序） */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} music_color_t;

/* 八度档位范围：-1 低八度 / 0 正常 / +1 高八度 */
#define MUSIC_OCTAVE_MIN (-1)
#define MUSIC_OCTAVE_MAX (1)

/* 八度档位钳位（超出范围就拉回边界） */
int music_clamp_octave(int octave);

/* 音名是否合法 */
bool music_note_is_valid(music_note_t note);

/*
 * 音的频率，单位 0.1 Hz 的整数（do = 2616 表示 261.6 Hz）。
 * 用整数是为了让电脑上测试可以精确比较，不受浮点误差影响。
 */
uint16_t music_note_frequency_dhz(music_note_t note);

/*
 * 音的实际频率（Hz），已按八度缩放。
 * 八度 clamp 到 [-1, 1]：升八度频率翻倍，降八度减半。
 */
float music_note_frequency_hz(music_note_t note, int octave);

/* 音对应的颜色 */
music_color_t music_note_color(music_note_t note);

/* 逻辑 RGB 转硬件 GRB（WS2812 的顺序是 GRB，不是 RGB） */
void music_color_to_grb(music_color_t color, uint8_t *out_g, uint8_t *out_r, uint8_t *out_b);

/*
 * 键盘物理键号 → 音名。
 * 键 0~7（S1~S8）对应 do~si 高音哆（八音琴 8 音）；越界值返回 MUSIC_NOTE_COUNT
 * 表示"非音符键"。这是纯查表逻辑，宿主测试覆盖它，确保按键映射不出错。
 */
music_note_t music_key_to_note(int key_index);

/* 八度循环切换：-1 → 0 → +1 → -1 ...（保留给旧接口；八音琴固定 0 档） */
int music_next_octave(int octave);

/* 音量钳位：把任意整数夹到 [0, max] 区间，max 通常是 127 */
uint8_t music_clamp_volume(int value, uint8_t max);

#ifdef __cplusplus
}
#endif
