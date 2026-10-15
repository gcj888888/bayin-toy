#include "music_notes.h"

#include <stddef.h>

/*
 * 纯逻辑实现：不依赖任何 ESP-IDF 组件，可直接在电脑上编译测试。
 */

/* 频率表，单位 0.1 Hz（索引即 music_note_t） */
static const uint16_t kNoteFrequencyDhz[MUSIC_NOTE_COUNT] = {
    2616, /* do      261.6 Hz */
    2937, /* re      293.7 Hz */
    3296, /* mi      329.6 Hz */
    3492, /* fa      349.2 Hz */
    3920, /* sol     392.0 Hz */
    4400, /* la      440.0 Hz */
    4939, /* si      493.9 Hz */
    5232  /* 高音哆   523.25 Hz（表精度 0.1Hz，取 2×do = 523.2） */
};

/* 颜色表（逻辑 RGB，索引即 music_note_t）。
 * 柔和版彩虹 7 色：色相沿色环均匀分布（红橙黄绿青蓝紫，每色约隔 51°），
 * 亮度压到约 50%，不刺眼。
 * 第 8 音（高音哆）补一个与 do 同色系（红）但更亮的颜色，与 do 可区分。 */
static const music_color_t kNoteColors[MUSIC_NOTE_COUNT] = {
    {150, 25, 25},  /* do      红 */
    {150, 85, 0},   /* re      橙 */
    {140, 120, 0},  /* mi      黄 */
    {0, 130, 40},   /* fa      绿 */
    {0, 130, 130},  /* sol     青 */
    {30, 60, 150},  /* la      蓝 */
    {120, 30, 150}, /* si      紫 */
    {200, 40, 40}   /* 高音哆   亮红（同 do 色系，可区分） */
};

int music_clamp_octave(int octave)
{
    if (octave < MUSIC_OCTAVE_MIN) {
        return MUSIC_OCTAVE_MIN;
    }
    if (octave > MUSIC_OCTAVE_MAX) {
        return MUSIC_OCTAVE_MAX;
    }
    return octave;
}

bool music_note_is_valid(music_note_t note)
{
    return (int)note >= 0 && (int)note < (int)MUSIC_NOTE_COUNT;
}

uint16_t music_note_frequency_dhz(music_note_t note)
{
    if (!music_note_is_valid(note)) {
        return 0;
    }
    return kNoteFrequencyDhz[(int)note];
}

float music_note_frequency_hz(music_note_t note, int octave)
{
    if (!music_note_is_valid(note)) {
        return 0.0f;
    }

    const int clamped = music_clamp_octave(octave);
    float hz = (float)kNoteFrequencyDhz[(int)note] / 10.0f;

    /* 升八度翻倍、降八度减半 */
    if (clamped > 0) {
        for (int i = 0; i < clamped; ++i) {
            hz *= 2.0f;
        }
    } else if (clamped < 0) {
        for (int i = 0; i < -clamped; ++i) {
            hz *= 0.5f;
        }
    }
    return hz;
}

music_color_t music_note_color(music_note_t note)
{
    const music_color_t black = {0, 0, 0};
    if (!music_note_is_valid(note)) {
        return black;
    }
    return kNoteColors[(int)note];
}

void music_color_to_grb(music_color_t color, uint8_t *out_g, uint8_t *out_r, uint8_t *out_b)
{
    /* 硬件 WS2812 的顺序是 GRB，这里把逻辑 RGB 拆成 GRB 三路输出 */
    if (out_g != NULL) {
        *out_g = color.g;
    }
    if (out_r != NULL) {
        *out_r = color.r;
    }
    if (out_b != NULL) {
        *out_b = color.b;
    }
}

music_note_t music_key_to_note(int key_index)
{
    if (key_index < 0 || key_index >= (int)MUSIC_NOTE_COUNT) {
        return MUSIC_NOTE_COUNT; /* 越界：不是音符键 */
    }
    return (music_note_t)key_index;
}

int music_next_octave(int octave)
{
    /* -1 → 0 → +1 → -1 循环 */
    if (octave >= MUSIC_OCTAVE_MAX) {
        return MUSIC_OCTAVE_MIN;
    }
    return octave + 1;
}

uint8_t music_clamp_volume(int value, uint8_t max)
{
    if (value < 0) {
        return 0;
    }
    if (value > (int)max) {
        return max;
    }
    return (uint8_t)value;
}
