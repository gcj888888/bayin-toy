#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * 钢琴化音色的客观回归测试（宿主，纯 C，读固件里实际内嵌的音符采样）。
 *
 * 声音"好不好听"是主观的，但"波形是否具备钢琴的客观特征"是可测的。本用例直接
 * 加载 tools/gen_note_samples.py 生成的 note_do.raw（固件里 S1 按键播放的那段 PCM），
 * 断言一组**可测量的**波形/包络性质：
 *   1. 起音起点为 0（余弦起音窗，两端导数为 0 → 无 click）；
 *   2. 全程不削顶（无样本到 ±32767），峰值落在合理区间；
 *   3. 谐波堆栈含偶次谐波（A2/A1 足够大）——不是纯三角波（三角波只有奇次）；
 *   4. 轻微失谐（inharmonicity）：高次分音能量出现在"被拉伸"的频率上，而不是
 *      理想整数倍频率上（模拟钢琴弦刚度）；
 *   5. 双段衰减：基频能量"先快后慢"，早期对数衰减率明显大于后期。
 *
 * 这些性质即使用人耳不可听板子也能客观核验；是否"像钢琴"仍需真机人耳判断。
 */

#ifndef SAMPLE_DIR
#define SAMPLE_DIR "../main/audio/samples"
#endif

#define SR       32000
#define N_FRAMES 27200 /* 0.85s × 32000，与生成器 DURATION 一致 */
#define F0_DO    261.63

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t g_x[N_FRAMES];

static void load_samples(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "piano_timbre_tests: cannot open %s\n", path);
        exit(2);
    }
    const size_t got = fread(g_x, 1, sizeof(g_x), f);
    fclose(f);
    assert(got == sizeof(g_x)); /* 必须是完整的 27200 帧 int16 LE */
}

/* 单频 DFT 幅度（归一化振幅），窗口 start..start+n */
static double dft_amp(int start, int n, double f)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * f * (double)i / SR;
        const double v = (double)g_x[start + i];
        re += v * cos(ph);
        im -= v * sin(ph);
    }
    return sqrt(re * re + im * im) / (double)n;
}

/* 基频在 [t0, t0+50ms] 与之后 50ms 的对数衰减率（每秒） */
static double fund_decay_rate(double t0)
{
    const int w = (int)(0.05 * SR);
    const int s0 = (int)(t0 * SR);
    const int s1 = (int)((t0 + 0.05) * SR);
    const double e0 = dft_amp(s0, w, F0_DO);
    const double e1 = dft_amp(s1, w, F0_DO);
    assert(e0 > 1.0 && e1 > 1.0); /* 两窗都仍有可测能量 */
    return (log(e0) - log(e1)) / 0.05;
}

/* 1. 起音起点为 0 + 起音段平滑（不 click） */
static void test_attack_starts_at_zero(void)
{
    assert(g_x[0] == 0); /* 余弦起音窗在 i=0 处为 0 */
    assert(g_x[1] == 0);
    assert(g_x[2] >= 0); /* 起音段上升 */

    /* 起音前 5ms 内相邻样本差有界（无阶跃/click；上分音造成的高频变化是允许的） */
    int max_step = 0;
    for (int i = 1; i < (int)(0.005 * SR); ++i) {
        const int d = g_x[i] - g_x[i - 1];
        const int ad = d < 0 ? -d : d;
        if (ad > max_step) {
            max_step = ad;
        }
    }
    assert(max_step < 9000);
    printf("  attack: x0=%d max|Δ|(5ms)=%d\n", (int)g_x[0], max_step);
}

/* 2. 不削顶：无样本到 ±32767，峰值落在合理区间，结尾自然收干 */
static void test_no_clipping(void)
{
    int peak = 0;
    for (int i = 0; i < N_FRAMES; ++i) {
        const int a = g_x[i] < 0 ? -g_x[i] : g_x[i];
        assert(a < 32767); /* 归一化到 85% 满幅，不应触顶 */
        if (a > peak) {
            peak = a;
        }
    }
    assert(peak > 20000 && peak < 30000); /* ≈27851 */
    assert(abs((int)g_x[N_FRAMES - 1]) < 200); /* 释放窗收到 0 附近 */
    printf("  peak=%d last=%d\n", peak, (int)g_x[N_FRAMES - 1]);
}

/* 3. 谐波堆栈含偶次谐波（不是纯三角波/纯奇次） */
static void test_even_harmonics_present(void)
{
    const double a1 = dft_amp(0, N_FRAMES, F0_DO * 1.0);
    const double a2 = dft_amp(0, N_FRAMES, F0_DO * 2.0);
    const double a3 = dft_amp(0, N_FRAMES, F0_DO * 3.0);
    assert(a1 > 500.0);
    assert(a2 / a1 > 0.12); /* 有明显二次谐波（纯三角波此处≈0） */
    assert(a3 < a1);        /* 高次谐波弱于基频 */
    printf("  A1=%.0f A2/A1=%.3f A3/A1=%.3f\n", a1, a2 / a1, a3 / a1);
}

/* 4. 轻微失谐：高次分音能量落在"拉伸"频率而非理想整数倍频率 */
static void test_inharmonicity_stretched_partials(void)
{
    const double B = 0.0005; /* 与生成器一致 */
    for (int n = 4; n <= 6; ++n) {
        const double ideal = F0_DO * n;
        const double stretched = F0_DO * n * sqrt(1.0 + B * (double)n * (double)n);
        const double amp_ideal = dft_amp(0, N_FRAMES, ideal);
        const double amp_stretch = dft_amp(0, N_FRAMES, stretched);
        assert(amp_stretch > amp_ideal * 3.0); /* 能量更多在被拉伸的频率上 */
        printf("  partial%d: ideal(%.1fHz)=%.1f stretched(%.1fHz)=%.1f ratio=%.2f\n", n,
               ideal, amp_ideal, stretched, amp_stretch, amp_stretch / amp_ideal);
    }
}

/* 5. 双段衰减：基频"先快后慢"——早期对数衰减率 > 后期 */
static void test_two_stage_decay(void)
{
    const double early = fund_decay_rate(0.01); /* 10→60ms */
    const double late = fund_decay_rate(0.40);  /* 400→450ms */
    assert(early > late * 1.3); /* 明显的前快后慢（单一指数则两者相等） */
    printf("  decay early=%.2f/s late=%.2f/s ratio=%.2f\n", early, late, early / late);
}

int main(void)
{
    load_samples(SAMPLE_DIR "/note_do.raw");

    test_attack_starts_at_zero();
    test_no_clipping();
    test_even_harmonics_present();
    test_inharmonicity_stretched_partials();
    test_two_stage_decay();

    printf("piano_timbre_tests: all passed (attack/no-clip/even-harmonics/inharmonicity/two-stage)\n");
    return 0;
}
