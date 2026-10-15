#include "music_notes.h"

#include <assert.h>
#include <stdio.h>

/*
 * 电子琴纯逻辑的宿主测试。
 * 不依赖任何 ESP-IDF 组件，直接用 gcc + ctest 在电脑上跑。
 * 目的：在烧录之前确认"七个音的频率与颜色"这张表是对的。
 */

/* 自己写浮点近似比较，避免额外链接数学库 */
static int nearly_equal(float a, float b)
{
    float diff = a - b;
    if (diff < 0.0f) {
        diff = -diff;
    }
    return diff < 0.1f;
}

/* 1. 七个音的频率必须精确等于 C 大调音阶 */
static void test_note_frequencies(void)
{
    assert(music_note_frequency_dhz(MUSIC_NOTE_DO) == 2616);  /* 261.6 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_RE) == 2937);  /* 293.7 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_MI) == 3296);  /* 329.6 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_FA) == 3492);  /* 349.2 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_SOL) == 3920); /* 392.0 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_LA) == 4400);  /* 440.0 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_SI) == 4939);  /* 493.9 Hz */
    assert(music_note_frequency_dhz(MUSIC_NOTE_DO_HIGH) == 5232); /* 523.25 Hz（2×do = 523.2） */
}

/* 2. 音高必须随音名递增（do 最低、si 最高） */
static void test_frequencies_ascending(void)
{
    for (int i = 1; i < (int)MUSIC_NOTE_COUNT; ++i) {
        assert(music_note_frequency_dhz((music_note_t)i) >
               music_note_frequency_dhz((music_note_t)(i - 1)));
    }
}

/* 3. 七个音的颜色必须是七种不同颜色（颜色即"音的身份证"，不能撞色） */
static void test_note_colors_distinct(void)
{
    for (int i = 0; i < (int)MUSIC_NOTE_COUNT; ++i) {
        music_color_t a = music_note_color((music_note_t)i);
        for (int j = i + 1; j < (int)MUSIC_NOTE_COUNT; ++j) {
            music_color_t b = music_note_color((music_note_t)j);
            const int same = (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
            assert(!same); /* 任意两个音的颜色都不相同 */
        }
    }
}

/* 4. 关键颜色取值正确（红橙黄绿青蓝紫） */
static void test_note_colors_values(void)
{
    music_color_t c;

    c = music_note_color(MUSIC_NOTE_DO);
    assert(c.r == 150 && c.g == 25 && c.b == 25); /* 红 */

    c = music_note_color(MUSIC_NOTE_RE);
    assert(c.r == 150 && c.g == 85 && c.b == 0); /* 橙 */

    c = music_note_color(MUSIC_NOTE_MI);
    assert(c.r == 140 && c.g == 120 && c.b == 0); /* 黄 */

    c = music_note_color(MUSIC_NOTE_FA);
    assert(c.r == 0 && c.g == 130 && c.b == 40); /* 绿 */

    c = music_note_color(MUSIC_NOTE_SOL);
    assert(c.r == 0 && c.g == 130 && c.b == 130); /* 青 */

    c = music_note_color(MUSIC_NOTE_LA);
    assert(c.r == 30 && c.g == 60 && c.b == 150); /* 蓝 */

    c = music_note_color(MUSIC_NOTE_SI);
    assert(c.r == 120 && c.g == 30 && c.b == 150); /* 紫 */

    c = music_note_color(MUSIC_NOTE_DO_HIGH);
    assert(c.r == 200 && c.g == 40 && c.b == 40); /* 高音哆：亮红（同 do 色系） */
}

/* 5. 八度：升八度翻倍、降八度减半、正常档不变 */
static void test_octave_scaling(void)
{
    /* do 在正常档 = 261.6 Hz */
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_DO, 0), 261.6f));
    /* 升八度 = 523.2 Hz */
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_DO, 1), 523.2f));
    /* 降八度 = 130.8 Hz */
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_DO, -1), 130.8f));

    /* la 在正常档是国际标准音 440 Hz */
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_LA, 0), 440.0f));
}

