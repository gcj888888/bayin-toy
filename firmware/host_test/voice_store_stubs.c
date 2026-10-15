/* 宿主测试用的 voice_store / recorder 依赖替身：
 *  - esp_partition：用两块内存模拟 voice(2MB) 与 rec(1MB) 分区
 *    （擦=全 0xFF，写/读=内存拷贝；erase_range 只清指定范围，不影响其他槽）
 *  - audio_click_play_pcm：记录"被播了什么"，供测试断言
 *  - 内嵌默认语音：用 asm 符号提供 _binary_voice_default_raw_start/end
 *  - rec_part_*：供测试读取 rec 分区内存，验证"落分区"。 */
#include "audio_click.h"
#include "esp_partition.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef VOICE_HEADER_BYTES
#define VOICE_HEADER_BYTES 8u
#endif

#define REC_STUB_MAGIC 0x52454330u
#define REC_STUB_SLOT  32768u

/* ---- 播放记录（测试读取） ---- */
const int16_t *g_pcm_ptr;
uint32_t g_pcm_frames;
int g_pcm_played;

esp_err_t audio_click_play_pcm(const int16_t *data, uint32_t frames)
{
    g_pcm_ptr = data;
    g_pcm_frames = frames;
    g_pcm_played = 1;
    return 0;
}

/* ---- 假分区（内存）：voice 2MB + rec 1MB ---- */
static uint8_t s_voice_buf[2 * 1024 * 1024];
static uint8_t s_rec_buf[1 * 1024 * 1024];
static uint8_t *g_bufs[2] = { s_voice_buf, s_rec_buf };
static size_t g_sizes[2] = { sizeof(s_voice_buf), sizeof(s_rec_buf) };
static esp_partition_t s_parts[2];
bool g_part_present = true;

static int part_id_by_label(const char *label)
{
    if (label && strcmp(label, "voice") == 0) {
        return 0;
    }
    if (label && strcmp(label, "rec") == 0) {
        return 1;
    }
    return -1;
}

const esp_partition_t *esp_partition_find_first(uint32_t type, uint32_t subtype, const char *label)
{
    (void)type; (void)subtype;
    if (!g_part_present) {
        return NULL;
    }
    const int id = part_id_by_label(label);
    if (id < 0) {
        return NULL;
    }
    s_parts[id].address = (uint32_t)id;
    s_parts[id].size = (uint32_t)g_sizes[id];
    s_parts[id].type = 0x40;
    s_parts[id].subtype = (id == 0) ? 0x40 : 0x41;
    s_parts[id].label = (id == 0) ? "voice" : "rec";
    return &s_parts[id];
}

esp_err_t esp_partition_read(const esp_partition_t *p, uint32_t off, void *dst, size_t size)
{
    const int id = (int)p->address;
    if ((size_t)off + size > g_sizes[id]) {
        return -1;
    }
    memcpy(dst, g_bufs[id] + off, size);
    return 0;
}

esp_err_t esp_partition_write(const esp_partition_t *p, uint32_t off, const void *src, size_t size)
{
    const int id = (int)p->address;
    if ((size_t)off + size > g_sizes[id]) {
        return -1;
    }
    memcpy(g_bufs[id] + off, src, size);
    return 0;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, uint32_t start, uint32_t size)
{
    const int id = (int)p->address;
    if ((size_t)start + size > g_sizes[id]) {
        return -1;
    }
    memset(g_bufs[id] + start, 0xFF, size);
    return 0;
}

/* ---- 内嵌默认语音：2 帧（4 字节）假 PCM ---- */
const uint8_t fake_voice[4] asm("_binary_voice_default_raw_start") = {0x01, 0x00, 0x02, 0x00};
const uint8_t fake_voice_end asm("_binary_voice_default_raw_end") = 0;

/* ---- 供测试读取：voice 分区内存里写进去了什么（验证"落分区"） ---- */
uint32_t voice_part_magic(void)
{
    uint32_t v = 0;
    memcpy(&v, s_voice_buf, sizeof(v));
    return v;
}

uint32_t voice_part_len(void)
{
    uint32_t v = 0;
    memcpy(&v, s_voice_buf + 4, sizeof(v));
    return v;
}

/* ---- 供测试读取：rec 分区内存 ---- */
uint32_t rec_part_magic(uint8_t slot)
{
    uint32_t v = 0;
    if ((uint32_t)slot * REC_STUB_SLOT + 4 > sizeof(s_rec_buf)) {
        return 0;
    }
    memcpy(&v, s_rec_buf + (uint32_t)slot * REC_STUB_SLOT, 4);
    return v;
}

bool rec_part_used(uint8_t slot)
{
    return rec_part_magic(slot) == REC_STUB_MAGIC;
}
