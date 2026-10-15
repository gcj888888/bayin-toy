#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 八音琴曲库 / 回放池。
 *
 * 5 槽：
 *   - 网页 song_add / song_del / song_list / song_play 直接操作这 5 槽
 *   - 自弹记谱 + 游戏通关保存：FIFO 追加，满 5 覆盖最旧的；只进 RAM，不写 flash
 *   - 网页 song_add 会把槽写进 flash（复用 recorder 的 "rec" 分区前 5 槽）→ 掉电不丢
 *   - 开机从 flash 加载 5 槽到 RAM；加载失败/为空按空处理
 *
 * 试听与回放共用同一份 5 槽：空闲灯"亮几颗 = 已存槽数"。
 */

#define SONGP_SLOTS 5
#define SONGP_MAX_EVENTS 400
#define SONGP_NAME_MAX 24

typedef struct {
    uint8_t note;      /* 0~7，对应 music_note_t */
    uint16_t delta_ms; /* 距上一音的间隔（毫秒），第一个音为 0 */
} song_ev_t;

void songpool_init(void);
void songpool_clear_all(void);

/* 网页 song_add：写入指定槽（覆盖旧内容）。slot 非法或 count<=0 返回 -1，否则返回槽号。 */
int songpool_set(int slot, const char *name, const song_ev_t *ev, int count);
/* 网页 song_del：清空指定槽。 */
int songpool_del(int slot);
/* 自弹/游戏写入：FIFO 追加，满 5 覆盖最旧；返回落入的槽号；count<=0 返回 -1。只进 RAM。 */
int songpool_push(const char *name, const song_ev_t *ev, int count);
/* 下一次 push 会落入哪个槽（不写入）。用于自弹记谱时点亮"目标槽位 D_N"。 */
int songpool_next_slot(void);

int songpool_used_count(void);
bool songpool_slot_used(int slot);
const char *songpool_name(int slot);          /* 未使用返回 "" */
const song_ev_t *songpool_events(int slot);
int songpool_event_count(int slot);

/* ---- flash 持久化（复用 recorder 的 "rec" 分区前 5 槽；不新建分区、不改分区表、不擦 NVS）---- */

/* 把 RAM 槽 slot 写进 flash 槽 slot（网页 song_add 用）。 */
esp_err_t songpool_flash_save(int slot);
/* 清空 flash 槽 slot（网页 song_del 用）。 */
esp_err_t songpool_flash_clear(int slot);
/* 开机：把 flash 5 槽加载进 RAM；返回成功加载的槽数（分区缺失/为空返回 0，不崩）。 */
int songpool_load_from_flash(void);
/* song_list 以 flash 为准：该 flash 槽是否有有效曲。 */
bool songpool_flash_used(int slot);
/* 读 flash 槽元信息：填 name 并返回事件数；未使用/非法返回 0。 */
int songpool_flash_meta(int slot, char *name_out, int name_cap);

#ifdef __cplusplus
}
#endif

