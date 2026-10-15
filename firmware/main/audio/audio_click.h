#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t step;         /* 0..15 grid step */
    uint8_t beat_in_bar;  /* 0..3 */
    uint16_t tick;        /* 0..383 within bar */
    uint32_t bar;
    bool accent;
    bool is_quarter;      /* quarter-note boundary */
    bool is_step;         /* 16th grid boundary (playhead) */
    uint64_t sample_index;
} audio_beat_event_t;

typedef enum {
    AUDIO_MODE_METRONOME = 0,
    AUDIO_MODE_DRUM = 1,
} audio_mode_t;

/**
 * Continuous, sample-clocked I2S renderer.
 *
 * Internal transport runs at 96 PPQN derived from the I2S sample cursor.
 * Metronome edges, sequencer ticks and external pad notes share the same
 * voice mixer so sounds never hard-cut each other.
 */
esp_err_t audio_click_init(void);
esp_err_t audio_click_set_bpm(uint16_t bpm);
esp_err_t audio_click_set_metronome(bool enabled);
esp_err_t audio_click_set_mode(audio_mode_t mode);
esp_err_t audio_click_set_volume(uint8_t volume);
uint8_t audio_click_get_volume(void);

/**
 * @param running transport state
 * @param restart true = Start (reset position); false = Continue
 */
esp_err_t audio_click_set_running(bool running, bool restart);

/** Non-blocking position / beat event for LED/host visualization. */
bool audio_click_poll_beat(audio_beat_event_t *event);

esp_err_t audio_click_play_normal(void);
esp_err_t audio_click_play_accent(void);
esp_err_t audio_click_play_note(uint8_t note, uint8_t velocity);

/**
 * 电子琴：现场合成一个指定音高的音（八音盒 / 木琴音色）。
 *
 * 与鼓声（播放预存采样）不同，这个音是**实时算出来的**：
 * 采样值 = (基础正弦 + 泛音 × 高倍频正弦) × 指数衰减。
 * 因此不占用任何存储，音长可以任意指定。
 *
 * @param freq_hz      音高频率（Hz），例如 do = 261.6
 * @param velocity     力度 0-127（0 视为 100）
 * @param duration_ms  音长（毫秒），建议 300-800；超出上限会被钳位
 */
esp_err_t audio_click_play_pitch(float freq_hz, uint8_t velocity, uint32_t duration_ms);

/**
 * 电子琴：播放离线合成的采样音（do~si）。
 *
 * 采样波形在结尾处已完整衰减到 0，播放完自然停止，不存在"掐断"，
 * 因此不会产生爆音 —— 这是与项目三鼓声采样同源的原理。
 *
 * @param note_index 0~6，对应 do~si（即 music_note_t 的取值）
 * @param velocity   力度 0-127（0 视为 100）
 * @param octave     八度 -1/0/+1，用变速播放实现（一组采样覆盖三个八度）
 */
esp_err_t audio_click_play_note_sample(uint8_t note_index, uint8_t velocity, int octave);

/**
 * 播放一段已有的 PCM 缓冲（开机语音 / 导入语音）。
 * 复用混音通道，音量、限幅自动生效。缓冲必须在播放期间保持有效
 * （内嵌默认在 Flash 常驻；导入语音在 SPIRAM 常驻）。
 *
 * @param data    int16 单声道 PCM（与 I2S 同采样率 32kHz）
 * @param frames  采样帧数（不是字节数）
 */
esp_err_t audio_click_play_pcm(const int16_t *data, uint32_t frames);

/** Current transport position (safe to read from main task). */
void audio_click_get_position(uint32_t *bar, uint8_t *step, uint8_t *beat, uint16_t *tick);

/** Stop transport, flush queued events, and silence all active voices. */
esp_err_t audio_click_stop(void);

/**
 * 立刻停掉所有正在播放的“外部语音”声道（开机/导入语音，VOICE_PCM）。
 * 用于“用户一开始弹琴/回放，开机欢迎语就让位”的场景：从主循环直接调用，
 * 通过 control 快照安全传递到音频任务（避免跨任务直接改 voices[]）。
 * 与队列里的“非 PCM 请求让位”是双重保险。
 */
esp_err_t audio_click_stop_pcm(void);

/**
 * 语音声道（VOICE_PCM）当前是否仍在播放。
 * 由音频任务按块刷新（入队即置忙，播完自动清）。
 * 八音琴主循环用它实现"语音播报期间丢弃 Pad 按键"。
 */
bool audio_click_pcm_busy(void);

/**
 * ★ 台架自测（BAYIN_ENABLE_DIAG）：音频层每**真正起一个「音符采样」声道**时回调一次。
 * note = 0~6（do~si）。仅诊断用；回调内**不要**做阻塞/串口打印（避免音频任务抖动）——
 * 本工程由 main 的 ring 收集后于主循环落 JSON 日志。
 */
typedef void (*audio_note_log_cb_t)(uint8_t note);
void audio_click_set_note_log_cb(audio_note_log_cb_t cb);

#ifdef __cplusplus
}
#endif
