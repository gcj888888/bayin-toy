#include "audio_click.h"

#include "board_pins.h"
#include "board_power.h"
#include "clock.h"
#include "pattern.h"
#include "sequencer.h"
#include "audio_gain.h"
#include "diag_config.h"
#include "voice_src.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "audio_engine";

#define AUDIO_SAMPLE_RATE       32000
#define AUDIO_BLOCK_FRAMES      128
#define AUDIO_STEREO_SAMPLES    (AUDIO_BLOCK_FRAMES * 2)
#define CLICK_FRAMES            288
#define AUDIO_VOICE_COUNT       20 /* 复音数：快速连奏时足够容纳同时发声的音，避免声道耗尽 */
#define AUDIO_REQUEST_QUEUE_LEN 48
#define AUDIO_BEAT_QUEUE_LEN    32

/*
 * 电子琴音色（现场合成，八音盒 / 木琴感）。
 * 波形 = 三角波为主（柔和圆润）+ 少量二次泛音增亮；
 * 包络 = 快速起音（像敲击一下）+ 中速指数衰减（余音自然）。
 * 想微调音色就改这几个数。
 */
#define AUDIO_NOTE_OVERTONE    0.15f   /* 二次泛音占比（0 = 纯三角波），小一点更纯 */
#define AUDIO_NOTE_ATTACK_S    0.005f  /* 起音时长（秒）：余弦窗，清脆且无 click */
#define AUDIO_NOTE_DECAY        4.5f   /* 衰减速度：让音在时长内自然衰减到接近 0 */
#define AUDIO_NOTE_AMPLITUDE 20000.0f  /* 单音峰值幅度 */
#define AUDIO_NOTE_DEFAULT_MS    850u  /* 音长：配合衰减，结尾自然收干（防爆音） */
#define AUDIO_NOTE_MAX_MS       1500u  /* 单音上限 1.5 秒，防止 position 溢出 */
#define AUDIO_NOTE_RELEASE_S    0.12f  /* 释放时长（秒）：余弦平滑窗，两端导数为 0 */

/*
 * 复音混音的软拐点限幅（int16 满幅 32767）：
 *   KNEE 以下完全线性 → 单音不失真、不压缩；
 *   KNEE 以上平滑渐近到 CEILING → 多键叠加时不硬切、不抽水，避免爆音。
 */
#define AUDIO_MIX_KNEE      28500  /* 单音峰值约 27851，留余量使其完全不压缩 */
#define AUDIO_MIX_CEILING   31000  /* 多音叠加时平滑压缩到这里（int16 满幅 32767） */

extern const uint8_t s_kick_start[] asm("_binary_kick_raw_start");
extern const uint8_t s_kick_end[] asm("_binary_kick_raw_end");
extern const uint8_t s_snare_start[] asm("_binary_snare_raw_start");
extern const uint8_t s_snare_end[] asm("_binary_snare_raw_end");
extern const uint8_t s_chh_start[] asm("_binary_hihat_closed_raw_start");
extern const uint8_t s_chh_end[] asm("_binary_hihat_closed_raw_end");
extern const uint8_t s_ohh_start[] asm("_binary_hihat_open_raw_start");
extern const uint8_t s_ohh_end[] asm("_binary_hihat_open_raw_end");
extern const uint8_t s_clap_start[] asm("_binary_clap_raw_start");
extern const uint8_t s_clap_end[] asm("_binary_clap_raw_end");
extern const uint8_t s_rim_start[] asm("_binary_rim_raw_start");
extern const uint8_t s_rim_end[] asm("_binary_rim_raw_end");

/*
 * 电子琴七个音的采样（do~si）。
 * 由 tools/gen_note_samples.py 离线合成：每个音含 6 个泛音、各自独立衰减，
 * 且波形在结尾已完整衰减到 0 —— 播放完自然停止，从根源消除爆音
 * （这正是项目三鼓声采样不炸的原理）。
 */
extern const uint8_t s_note_do_start[] asm("_binary_note_do_raw_start");
extern const uint8_t s_note_do_end[] asm("_binary_note_do_raw_end");
extern const uint8_t s_note_re_start[] asm("_binary_note_re_raw_start");
extern const uint8_t s_note_re_end[] asm("_binary_note_re_raw_end");
extern const uint8_t s_note_mi_start[] asm("_binary_note_mi_raw_start");
extern const uint8_t s_note_mi_end[] asm("_binary_note_mi_raw_end");
extern const uint8_t s_note_fa_start[] asm("_binary_note_fa_raw_start");
extern const uint8_t s_note_fa_end[] asm("_binary_note_fa_raw_end");
extern const uint8_t s_note_sol_start[] asm("_binary_note_sol_raw_start");
extern const uint8_t s_note_sol_end[] asm("_binary_note_sol_raw_end");
extern const uint8_t s_note_la_start[] asm("_binary_note_la_raw_start");
extern const uint8_t s_note_la_end[] asm("_binary_note_la_raw_end");
extern const uint8_t s_note_si_start[] asm("_binary_note_si_raw_start");
extern const uint8_t s_note_si_end[] asm("_binary_note_si_raw_end");

