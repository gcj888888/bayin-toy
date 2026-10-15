#pragma once

/*
 * rec 数据分区槽位分配（单一事实源）。
 *
 * rec 分区 = 1MB，按 32KB 一槽切分，共 32 槽。八音琴把它三段式使用：
 *
 *   槽 0 ~ 4   ：曲库（song_pool 的网页持久化曲）        —— 5 槽
 *   槽 5 ~ 20  ：语音提示资源（VPMT 打包，含索引表）      —— 16 槽上限
 *   槽 21 ~ 31 ：留给 recorder 旧 flash 槽（legacy）      —— 11 槽
 *
 * ★ 铁律：曲库槽与语音槽被 song_pool / prompt 独占，recorder 的旧 flash 写命令
 *   （rec_save / rec_del）绝不允许写这两段，见 recorder.c 的根源级钳制。
 *
 * 本头文件不依赖任何 ESP-IDF 组件，可被纯逻辑模块与宿主测试直接包含。
 */

#define REC_FLASH_PARTITION "rec"

#define REC_FLASH_SLOTS 32u   /* 总槽数 */
#define REC_SLOT_SIZE 32768u  /* 每槽字节数（32KB） */

/* 曲库段（song_pool flash 持久化） */
#define REC_SLOT_SONGBOOK_BASE 0u
#define REC_SLOT_SONGBOOK_COUNT 5u

/* 语音提示段（prompt 资源） */
#define REC_SLOT_PROMPT_BASE 5u
#define REC_SLOT_PROMPT_COUNT 16u

/* 回放池段（play_pool 持久化；最多 5 首，FIFO 覆盖最旧） */
#define REC_SLOT_PLAYBACK_BASE (REC_SLOT_PROMPT_BASE + REC_SLOT_PROMPT_COUNT) /* 21 */
#define REC_SLOT_PLAYBACK_COUNT 5u

/* recorder 旧 flash 槽可用的起始槽（= 曲库 + 语音 + 回放池 段之后） */
#define REC_SLOT_LEGACY_BASE (REC_SLOT_PLAYBACK_BASE + REC_SLOT_PLAYBACK_COUNT) /* 26 */

/* 受保护段（曲库 + 语音 + 回放池）：slot < REC_SLOT_LEGACY_BASE 一律禁止 recorder 写 */
#define REC_SLOT_PROTECTED_END REC_SLOT_LEGACY_BASE
