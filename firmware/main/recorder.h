#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

/*
 * 阶段② 录音 / 回放。
 *
 * 记录的是"按了哪个音 + 距上一个音隔了多久"，不是录音波形本身
 * （波形靠离线采样播放，录音只要记'谱'就行，省空间、也好回放）。
 *
 * 录音瞬态数据放 RAM 缓存（重启就丢）。停录时把谱序列化后经 host link 发给电脑
 * 下载保存（也可离线用）。若要断电不丢、不绑电脑，手动"保存"才把当前谱
 * 整块擦写进 rec 分区（固定 32 槽，每槽 32KB，共 1MB）——不无限增长、不反复磨损，
 * 符合存储铁律。最多记 RECORDER_MAX_EVENTS 个音。
 */

#define RECORDER_MAX_EVENTS 400

typedef struct {
    uint8_t note;       /* 0..6，对应 do~si */
    uint16_t delta_ms;  /* 距上一个音符的间隔（毫秒），第一个音为 0 */
} recorder_event_t;

/* 回放时每触发一个音符，调用此回调让上层发声 + 亮灯（解耦音频/LED） */
typedef void (*recorder_play_note_cb_t)(uint8_t note);

void recorder_init(void);
void recorder_set_play_note_cb(recorder_play_note_cb_t cb);

void recorder_start(void);       /* 开始录音（清空缓冲） */
void recorder_stop(void);        /* 停止录音（仍在 RAM 缓存，不写 Flash） */
void recorder_toggle_record(void);
bool recorder_is_recording(void);

void recorder_add_note(uint8_t note, int64_t now_us); /* 录音中按到音符键时调用 */

void recorder_playback_start(int64_t now_us);
bool recorder_is_playing(void);
void recorder_playback_tick(int64_t now_us); /* 主循环每帧调用，按计划触发音符 */

/* 把当前录好的谱序列化成 JSON（{"t":"rec","n":N,"ev":[[note,delta_ms],...]}）。
 * 返回写入长度（不含结尾\0）；buf 太小则返回 0。宿主测试与发电脑复用。 */
size_t recorder_export_json(char *buf, size_t buflen);

/* 把谱经 host link 发给电脑（由电脑下载保存）。无录音则不发。 */
void recorder_send_to_host(void);

bool recorder_has_recording(void);

/* 读取当前 RAM 谱（八音琴自弹记谱结束后转入回放池用）。未录音时 count=0。 */
const recorder_event_t *recorder_events(void);
int recorder_event_count(void);

/* ---- 可选：手动保存到 flash 的 rec 分区（断电不丢、不绑电脑）----
 * 注意：rec 分区槽位现由 rec_layout.h 统一分配。曲库(slot0~4)/语音(slot5~20)
 * 受保护，recorder 的旧 flash 命令只允许使用 slot >= REC_SLOT_LEGACY_BASE(21)。 */
#include "rec_layout.h"

esp_err_t recorder_save_slot(uint8_t slot);    /* 把当前 RAM 谱写入 slot（仅 slot>=21） */
esp_err_t recorder_load_slot(uint8_t slot);    /* 从 slot 读入 RAM（回放 / 续录） */
esp_err_t recorder_delete_slot(uint8_t slot); /* 清空某槽（仅 slot>=21） */
bool recorder_slot_used(uint8_t slot);         /* 该槽是否有有效谱（magic 校验） */
uint8_t recorder_slot_count(void);             /* 已用槽数 */