typedef enum {
    VOICE_CLICK_NORMAL = 0,
    VOICE_CLICK_ACCENT,
    VOICE_KICK,
    VOICE_SNARE,
    VOICE_CHH,
    VOICE_OHH,
    VOICE_CLAP,
    VOICE_RIM,
    VOICE_SYNTH_NOTE, /* 电子琴：按频率现场合成的乐音（保留备用） */
    /* 电子琴：离线合成的采样音 do~si（当前使用，波形完整衰减到 0，无爆音） */
    VOICE_NOTE_DO,
    VOICE_NOTE_RE,
    VOICE_NOTE_MI,
    VOICE_NOTE_FA,
    VOICE_NOTE_SOL,
    VOICE_NOTE_LA,
    VOICE_NOTE_SI,
    VOICE_PCM, /* 任意 PCM 缓冲（开机/导入语音），指向外部内存 */
} voice_kind_t;

typedef struct {
    voice_kind_t kind;
    uint8_t velocity;
    int8_t octave;      /* 电子琴采样音：-1/0/+1，决定播放速度（八度） */
    float freq_hz;      /* 仅 VOICE_SYNTH_NOTE：要合成的频率 */
    uint16_t duration;  /* 仅 VOICE_SYNTH_NOTE：总帧数 */
    const int16_t *pcm; /* 仅 VOICE_PCM：外部 PCM 缓冲（int16 单声道） */
    uint32_t pcm_frames; /* 仅 VOICE_PCM：帧数 */
} audio_request_t;

typedef struct {
    bool active;
    voice_kind_t kind;
    uint32_t position;  /* Q8 定点播放位置（实际帧号 = position >> 8） */
    uint16_t step_q8;   /* 每帧步进(Q8)：256=原速，128=慢放(低八度)，512=快放(高八度) */
    uint16_t length;
    uint8_t velocity;
    uint32_t age;
    float freq_hz;     /* 仅 VOICE_SYNTH_NOTE：要合成的频率 */
    uint16_t duration; /* 仅 VOICE_SYNTH_NOTE：总帧数 */
    const int16_t *ext; /* 仅 VOICE_PCM：外部缓冲指针（NULL=用 kind 查表） */
    uint32_t ext_len;   /* 仅 VOICE_PCM：帧数 */
} audio_voice_t;

typedef struct {
    bool running;
    bool restart;
    bool metronome;
    bool drum_mode;
    uint8_t volume;
    uint16_t bpm;
    uint32_t transport_generation;
    uint32_t flush_generation;
    bool stop_pcm; /* 主循环请求：立刻停掉所有 VOICE_PCM（开机/导入语音）声道 */
} audio_control_t;

typedef struct {
    uint32_t bar;
    uint16_t tick;
    uint8_t step;
    uint8_t beat;
} audio_position_t;

static i2s_chan_handle_t s_tx;
static int16_t s_click_normal[CLICK_FRAMES];
static int16_t s_click_accent[CLICK_FRAMES];
static int16_t s_render[AUDIO_STEREO_SAMPLES];
static QueueHandle_t s_request_queue;
static QueueHandle_t s_beat_queue;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_position_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_control_t s_control = {
    .running = false,
    .restart = true,
    .metronome = true,
    .drum_mode = false,
    .volume = 100,
    .bpm = 120,
};
/* Filled only from the audio task via sequencer callback (same-tick, no queue). */
static audio_request_t s_seq_notes[BEATBOX_TRACK_COUNT];
static uint8_t s_seq_note_count;
static audio_position_t s_position;
static bool s_ready;
/* 语音（VOICE_PCM）是否仍在播：由音频任务按声道状态刷新；入队时先置位。
 * 主循环用它实现"语音播报期间丢弃 Pad 按键"。 */
static volatile bool s_pcm_busy;
/* 台架自测：音符采样声道起播回调（见 diag_config.h） */
static audio_note_log_cb_t s_note_log_cb;

static float clampf(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void synthesize_clave(int16_t *destination, int frames, float fundamental_hz, float amplitude)
{
    float noise_lp = 0.0f;
    float previous_noise_lp = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float time = (float)i / (float)AUDIO_SAMPLE_RATE;
        const float body_envelope = expf(-time * 480.0f);
        const float attack_envelope = expf(-time * 2300.0f);
        const float raw_noise =
            ((float)((i * 1103515245u + 12345u) & 0xffff) / 32768.0f - 1.0f);
        noise_lp = 0.58f * noise_lp + 0.42f * raw_noise;
        const float noise_hp = noise_lp - previous_noise_lp;
        previous_noise_lp = noise_lp;

        const float body =
            (sinf(2.0f * (float)M_PI * fundamental_hz * time) +
             0.30f * sinf(2.0f * (float)M_PI * fundamental_hz * 1.52f * time)) *
            body_envelope;
        const float attack = noise_hp * 0.72f * attack_envelope;
        const float attack_ramp = i < 4 ? (float)i / 4.0f : 1.0f;
        const int tail_start = frames - 32;
        const float tail = i < tail_start ? 1.0f : (float)(frames - 1 - i) / 31.0f;
        const float sample =
            clampf((body + attack) * amplitude * attack_ramp * tail, -0.78f, 0.78f);
        destination[i] = (int16_t)(sample * 6200.0f);
    }
}

static uint64_t tick_interval_q32(uint16_t bpm)
{
    /* samples per tick = SAMPLE_RATE * 60 / (bpm * 96) */
    return (((uint64_t)AUDIO_SAMPLE_RATE * 60ULL) << 32) /
           ((uint64_t)beatbox_clamp_bpm(bpm) * (uint64_t)BEATBOX_PPQN_INTERNAL);
}

static audio_control_t control_snapshot(void)
{
    audio_control_t snapshot;
    portENTER_CRITICAL(&s_control_lock);
    snapshot = s_control;
    portEXIT_CRITICAL(&s_control_lock);
    return snapshot;
}

