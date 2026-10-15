#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rec_layout.h"

/*
 * 八音琴语音提示：清单定义 + 资源索引格式（纯逻辑，可宿主测试）。
 *
 * 定稿 = 14 句静态清单（放弃动态报数"共N首/第N首"），id 顺序即清单顺序。
 *
 * 资源格式（由 tools/make_voice_prompts.py 打包，写入 rec 分区 slot5 起）：
 *   头 16 字节：magic(u32 'VPMT') + version(u16) + count(u16) + 保留(8)
 *   索引 count × 12 字节：slot(u16) + offset(u32) + length(u32) + 保留(u16)
 *   数据：每句 PCM（16kHz 单声道 int16 小端），offset 为相对资源区起始的字节偏移，
 *         length 为字节数（int16 个数 = length/2）。单句可跨槽，故索引记录 slot+offset。
 */

typedef enum {
    PROMPT_SELECT_MODE = 0, /*  1 请选择模式 */
    PROMPT_MODE_TRIAL,      /*  2 试听模式 */
    PROMPT_MODE_SELF,       /*  3 自弹模式 */
    PROMPT_MODE_GAME,       /*  4 游戏模式 */
    PROMPT_MODE_PLAYBACK,   /*  5 回放模式 */
    PROMPT_SONG_TWINKLE,    /*  6 小星星 */
    PROMPT_SONG_TWO_TIGERS, /*  7 两只老虎 */
    PROMPT_SONG_ODE,        /*  8 欢乐颂 */
    PROMPT_START_RECORD,    /*  9 开始记谱 */
    PROMPT_SAVED,           /* 10 已保存 */
    PROMPT_PLAY_END,        /* 11 播放结束 */
    PROMPT_NO_RECORD,       /* 12 暂无记录 */
    PROMPT_NO_FUNCTION,     /* 13 本键无功能 */
    PROMPT_BACK_HOME,       /* 14 回到主目录 */
    PROMPT_COUNT
} prompt_id_t;

#define PROMPT_MAGIC_STR "VPMT" /* 资源文件前 4 字节 ASCII 标记（按字节原样比较，避免端序坑） */
#define PROMPT_VERSION 1u
#define PROMPT_HDR_BYTES 16u
#define PROMPT_ENTRY_BYTES 12u
#define PROMPT_SAMPLE_RATE 16000u /* 资源采样率（播放时 2× 上采样到 32kHz） */

/* 资源区容量（字节）= 语音段槽数 × 槽大小 */
#define PROMPT_REGION_BYTES (REC_SLOT_PROMPT_COUNT * REC_SLOT_SIZE)

typedef struct {
    uint16_t slot;   /* 起始槽号（应 = REC_SLOT_PROMPT_BASE + offset/REC_SLOT_SIZE） */
    uint32_t offset; /* 相对资源区起始的字节偏移 */
    uint32_t length; /* 字节数（int16 个数 = length/2） */
} prompt_entry_t;

/* 解析资源头。成功返回版本号(>=1)，失败返回 -1；成功时 *count_out = 句数。 */
int prompt_parse_header(const uint8_t *buf, size_t len, int *count_out);

/* 解析一条索引（含 slot/offset 一致性与容量校验）。成功返回 0，失败返回 -1。 */
int prompt_parse_entry(const uint8_t *buf, size_t len, prompt_entry_t *out);

/* 清单文案（id → 中文），非法 id 返回 ""。 */
const char *prompt_text(int id);
