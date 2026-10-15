#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "song_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host link over USB-Serial/JTAG.
 * Domain events framed as newline JSON (protocol v2). See docs/host-protocol.md.
 */

esp_err_t host_link_init(void);
bool host_link_ready(void);

void host_link_send_hello(void);
void host_link_send_start(void);
void host_link_send_continue(void);
void host_link_send_stop(void);
void host_link_send_beat(bool accent, uint8_t beat_in_bar, uint8_t step);
void host_link_send_position(uint32_t bar, uint8_t step, uint8_t beat, uint16_t tick,
                             bool accent);
void host_link_send_note(uint8_t note, uint8_t velocity);
/** Live key feedback for host UI: index 0..7 = S1..S8, down=1/0. */
void host_link_send_key(uint8_t index, bool down);
void host_link_send_status(uint16_t bpm, bool running, uint8_t beat_in_bar, uint8_t step,
                           uint32_t bar, uint16_t tick, bool drum_mode, uint8_t volume,
                           uint8_t brightness, uint8_t playback_count);
void host_link_send_pattern_dump(void);
void host_link_send_ack(const char *cmd, bool ok, uint32_t rev);
void host_link_send_error(const char *cmd, const char *msg);
/** 透传一行 JSON 给电脑（recorder 发乐谱清单等复用）。 */
void host_link_send_line(const char *line);
/**
 * 八音琴：把一条日志以换行 JSON 上报给电脑：{"t":"log","tag":"...","msg":"..."}。
 * 走已被证实在 COM5 可用的 host_link 通道（IDF console 在该板不可见）。
 * tag 建议用模块名（voice/mode/play/pad/enc/song_play…）。
 */
void host_link_send_log(const char *tag, const char *msg);

typedef void (*host_on_transport_fn)(bool start, bool restart);
typedef void (*host_on_bpm_fn)(uint16_t bpm);
typedef void (*host_on_swing_fn)(uint8_t swing);
typedef void (*host_on_variation_fn)(uint8_t var);
typedef void (*host_on_fill_fn)(bool held);
typedef void (*host_on_note_fn)(uint8_t note, uint8_t velocity);
typedef void (*host_on_click_fn)(bool enabled);
typedef void (*host_on_mode_fn)(bool drum_mode);
typedef void (*host_on_volume_fn)(uint8_t volume);
typedef void (*host_on_brightness_fn)(uint8_t brightness);
typedef void (*host_on_pattern_set_fn)(uint8_t bank, uint32_t rev, const uint8_t *bytes);
typedef void (*host_on_save_fn)(void);
typedef void (*host_on_ping_fn)(void);
/** Host overdub arming: when true, device ignores S7/S8/encoder transport. */
typedef void (*host_on_record_fn)(bool armed);
/** Host requests playback of the last recording (same as S8 long press). */
typedef void (*host_on_playback_fn)(void);
/** Host selects a built-in song and enters learn mode (web song dropdown). */
typedef void (*host_on_song_fn)(int idx);
/** Host requests auto-play (demo) of the current song. */
typedef void (*host_on_demo_fn)(void);
/** Host begins a voice import of total_bytes (base64 chunks follow). */
typedef void (*host_on_voice_begin_fn)(uint32_t total_bytes);
/** Host streams one base64 chunk of the voice import. */
typedef void (*host_on_voice_data_fn)(const char *b64);
/** Host ends the voice import (commit to Flash). */
typedef void (*host_on_voice_end_fn)(void);
/** Host requests replay of the greeting now. */
typedef void (*host_on_voice_replay_fn)(void);
/** Host requests clearing the imported voice (revert to embedded default). */
typedef void (*host_on_voice_default_fn)(void);
/** Host saves the current RAM recording into flash slot s (manual save only). */
typedef void (*host_on_rec_save_fn)(uint8_t slot);
/** Host loads flash slot s into RAM for playback / edit. */
typedef void (*host_on_rec_load_fn)(uint8_t slot);
/** Host deletes flash slot s. */
typedef void (*host_on_rec_del_fn)(uint8_t slot);
/** Host requests the list of used flash slots. */
typedef void (*host_on_rec_list_fn)(void);
/** Host loads flash slot s and starts playback immediately. */
typedef void (*host_on_rec_play_fn)(uint8_t slot);
/* ---- 八音琴曲库（5 槽，RAM）：网页 song_add / song_del / song_list / song_play ---- */
/** 网页 song_add：向槽 slot 写入一首歌（name + notes）。 */
typedef void (*host_on_song_add_fn)(uint8_t slot, const char *name, const song_ev_t *ev, int count);
/** 网页 song_del：清空槽 slot。 */
typedef void (*host_on_song_del_fn)(uint8_t slot);
/** 网页 song_list：请板子回报 5 槽清单。 */
typedef void (*host_on_song_list_fn)(void);
/** 网页 song_play：播放槽 slot（试听）。 */
typedef void (*host_on_song_play_fn)(uint8_t slot);
/* ---- 台架自测诊断命令（BAYIN_ENABLE_DIAG；见 diag_config.h）---- */
/** rec_inject：把合成乐谱注入回放池（等价自弹记谱产物）。 */
typedef void (*host_on_rec_inject_fn)(const song_ev_t *ev, int count);
/** pb_play：直接起播回放池第 slot 槽（等价回放模式按 S1）。 */
typedef void (*host_on_pb_play_fn)(uint8_t slot);

typedef struct {
    host_on_transport_fn on_transport;
    host_on_bpm_fn on_bpm;
    host_on_swing_fn on_swing;
    host_on_variation_fn on_variation;
    host_on_fill_fn on_fill;
    host_on_note_fn on_note;
    host_on_click_fn on_click;
    host_on_mode_fn on_mode;
    host_on_volume_fn on_volume;
    host_on_brightness_fn on_brightness;
    host_on_pattern_set_fn on_pattern_set;
    host_on_save_fn on_save;
    host_on_ping_fn on_ping;
    host_on_record_fn on_record;
    host_on_playback_fn on_playback;
    host_on_song_fn on_song;
    host_on_demo_fn on_demo;
    host_on_voice_begin_fn on_voice_begin;
    host_on_voice_data_fn on_voice_data;
    host_on_voice_end_fn on_voice_end;
    host_on_voice_replay_fn on_voice_replay;
    host_on_voice_default_fn on_voice_default;
    host_on_rec_save_fn on_rec_save;
    host_on_rec_load_fn on_rec_load;
    host_on_rec_del_fn on_rec_del;
    host_on_rec_list_fn on_rec_list;
    host_on_rec_play_fn on_rec_play;
    host_on_song_add_fn on_song_add;
    host_on_song_del_fn on_song_del;
    host_on_song_list_fn on_song_list;
    host_on_song_play_fn on_song_play;
    /* 台架自测诊断命令（BAYIN_ENABLE_DIAG） */
    host_on_rec_inject_fn on_rec_inject;
    host_on_pb_play_fn on_pb_play;
} host_link_handlers_t;

void host_link_set_handlers(const host_link_handlers_t *handlers);
void host_link_poll_rx(void);

/* 解析并执行一行 JSON 命令（host_link_poll_rx 内部调用）。
 * 暴露出来仅用于宿主测试，正常固件请勿直接调用。 */
void host_link_handle_line(const char *line);

#ifdef __cplusplus
}
#endif