static void publish_position(uint32_t bar, uint16_t tick)
{
    portENTER_CRITICAL(&s_position_lock);
    s_position.bar = bar;
    s_position.tick = tick;
    s_position.step = (uint8_t)((tick / BEATBOX_TICKS_PER_16TH) % BEATBOX_STEPS_PER_BAR);
    s_position.beat = (uint8_t)((tick / BEATBOX_TICKS_PER_QUARTER) % BEATBOX_BEATS_PER_BAR);
    portEXIT_CRITICAL(&s_position_lock);
}

static void voice_source(voice_kind_t kind, const int16_t **source, uint16_t *length)
{
    switch (kind) {
    case VOICE_CLICK_ACCENT:
        *source = s_click_accent;
        *length = CLICK_FRAMES;
        break;
    case VOICE_KICK:
        *source = (const int16_t *)s_kick_start;
        *length = (uint16_t)((s_kick_end - s_kick_start) / sizeof(int16_t));
        break;
    case VOICE_SNARE:
        *source = (const int16_t *)s_snare_start;
        *length = (uint16_t)((s_snare_end - s_snare_start) / sizeof(int16_t));
        break;
    case VOICE_CHH:
        *source = (const int16_t *)s_chh_start;
        *length = (uint16_t)((s_chh_end - s_chh_start) / sizeof(int16_t));
        break;
    case VOICE_OHH:
        *source = (const int16_t *)s_ohh_start;
        *length = (uint16_t)((s_ohh_end - s_ohh_start) / sizeof(int16_t));
        break;
    case VOICE_CLAP:
        *source = (const int16_t *)s_clap_start;
        *length = (uint16_t)((s_clap_end - s_clap_start) / sizeof(int16_t));
        break;
    case VOICE_RIM:
        *source = (const int16_t *)s_rim_start;
        *length = (uint16_t)((s_rim_end - s_rim_start) / sizeof(int16_t));
        break;
    case VOICE_SYNTH_NOTE:
        /* 乐音没有预存采样，由混音循环现场算正弦波 */
        *source = NULL;
        *length = 0;
        break;
    case VOICE_NOTE_DO:
        *source = (const int16_t *)s_note_do_start;
        *length = (uint16_t)((s_note_do_end - s_note_do_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_RE:
        *source = (const int16_t *)s_note_re_start;
        *length = (uint16_t)((s_note_re_end - s_note_re_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_MI:
        *source = (const int16_t *)s_note_mi_start;
        *length = (uint16_t)((s_note_mi_end - s_note_mi_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_FA:
        *source = (const int16_t *)s_note_fa_start;
        *length = (uint16_t)((s_note_fa_end - s_note_fa_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_SOL:
        *source = (const int16_t *)s_note_sol_start;
        *length = (uint16_t)((s_note_sol_end - s_note_sol_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_LA:
        *source = (const int16_t *)s_note_la_start;
        *length = (uint16_t)((s_note_la_end - s_note_la_start) / sizeof(int16_t));
        break;
    case VOICE_NOTE_SI:
        *source = (const int16_t *)s_note_si_start;
        *length = (uint16_t)((s_note_si_end - s_note_si_start) / sizeof(int16_t));
        break;
    case VOICE_CLICK_NORMAL:
    default:
        *source = s_click_normal;
        *length = CLICK_FRAMES;
        break;
    }
}

static voice_kind_t note_to_voice(uint8_t note)
{
    switch (note) {
    case BEATBOX_NOTE_KICK:
        return VOICE_KICK;
    case BEATBOX_NOTE_SNARE:
        return VOICE_SNARE;
    case BEATBOX_NOTE_CHH:
        return VOICE_CHH;
    case BEATBOX_NOTE_OHH:
        return VOICE_OHH;
    case BEATBOX_NOTE_CLAP:
        return VOICE_CLAP;
    case BEATBOX_NOTE_RIM:
        return VOICE_RIM;
    case BEATBOX_NOTE_CLICK_ACCENT:
        return VOICE_CLICK_ACCENT;
    case BEATBOX_NOTE_CLICK_NORMAL:
    default:
        return VOICE_CLICK_NORMAL;
    }
}

/* 诊断用：音符采样 kind → 音号 0~6（do~si）；非音符采样返回 0xFF */
static uint8_t note_index_of_kind(voice_kind_t kind)
{
    switch (kind) {
    case VOICE_NOTE_DO:
        return 0;
    case VOICE_NOTE_RE:
        return 1;
    case VOICE_NOTE_MI:
        return 2;
    case VOICE_NOTE_FA:
        return 3;
    case VOICE_NOTE_SOL:
        return 4;
    case VOICE_NOTE_LA:
        return 5;
    case VOICE_NOTE_SI:
        return 6;
    default:
        return 0xFF;
    }
}

/* 声道播放进度 0~1：越大表示衰减越多、当前幅度越小 */
static float voice_progress(const audio_voice_t *voice)
{
    const float pos = (float)(voice->position >> 8); /* Q8 定点 → 实际帧号 */
    if (voice->kind == VOICE_SYNTH_NOTE) {
        if (voice->duration == 0) {
            return 1.0f;
        }
        return pos / (float)voice->duration;
    }
    if (voice->length == 0) {
        return 1.0f;
    }
    return pos / (float)voice->length;
}

static audio_voice_t *allocate_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], uint32_t age)
{
    /* 优先用空闲声道 */
    for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
        if (!voices[i].active) {
            voices[i].age = age;
            return &voices[i];
        }
    }

    /*
     * 声道占满：挑"播放进度最大"的替换。
     * 进度越大 = 衰减越多 = 当前幅度越小，掐掉它产生的爆音最小。
     * （早先按"年龄最大"替换，会掐掉刚起音、正响亮的音，爆音明显——这是爆音根因。）
     */
    audio_voice_t *victim = &voices[0];
    float best = voice_progress(&voices[0]);
    for (int i = 1; i < AUDIO_VOICE_COUNT; ++i) {
        const float progress = voice_progress(&voices[i]);
        if (progress > best) {
            best = progress;
            victim = &voices[i];
        }
    }
    victim->age = age;
    return victim;
}

/**
 * Relative instrument levels (100 = unity).
 * Closed/open hats are bright and dense on this speaker; keep them well below
 * kick/snare so they glue the groove instead of masking it.
 */
static int voice_level_q7(voice_kind_t kind)
{
    switch (kind) {
    case VOICE_CHH:
        return 34;
    case VOICE_OHH:
        return 40;
    case VOICE_CLAP:
        return 88;
    case VOICE_RIM:
        return 82;
    case VOICE_CLICK_NORMAL:
        return 70;
    case VOICE_CLICK_ACCENT:
        return 76;
    case VOICE_SNARE:
        return 100;
    case VOICE_KICK:
        return 120;
    case VOICE_SYNTH_NOTE:
        return 100; /* 乐音用统一电平，强弱交给 velocity 控制 */
    case VOICE_PCM:
        return 100; /* 语音：统一电平，音量交给总音量 */
    default:
        return 100;
    }
}

static void start_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], voice_kind_t kind,
                        uint8_t velocity, uint32_t age, uint16_t step_q8)
{
    const int16_t *source = NULL;
    uint16_t length = 0;
    voice_source(kind, &source, &length);
    (void)source;

    /*
     * Drum-machine choke rules: retriggering one instrument replaces its
     * previous tail. Closed/open hats also choke each other.
     */
    for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
        if (!voices[i].active) {
            continue;
        }
        const bool same_kind = voices[i].kind == kind;
        const bool hats =
            (kind == VOICE_CHH || kind == VOICE_OHH) &&
            (voices[i].kind == VOICE_CHH || voices[i].kind == VOICE_OHH);
        if (same_kind || hats) {
            voices[i].active = false;
        }
    }

    audio_voice_t *voice = allocate_voice(voices, age);
    voice->active = true;
    voice->kind = kind;
    voice->position = 0;
    voice->step_q8 = step_q8;
    voice->length = length;
    voice->velocity = velocity == 0 ? 100 : velocity;
    /*
     * ★ 根因修复：采样音/鼓声不走外部 PCM 路径，必须清掉声道里可能残留的
     * 旧 VOICE_PCM 缓冲指针。否则复用该声道的音符会被混音循环误当成 PCM 播放，
     * 把上一段语音（如曲名/模式提示）反复重放。
     */
    voice->ext = NULL;
    voice->ext_len = 0;
}

/*
 * 启动一个"现场合成"的乐音。
 * 与鼓声不同，它不读采样数组，而是每帧现场算正弦波（见混音循环）。
 * 同一个音高再次触发时替换掉旧的那个，避免叠加成爆音。
 */
static void start_synth_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], float freq_hz,
                              uint8_t velocity, uint16_t duration_frames, uint32_t age)
{
    for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
        if (voices[i].active && voices[i].kind == VOICE_SYNTH_NOTE &&
            fabsf(voices[i].freq_hz - freq_hz) < 0.01f) {
            voices[i].active = false;
        }
    }

    audio_voice_t *voice = allocate_voice(voices, age);
    voice->active = true;
    voice->kind = VOICE_SYNTH_NOTE;
    voice->position = 0;
    voice->step_q8 = 256; /* 现场合成音不变速 */
    voice->length = 0;
    voice->duration = duration_frames;
    voice->freq_hz = freq_hz;
    voice->velocity = velocity == 0 ? 100 : velocity;
    voice->ext = NULL; /* ★ 合成音同样清掉旧 PCM 残留指针，避免误走 PCM 路径 */
    voice->ext_len = 0;
}

/*
 * 启动一个“外部 PCM 缓冲”音（开机/导入语音）。
 * 不查表，直接指向调用方给的内存，混音循环按帧读取并应用音量/限幅。
 * 缓冲在播放期间必须保持有效（Flash 常驻或 SPIRAM 常驻）。
 */
static void start_pcm_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], const int16_t *data,
                            uint32_t frames, uint32_t age)
{
    if (data == NULL || frames == 0) {
        return;
    }
    audio_voice_t *voice = allocate_voice(voices, age);
    voice->active = true;
    voice->kind = VOICE_PCM;
    voice->position = 0;
    voice->step_q8 = 256; /* 原速播放 */
    voice->length = 0;
    voice->ext = data;
    voice->ext_len = frames;
    voice->velocity = 100;
}