/* 6. 八度必须被钳位在 [-1, 1]——防止长按旋钮把音高推到听不见 */
static void test_octave_clamp(void)
{
    assert(music_clamp_octave(-1) == -1);
    assert(music_clamp_octave(0) == 0);
    assert(music_clamp_octave(1) == 1);
    assert(music_clamp_octave(-5) == -1);
    assert(music_clamp_octave(5) == 1);

    /* 即使传入越界值，频率也只按钳位后的档位缩放 */
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_DO, 99), 523.2f));
    assert(nearly_equal(music_note_frequency_hz(MUSIC_NOTE_DO, -99), 130.8f));
}

/* 7. 非法音名必须安全返回，不能越界读数组 */
static void test_invalid_note_is_safe(void)
{
    assert(music_note_is_valid(MUSIC_NOTE_DO));
    assert(music_note_is_valid(MUSIC_NOTE_SI));
    assert(music_note_is_valid(MUSIC_NOTE_DO_HIGH));
    assert(!music_note_is_valid((music_note_t)8));
    assert(!music_note_is_valid((music_note_t)99));
    assert(!music_note_is_valid((music_note_t)-1));

    assert(music_note_frequency_dhz((music_note_t)99) == 0);
    assert(nearly_equal(music_note_frequency_hz((music_note_t)99, 0), 0.0f));

    const music_color_t fallback = music_note_color((music_note_t)99);
    assert(fallback.r == 0 && fallback.g == 0 && fallback.b == 0);
}

/* 8. RGB → GRB 转换（硬件 WS2812 的顺序是 GRB，搞反会红绿颠倒） */
static void test_grb_conversion(void)
{
    uint8_t out_g = 0;
    uint8_t out_r = 0;
    uint8_t out_b = 0;

    /* 逻辑 RGB = (r=10, g=20, b=30) */
    const music_color_t sample = {10, 20, 30};
    music_color_to_grb(sample, &out_g, &out_r, &out_b);

    /* GRB 顺序：第一个是绿、第二个是红、第三个是蓝 */
    assert(out_g == 20);
    assert(out_r == 10);
    assert(out_b == 30);

    /* 空指针不能崩 */
    music_color_to_grb(sample, NULL, NULL, NULL);
}

/* 9. 键盘键号 → 音名映射（S1~S7 = do~si，S8 及越界 = 非音符键） */
static void test_key_to_note(void)
{
    assert(music_key_to_note(0) == MUSIC_NOTE_DO);
    assert(music_key_to_note(1) == MUSIC_NOTE_RE);
    assert(music_key_to_note(2) == MUSIC_NOTE_MI);
    assert(music_key_to_note(3) == MUSIC_NOTE_FA);
    assert(music_key_to_note(4) == MUSIC_NOTE_SOL);
    assert(music_key_to_note(5) == MUSIC_NOTE_LA);
    assert(music_key_to_note(6) == MUSIC_NOTE_SI);
    assert(music_key_to_note(7) == MUSIC_NOTE_DO_HIGH);  /* S8 = 高音哆 */
    assert(music_key_to_note(8) == MUSIC_NOTE_COUNT);    /* 越界 */
    assert(music_key_to_note(-1) == MUSIC_NOTE_COUNT);   /* 越界 */
    assert(music_key_to_note(99) == MUSIC_NOTE_COUNT);   /* 越界 */
}

/* 10. 八度循环切换：-1 → 0 → +1 → -1 */
static void test_next_octave(void)
{
    assert(music_next_octave(-1) == 0);
    assert(music_next_octave(0) == 1);
    assert(music_next_octave(1) == -1); /* 高八度再按回到低八度 */
}

/* 11. 音量钳位 */
static void test_clamp_volume(void)
{
    assert(music_clamp_volume(0, 127) == 0);
    assert(music_clamp_volume(100, 127) == 100);
    assert(music_clamp_volume(127, 127) == 127);
    assert(music_clamp_volume(-5, 127) == 0);    /* 下溢 */
    assert(music_clamp_volume(300, 127) == 127); /* 上溢 */
}

int main(void)
{
    test_note_frequencies();
    test_frequencies_ascending();
    test_note_colors_distinct();
    test_note_colors_values();
    test_octave_scaling();
    test_octave_clamp();
    test_invalid_note_is_safe();
    test_grb_conversion();
    test_key_to_note();
    test_next_octave();
    test_clamp_volume();

    printf("music_notes_tests: all passed (notes, colors, octave, GRB, key map, volume)\n");
    return 0;
}
