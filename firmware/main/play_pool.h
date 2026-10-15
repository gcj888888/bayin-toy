#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "song_pool.h" /* 复用 song_ev_t（音号+间隔） */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 八音琴「回放池」——独立于试听池（flash 曲库）的第 2 个 5 槽池。
 *
 *   - 素材来源：自弹记谱 + 游戏通关 + 台架注入
 *   - **持久化到 rec 分区槽 21~25（5 槽）**：push 同时写 RAM 与 flash
 *   - 上限 5 首，超出**FIFO 覆盖最旧的**（"最旧"按槽头里的**单调 seq** 判断，不看槽序）
 *   - 每槽带头部：magic 'PBPL' + version + seq + count + 简单校验 + 名字
 *   - 开机从 flash 重建 5 槽，按 seq 恢复 FIFO 顺序
 *   - 回放模式只列本池；内置 3 首/网页曲（flash 曲库）绝不出现于回放
 */

#define PLAY_POOL_SLOTS 5
#define PLAY_POOL_MAX_EVENTS 400
#define PLAY_POOL_NAME_MAX 24

/* 起播首音延迟钳制上限（ms）：真实录音的首音间隔可能很大（开头静默会被误当成
 * "没播放"），起播时钳到该上限。**只在这一处钳，不动已存数据。** */
#define PLAY_FIRST_NOTE_DELAY_MAX_MS 300u

static inline uint16_t play_first_delay_clamp(uint16_t delta_ms)
{
    return delta_ms > PLAY_FIRST_NOTE_DELAY_MAX_MS ? (uint16_t)PLAY_FIRST_NOTE_DELAY_MAX_MS
                                                   : delta_ms;
}

void playpool_init(void);      /* 仅清 RAM（开机） */
void playpool_clear_all(void); /* 清 RAM + 擦 flash 5 槽（彻底清空） */

/* 追加一首（FIFO，满 5 覆盖最旧）。count<=0 或 ev==NULL 返回 -1，否则返回落入槽号。
 * ★ 同时写 RAM 与 flash（rec_inject 诊断命令走同一入口，故注入也可持久化）。 */
int playpool_push(const char *name, const song_ev_t *ev, int count);
/* 下一次 push 会落入哪个槽（不写入）。自弹记谱时用于点亮"目标槽位 D_N"。 */
int playpool_next_slot(void);

int playpool_used_count(void);
bool playpool_slot_used(int slot);
const char *playpool_name(int slot); /* 未使用返回 "" */
const song_ev_t *playpool_events(int slot);
int playpool_event_count(int slot);

/* ---- flash 持久化（rec 分区槽 21~25；每槽 32KB）---- */
int playpool_load_from_flash(void);     /* 开机：重建 5 槽，按 seq 恢复 FIFO；返回加载槽数 */
esp_err_t playpool_flash_save(int slot);   /* 把 RAM 槽写进 flash 槽 */
esp_err_t playpool_flash_clear(int slot);  /* 擦掉 flash 槽 */
bool playpool_flash_used(int slot);        /* 该 flash 槽头是否有效 */
uint32_t playpool_slot_seq(int slot);      /* 该槽写序（FIFO 判据；未用返回 0） */

#ifdef __cplusplus
}
#endif