static void clear_voices(audio_voice_t voices[AUDIO_VOICE_COUNT])
{
    memset(voices, 0, sizeof(audio_voice_t) * AUDIO_VOICE_COUNT);
}

static int16_t limit_sample(int32_t sample)
{
    const int32_t sign = sample < 0 ? -1 : 1;
    int32_t magnitude = sample < 0 ? -sample : sample;
    /* 线性区提高到 24000：单音（约 15700）和复音限幅后（约 22000）都不该被这里误伤 */
    if (magnitude <= 24000) {
        return (int16_t)sample;
    }
    const int32_t excess = magnitude - 24000;
    magnitude = 24000 + (excess * 5000) / (excess + 6000);
    return (int16_t)(sign * magnitude);
}

static esp_err_t queue_voice(voice_kind_t kind, uint8_t velocity)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    const audio_request_t request = {.kind = kind, .velocity = velocity};
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

/* 电子琴采样音入队：带八度（决定播放速度） */
static esp_err_t queue_note_voice(voice_kind_t kind, uint8_t velocity, int octave)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    const audio_request_t request = {
        .kind = kind,
        .velocity = velocity,
        .octave = (int8_t)octave,
    };
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t queue_synth_voice(float freq_hz, uint8_t velocity, uint16_t duration_frames)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    const audio_request_t request = {
        .kind = VOICE_SYNTH_NOTE,
        .velocity = velocity,
        .freq_hz = freq_hz,
        .duration = duration_frames,
    };
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t queue_pcm_voice(const int16_t *data, uint32_t frames)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    const audio_request_t request = {
        .kind = VOICE_PCM,
        .velocity = 100,
        .pcm = data,
        .pcm_frames = frames,
    };
    if (xQueueSend(s_request_queue, &request, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    s_pcm_busy = true; /* 入队即视为忙：下一次 Pad 事件立刻被丢弃 */
    static uint32_t s_pcm_enqueued;
    ESP_LOGI(TAG, "PCM enqueue #%u frames=%u", (unsigned)++s_pcm_enqueued, (unsigned)frames);
    return ESP_OK;
}

esp_err_t audio_click_play_pcm(const int16_t *data, uint32_t frames)
{
    return queue_pcm_voice(data, frames);
}

static void sequencer_note_trampoline(uint8_t note, uint8_t velocity)
{
    if (s_seq_note_count >= BEATBOX_TRACK_COUNT) {
        return;
    }
    s_seq_notes[s_seq_note_count].kind = note_to_voice(note);
    s_seq_notes[s_seq_note_count].velocity = velocity == 0 ? 100 : velocity;
    s_seq_note_count++;
}

static void audio_task(void *argument)
{
    (void)argument;

    audio_voice_t voices[AUDIO_VOICE_COUNT] = {0};
    uint64_t sample_cursor = 0;
    uint64_t next_tick_q32 = 0;
    uint64_t interval_q32 = tick_interval_q32(120);
    uint32_t tick_count = 0;
    uint32_t bar = 0;
    uint32_t voice_age = 0;
    uint32_t transport_generation = 0;
    uint32_t flush_generation = 0;
    uint16_t current_bpm = 120;
    bool running = false;
    int32_t gain_q15 = 0; /* 平滑音量增益（Q15）：逐块向目标收敛，消除阶跃杂音 */

    while (true) {
        const audio_control_t control = control_snapshot();

        /* 主循环请求停掉所有语音声道（开机/导入欢迎语让位）：在音频任务里安全执行 */
        if (control.stop_pcm) {
            for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
                if (voices[i].active && voices[i].kind == VOICE_PCM) {
                    voices[i].active = false;
                }
            }
            portENTER_CRITICAL(&s_control_lock);
            s_control.stop_pcm = false;
            portEXIT_CRITICAL(&s_control_lock);
        }

        if (control.flush_generation != flush_generation) {
            flush_generation = control.flush_generation;
            clear_voices(voices);
            xQueueReset(s_request_queue);
            xQueueReset(s_beat_queue);
        }

        if (control.transport_generation != transport_generation) {
            transport_generation = control.transport_generation;
            running = control.running;
            if (running) {
                if (control.restart) {
                    tick_count = 0;
                    bar = 0;
                }
                next_tick_q32 = sample_cursor << 32;
                publish_position(bar, (uint16_t)(tick_count % BEATBOX_TICKS_PER_BAR));
            }
        }

        if (control.bpm != current_bpm) {
            const uint64_t old_interval = interval_q32;
            const uint64_t new_interval = tick_interval_q32(control.bpm);
            if (running) {
                const uint64_t now_q32 = sample_cursor << 32;
                if (next_tick_q32 > now_q32) {
                    const long double phase_remaining =
                        (long double)(next_tick_q32 - now_q32) / (long double)old_interval;
                    next_tick_q32 =
                        now_q32 + (uint64_t)(phase_remaining * (long double)new_interval);
                }
            }
            current_bpm = control.bpm;
            interval_q32 = new_interval;
        }

        /* 音量增益单极点平滑（时间常数 ~10ms）：逐块向目标收敛，消除 zipper 杂音 */
        gain_q15 = audio_gain_step_q15(gain_q15,
                                       audio_gain_target_q15(control.volume, BEATBOX_VOLUME_MAX),
                                       AUDIO_GAIN_SMOOTH_K_Q15);

        audio_request_t request;
        while (xQueueReceive(s_request_queue, &request, 0) == pdTRUE) {
            /*
             * 开机/导入语音走 VOICE_PCM 通道。一旦用户开始弹奏（任何非 PCM 的
             * 音符或鼓点入队），就让正在播放的语音声道让位——既避免"开机音赖着
             * 不走、还和琴音搅在一起"，也保证提示音只在开机时出现。
             * 语音本身(VOICE_PCM)入队时不受此影响，可正常播完或被后续弹奏打断。
             */
            if (request.kind != VOICE_PCM) {
                for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
                    if (voices[i].active && voices[i].kind == VOICE_PCM) {
                        voices[i].active = false;
                    }
                }
            }
            if (request.kind == VOICE_SYNTH_NOTE) {
                start_synth_voice(voices, request.freq_hz, request.velocity, request.duration,
                                  voice_age++);
            } else {
                /*
                 * 电子琴采样音：按八度变速播放（一组采样覆盖三个八度）。
                 * 低八度 = 慢放 0.5x(128)，高八度 = 快放 2x(512)。
                 * 副作用刚好符合真实乐器：低音余音长、高音余音短。
                 */
                if (request.kind == VOICE_PCM) {
                    start_pcm_voice(voices, request.pcm, request.pcm_frames, voice_age++);
                } else {
                    uint16_t step_q8 = 256;
                    if (request.octave > 0) {
                        step_q8 = 512;
                    } else if (request.octave < 0) {
                        step_q8 = 128;
                    }
                    start_voice(voices, request.kind, request.velocity, voice_age++, step_q8);
#if BAYIN_ENABLE_DIAG
                    /* 台架自测：真正起了「音符采样」声道 → 触发 snd 事件（回调不做阻塞打印） */
                    const uint8_t ni = note_index_of_kind(request.kind);
                    if (ni != 0xFF && s_note_log_cb != NULL) {
                        s_note_log_cb(ni);
                    }
#endif
                }
            }
        }

        for (uint32_t frame = 0; frame < AUDIO_BLOCK_FRAMES; ++frame) {
            const uint64_t frame_q32 = (sample_cursor + frame) << 32;

            if (running && frame_q32 >= next_tick_q32) {
                const uint16_t tick_in_bar =
                    (uint16_t)(tick_count % BEATBOX_TICKS_PER_BAR);
                const uint8_t step =
                    (uint8_t)((tick_in_bar / BEATBOX_TICKS_PER_16TH) % BEATBOX_STEPS_PER_BAR);
                const uint8_t beat_in_bar =
                    (uint8_t)((tick_in_bar / BEATBOX_TICKS_PER_QUARTER) % BEATBOX_BEATS_PER_BAR);
                const bool on_step = (tick_in_bar % BEATBOX_TICKS_PER_16TH) == 0;
                const bool on_quarter = (tick_in_bar % BEATBOX_TICKS_PER_QUARTER) == 0;
                const bool accent = on_quarter && beat_in_bar == 0;

                /* Independent layers: drum sequencer and metronome click can both run. */
                if (control.drum_mode) {
                    s_seq_note_count = 0;
                    sequencer_on_tick(tick_in_bar);
                    for (uint8_t i = 0; i < s_seq_note_count; ++i) {
                        start_voice(voices, s_seq_notes[i].kind, s_seq_notes[i].velocity,
                                    voice_age++, 256);
                    }
                }

                if (on_quarter && control.metronome) {
                    start_voice(voices, accent ? VOICE_CLICK_ACCENT : VOICE_CLICK_NORMAL, 90,
                                voice_age++, 256);
                }

                if (on_step || on_quarter) {
                    const audio_beat_event_t event = {
                        .step = step,
                        .beat_in_bar = beat_in_bar,
                        .tick = tick_in_bar,
                        .bar = bar,
                        .accent = accent,
                        .is_quarter = on_quarter,
                        .is_step = on_step,
                        .sample_index = sample_cursor + frame,
                    };
                    (void)xQueueSend(s_beat_queue, &event, 0);
                }

                publish_position(bar, tick_in_bar);

                tick_count++;
                if ((tick_count % BEATBOX_TICKS_PER_BAR) == 0) {
                    bar++;
                }
                next_tick_q32 += interval_q32;
            }

            int32_t mixed = 0;
            for (int voice_index = 0; voice_index < AUDIO_VOICE_COUNT; ++voice_index) {
                audio_voice_t *voice = &voices[voice_index];
                if (!voice->active) {
                    continue;
                }
                int32_t sample;
                if (voice->kind == VOICE_SYNTH_NOTE) {
                    /*
                     * 电子琴乐音：现场合成（三角波为主 + 自然包络）。
                     * 三角波比正弦波多了奇数谐波（3倍、5倍频…幅度 1/n² 递减），
                     * 听起来圆润柔和，接近八音盒/木琴，而不是玩具的"哔"声。
                     */
                    const float t = (float)(voice->position >> 8) / (float)AUDIO_SAMPLE_RATE;
                    const float freq = voice->freq_hz;

                    /* 三角波：相位归一到 0~1，输出 -1~1 */
                    float phase = freq * t;
                    phase -= floorf(phase);
                    float tri;
                    if (phase < 0.25f) {
                        tri = 4.0f * phase;
                    } else if (phase < 0.75f) {
                        tri = 2.0f - 4.0f * phase;
                    } else {
                        tri = 4.0f * phase - 4.0f;
                    }

                    /* 少量二次泛音（正弦），补一点亮度，不至于太闷 */
                    const float omega = 2.0f * (float)M_PI * freq * t;
                    const float overtone = sinf(2.0f * omega);

                    const float wave =
                        tri * (1.0f - AUDIO_NOTE_OVERTONE) + overtone * AUDIO_NOTE_OVERTONE;

                    /*
                     * 包络：起音 → 指数衰减 → 释放（三者都平滑）。
                     * 起音用余弦窗：指数起音在起始点导数也是突变的，同样会有 click；
                     * 余弦窗两端导数为 0，且 5 毫秒足够短，能保持敲击的"脆"感。
                     */
                    float attack;
                    if (t < AUDIO_NOTE_ATTACK_S) {
                        attack = 0.5f * (1.0f - cosf((float)M_PI * t / AUDIO_NOTE_ATTACK_S));
                    } else {
                        attack = 1.0f;
                    }
                    float envelope = attack * expf(-t * AUDIO_NOTE_DECAY);

                    /*
                     * 释放期：用余弦平滑窗降到 0。
                     * 线性斜坡在收尾点斜率不为零（导数突变），仍会有轻微 click；
                     * 余弦窗两端导数都为 0，收尾在数学上完全平滑。
                     * 这是向项目三的鼓声采样学的：真实采样结尾本就衰减到 0，
                     * 所以播放完直接停也不会有爆音。
                     */
                    const float remaining =
                        (float)(voice->duration - (voice->position >> 8)) /
                        (float)AUDIO_SAMPLE_RATE;
                    if (remaining < AUDIO_NOTE_RELEASE_S) {
                        const float x = remaining / AUDIO_NOTE_RELEASE_S; /* 1 → 0 */
                        envelope *= 0.5f * (1.0f - cosf((float)M_PI * x));
                    }

                    sample = (int32_t)(wave * envelope * AUDIO_NOTE_AMPLITUDE);

                    voice->position += voice->step_q8;
                    if ((voice->position >> 8) >= voice->duration) {
                        voice->active = false;
                    }
                } else {
                    const int16_t *source = NULL;
                    uint32_t length = 0;
                    /*
                     * ★ 只用 kind 判定是否 PCM（voice_src_uses_external）：
                     * 绝不能用 "ext != NULL"，否则复用过 PCM 声道的音符会残留
                     * ext 指针、被误当成 PCM 播放（真机"语音反复播报"根因）。
                     */
                    if (voice_src_uses_external(voice->kind == VOICE_PCM, voice->ext != NULL)) {
                        /* 外部 PCM 缓冲（语音提示/开机语音） */
                        source = voice->ext;
                        length = voice->ext_len;
                    } else {
                        uint16_t l = 0;
                        voice_source(voice->kind, &source, &l);
                        length = l;
                    }
                    /* Q8 定点播放：实际帧号 = position >> 8，步进由 step_q8 控制（八度变速） */
                    const uint32_t frame_index = voice->position >> 8;
                    if (frame_index >= length) {
                        voice->active = false;
                        sample = 0;
                    } else {
                        sample = source[frame_index];
                        voice->position += voice->step_q8;
                        if ((voice->position >> 8) >= length) {
                            voice->active = false;
                        }
                    }
                }

                sample = (sample * (int32_t)voice->velocity) / 127;
                sample = (sample * voice_level_q7(voice->kind)) / 100;
                mixed += sample;
            }

            /*
             * 软拐点限幅：多键同时按下时总幅度可能超过安全值。
             * 阈值以下完全线性（单音不受影响），超过后平滑压缩到上限——
             * 不硬切，也不逐采样等比缩放（那样会拧变形，产生"抽水"失真和爆音）。
             */
            {
                const int32_t abs_mixed = mixed < 0 ? -mixed : mixed;
                if (abs_mixed > AUDIO_MIX_KNEE) {
                    const float excess = (float)(abs_mixed - AUDIO_MIX_KNEE) /
                                         (float)(AUDIO_MIX_CEILING - AUDIO_MIX_KNEE);
                    const float compressed =
                        (float)AUDIO_MIX_KNEE +
                        (float)(AUDIO_MIX_CEILING - AUDIO_MIX_KNEE) * (1.0f - expf(-excess));
                    mixed = mixed < 0 ? -(int32_t)compressed : (int32_t)compressed;
                }
            }

            /* 音量增益：用平滑后的 gain_q15（不再逐样本取 control.volume 阶跃） */
            mixed = (int32_t)(((int64_t)mixed * gain_q15) >> 15);
            const int16_t output = limit_sample(mixed);
            s_render[frame * 2] = output;
            s_render[frame * 2 + 1] = output;
        }

        size_t bytes_written = 0;
        const esp_err_t error =
            i2s_channel_write(s_tx, s_render, sizeof(s_render), &bytes_written, portMAX_DELAY);
        if (error != ESP_OK || bytes_written != sizeof(s_render)) {
            ESP_LOGE(TAG, "I2S render failed: %s (%u/%u)", esp_err_to_name(error),
                     (unsigned)bytes_written, (unsigned)sizeof(s_render));
        }
        /*
         * 刷新"语音是否仍在播"：本块渲染后若已无 VOICE_PCM 声道在放，则清忙标志。
         * 供主循环判断"语音播报期间是否丢弃 Pad 按键"。
         */
        {
            bool busy = false;
            for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
                if (voices[i].active && voices[i].kind == VOICE_PCM) {
                    busy = true;
                    break;
                }
            }
            s_pcm_busy = busy;
        }

        sample_cursor += AUDIO_BLOCK_FRAMES;
    }
}

