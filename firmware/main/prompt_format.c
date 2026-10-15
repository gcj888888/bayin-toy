#include "prompt_format.h"

#include <string.h>

/* 纯逻辑实现：不依赖任何 ESP-IDF 组件，可直接在电脑上编译测试。 */

static const char *const k_prompt_text[PROMPT_COUNT] = {
    "请选择模式", "试听模式", "自弹模式", "游戏模式", "回放模式",
    "小星星",     "两只老虎", "欢乐颂",   "开始记谱", "已保存",
    "播放结束",   "暂无记录", "本键无功能", "回到主目录",
};

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int prompt_parse_header(const uint8_t *buf, size_t len, int *count_out)
{
    if (buf == NULL || len < PROMPT_HDR_BYTES) {
        return -1;
    }
    if (memcmp(buf, PROMPT_MAGIC_STR, 4) != 0) {
        return -1;
    }
    const uint16_t version = rd_u16(buf + 4);
    if (version == 0) {
        return -1;
    }
    const uint16_t count = rd_u16(buf + 6);
    if (count < 1 || count > (uint16_t)PROMPT_COUNT) {
        return -1;
    }
    if (count_out != NULL) {
        *count_out = (int)count;
    }
    return (int)version;
}

int prompt_parse_entry(const uint8_t *buf, size_t len, prompt_entry_t *out)
{
    if (buf == NULL || out == NULL || len < PROMPT_ENTRY_BYTES) {
        return -1;
    }
    const uint16_t slot = rd_u16(buf);
    const uint32_t offset = rd_u32(buf + 2);
    const uint32_t length = rd_u32(buf + 6);
    if (length == 0 || (length & 1u) != 0) {
        return -1; /* 空句或非整样本 */
    }
    if ((uint64_t)offset + length > (uint64_t)PROMPT_REGION_BYTES) {
        return -1; /* 越界 */
    }
    if (slot != (uint16_t)(REC_SLOT_PROMPT_BASE + offset / REC_SLOT_SIZE)) {
        return -1; /* slot 与 offset 不一致 */
    }
    out->slot = slot;
    out->offset = offset;
    out->length = length;
    return 0;
}

const char *prompt_text(int id)
{
    if (id < 0 || id >= PROMPT_COUNT) {
        return "";
    }
    return k_prompt_text[id];
}