esp_err_t audio_click_init(void)
{
    ESP_RETURN_ON_FALSE(board_power_peripherals_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "enable GPIO8 before speaker");

    synthesize_clave(s_click_normal, CLICK_FRAMES, 1850.0f, 0.62f);
    synthesize_clave(s_click_accent, CLICK_FRAMES, 2450.0f, 0.68f);

    ESP_RETURN_ON_ERROR(sequencer_init(sequencer_note_trampoline), TAG, "sequencer");

    s_request_queue = xQueueCreate(AUDIO_REQUEST_QUEUE_LEN, sizeof(audio_request_t));
    s_beat_queue = xQueueCreate(AUDIO_BEAT_QUEUE_LEN, sizeof(audio_beat_event_t));
    ESP_RETURN_ON_FALSE(s_request_queue != NULL && s_beat_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "audio queues");

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = AUDIO_BLOCK_FRAMES;
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx, NULL), TAG, "new I2S channel");

    /*
     * I2S 时钟源：显式选 ESP32-S3 的 PLL_F160M（160MHz）。
     *
     * 重要事实（之前踩坑）：ESP32-S3 的 I2S **没有** APLL 时钟源
     * （APLL 只存在于 ESP32 / ESP32-S2 / ESP32-P4），所以 `I2S_CLK_SRC_APLL`
     * 在 S3 上直接编译不过——而且也不需要它。
     *
     * 为什么 PLL_F160M 就够准：S3 的 MCLK 分频器支持小数分频
     * （分母最大 512）。160MHz 要得到 32kHz 采样率，需要 MCLK = 8.192MHz，
     * 即 160 / 8.192 = 19.53125 = 19 + 17/32，正好是 分母32 的分数，
     * 小数分频能精确凑出来 → 输出严格 32.000kHz，乐音音高完全准确
     * （仅剩晶振本身 ±20ppm 误差，人耳听不出）。
     * 鼓声是打击乐、无明确音高，所以过去从没暴露时钟精度问题。
     */
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;

    const i2s_std_config_t standard_config = {
        .clk_cfg = clk_cfg,
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = BOARD_GPIO_SPK_BCLK,
                .ws = BOARD_GPIO_SPK_WS,
                .dout = BOARD_GPIO_SPK_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard_config), TAG,
                        "init I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable I2S");

    const BaseType_t created =
        xTaskCreatePinnedToCore(audio_task, "audio_render", 6144, NULL, 8, NULL, 0);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "audio render task");
    s_ready = true;

    ESP_LOGI(TAG, "sample-clock engine ready: %d Hz, 96 PPQN, %d voices", AUDIO_SAMPLE_RATE,
             AUDIO_VOICE_COUNT);
    return ESP_OK;
}

esp_err_t audio_click_set_bpm(uint16_t bpm)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.bpm = beatbox_clamp_bpm(bpm);
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_metronome(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.metronome = enabled;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_mode(audio_mode_t mode)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.drum_mode = mode == AUDIO_MODE_DRUM;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_volume(uint8_t volume)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    if (volume > BEATBOX_VOLUME_MAX) {
        volume = BEATBOX_VOLUME_MAX;
    }
    portENTER_CRITICAL(&s_control_lock);
    s_control.volume = volume;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

uint8_t audio_click_get_volume(void)
{
    portENTER_CRITICAL(&s_control_lock);
    const uint8_t volume = s_control.volume;
    portEXIT_CRITICAL(&s_control_lock);
    return volume;
}

esp_err_t audio_click_set_running(bool running, bool restart)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.running = running;
    s_control.restart = restart;
    s_control.transport_generation++;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

bool audio_click_poll_beat(audio_beat_event_t *event)
{
    if (!s_ready || event == NULL) {
        return false;
    }
    return xQueueReceive(s_beat_queue, event, 0) == pdTRUE;
}

esp_err_t audio_click_play_normal(void)
{
    return queue_voice(VOICE_CLICK_NORMAL, 110);
}

esp_err_t audio_click_play_accent(void)
{
    return queue_voice(VOICE_CLICK_ACCENT, 120);
}

esp_err_t audio_click_play_note(uint8_t note, uint8_t velocity)
{
    return queue_voice(note_to_voice(note), velocity);
}

esp_err_t audio_click_play_pitch(float freq_hz, uint8_t velocity, uint32_t duration_ms)
{
    /* 频率必须落在人耳可闻范围内 */
    if (freq_hz < 20.0f || freq_hz > 20000.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ms = duration_ms == 0 ? AUDIO_NOTE_DEFAULT_MS : duration_ms;
    if (ms > AUDIO_NOTE_MAX_MS) {
        ms = AUDIO_NOTE_MAX_MS;
    }

    /* 毫秒 → 采样帧数；上限 1.5 秒 = 48000 帧，uint16 装得下 */
    const uint32_t frames = (ms * (uint32_t)AUDIO_SAMPLE_RATE) / 1000u;
    return queue_synth_voice(freq_hz, velocity, (uint16_t)frames);
}

esp_err_t audio_click_play_note_sample(uint8_t note_index, uint8_t velocity, int octave)
{
    static const voice_kind_t kNoteVoices[7] = {
        VOICE_NOTE_DO, VOICE_NOTE_RE, VOICE_NOTE_MI, VOICE_NOTE_FA,
        VOICE_NOTE_SOL, VOICE_NOTE_LA, VOICE_NOTE_SI,
    };
    if (note_index >= 7) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 与鼓声同一条播放路径：采样播完自然停止，不爆音 */
    return queue_note_voice(kNoteVoices[note_index], velocity, octave);
}

void audio_click_get_position(uint32_t *bar, uint8_t *step, uint8_t *beat, uint16_t *tick)
{
    portENTER_CRITICAL(&s_position_lock);
    if (bar) {
        *bar = s_position.bar;
    }
    if (step) {
        *step = s_position.step;
    }
    if (beat) {
        *beat = s_position.beat;
    }
    if (tick) {
        *tick = s_position.tick;
    }
    portEXIT_CRITICAL(&s_position_lock);
}

esp_err_t audio_click_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.running = false;
    s_control.restart = false;
    s_control.transport_generation++;
    s_control.flush_generation++;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_stop_pcm(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.stop_pcm = true;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

bool audio_click_pcm_busy(void)
{
    return s_pcm_busy;
}

void audio_click_set_note_log_cb(audio_note_log_cb_t cb)
{
    s_note_log_cb = cb;
}
