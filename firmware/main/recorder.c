#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "host_link.h"
#include "esp_partition.h"

static const char *TAG = "recorder";

static recorder_event_t s_events[RECORDER_MAX_EVENTS];
static int s_count;
static bool s_recording;
static int64_t s_rec_last_us;   /* 上一个被记录音符的时刻（0=尚未记到第一个） */
static bool s_playing;
static int s_play_idx;
static int64_t s_play_next_us;  /* 下一个音符应触发的时刻 */

static recorder_play_note_cb_t s_play_cb;

void recorder_set_play_note_cb(recorder_play_note_cb_t cb)
{
    s_play_cb = cb;
}

void recorder_init(void)
{
    s_count = 0;
    s_recording = false;
    s_playing = false;
    s_play_cb = NULL;
    s_rec_last_us = 0;
    ESP_LOGI(TAG, "recorder init (volatile RAM, no flash)");
}

void recorder_start(void)
{
    s_count = 0;
    s_recording = true;
    s_rec_last_us = 0; /* 第一个音的 delta 记为 0 */
    ESP_LOGI(TAG, "record start");
}

void recorder_stop(void)
{
    if (!s_recording) {
        return;
    }
    s_recording = false;
    ESP_LOGI(TAG, "record stop (%d notes in RAM, not saved to flash)", s_count);
}

bool recorder_is_recording(void)
{
    return s_recording;
}

void recorder_toggle_record(void)
{
    if (s_recording) {
        recorder_stop();
        recorder_send_to_host();
    } else {
        recorder_start();
    }
}

void recorder_add_note(uint8_t note, int64_t now_us)
{
    if (!s_recording) {
        return;
    }
    if (s_count >= RECORDER_MAX_EVENTS) {
        return; /* 满了就忽略，避免溢出 */
    }
    int64_t delta_us;
    if (s_rec_last_us == 0) {
        delta_us = 0; /* 第一个音：从录音开始算 0 */
        s_rec_last_us = now_us;
    } else {
        delta_us = now_us - s_rec_last_us;
        s_rec_last_us = now_us;
    }
    if (delta_us < 0) {
        delta_us = 0;
    }
    /* ★ 单位换算：now_us 是【微秒】，存储字段是【毫秒】——必须 /1000，
     * 否则 400ms 会被存成 400000 并被下面的 65535 钳住 → 回放要等 65.5 秒。 */
    int64_t delta_ms = delta_us / 1000;
    if (delta_ms > 65535) {
        delta_ms = 65535; /* 间隔封顶 ~65 秒 */
    }
    s_events[s_count].note = note;
    s_events[s_count].delta_ms = (uint16_t)delta_ms;
    s_count++;
}

void recorder_playback_start(int64_t now_us)
{
    if (s_count == 0 || s_playing) {
        return;
    }
    s_playing = true;
    s_play_idx = 0;
    /* 第一个音 delta 为 0 → 立刻触发 */
    s_play_next_us = now_us + (int64_t)s_events[0].delta_ms * 1000;
    ESP_LOGI(TAG, "playback start, %d notes", s_count);
}

bool recorder_is_playing(void)
{
    return s_playing;
}

void recorder_playback_tick(int64_t now_us)
{
    if (!s_playing) {
        return;
    }
    while (s_play_idx < s_count && now_us >= s_play_next_us) {
        const uint8_t note = s_events[s_play_idx].note;
        if (s_play_cb) {
            s_play_cb(note);
        }
        s_play_idx++;
        if (s_play_idx < s_count) {
            s_play_next_us += (int64_t)s_events[s_play_idx].delta_ms * 1000;
        } else {
            s_playing = false;
            ESP_LOGI(TAG, "playback done");
            break;
        }
    }
}

size_t recorder_export_json(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return 0;
    }
    int n = snprintf(buf, buflen, "{\"t\":\"rec\",\"n\":%d,\"ev\":[", s_count);
    if (n < 0) {
        return 0;
    }
    size_t off = (size_t)n;
    for (int i = 0; i < s_count; ++i) {
        if (off >= buflen) {
            break;
        }
        int k = snprintf(buf + off, buflen - off, "[%u,%u]%s",
                         (unsigned)s_events[i].note, (unsigned)s_events[i].delta_ms,
                         (i + 1 < s_count) ? "," : "");
        if (k < 0) {
            break;
        }
        off += (size_t)k;
    }
    if (off < buflen) {
        int k = snprintf(buf + off, buflen - off, "]}");
        if (k > 0) {
            off += (size_t)k;
        }
    }
    return off;
}

void recorder_send_to_host(void)
{
    if (s_count == 0) {
        return;
    }
    static char s_json[4200];
    const size_t len = recorder_export_json(s_json, sizeof(s_json));
    if (len > 0) {
        host_link_send_line(s_json);
    }
}

bool recorder_has_recording(void)
{
    return s_count > 0;
}

const recorder_event_t *recorder_events(void)
{
    return s_events;
}

int recorder_event_count(void)
{
    return s_count;
}

/* ---- 可选：手动保存到 flash 的 rec 分区（断电不丢、不绑电脑）---- */
#define REC_MAGIC 0x52454330u /* 'REC0' */
#define REC_HDR_BYTES 8u

static const esp_partition_t *rec_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                     REC_FLASH_PARTITION);
}

/*
 * ★ 根源级钳制（不是打补丁）：rec 分区槽位由 rec_layout.h 统一分配——
 *   槽 0~4  = 曲库（song_pool 持久化），槽 5~20 = 语音资源（prompt）。
 *   这两段被独占，recorder 的旧 flash 写命令（rec_save / rec_del）绝不允许写它们；
 *   否则串口误发 rec_save/rec_del 会毁掉曲库曲与语音。
 *   只有 legacy 段（slot >= REC_SLOT_LEGACY_BASE=26）才允许 recorder 读写。
 *   边界断言（编译期）保证分配关系不被后续改动悄悄破坏。
 */
_Static_assert(REC_SLOT_PROTECTED_END == REC_SLOT_LEGACY_BASE, "protect-end mismatch");
_Static_assert(REC_SLOT_SONGBOOK_BASE + REC_SLOT_SONGBOOK_COUNT == REC_SLOT_PROMPT_BASE,
               "songbook/prompt ranges must be contiguous");
_Static_assert(REC_SLOT_PROMPT_BASE + REC_SLOT_PROMPT_COUNT == REC_SLOT_PLAYBACK_BASE,
               "prompt/playback ranges must be contiguous");
_Static_assert(REC_SLOT_PLAYBACK_BASE + REC_SLOT_PLAYBACK_COUNT == REC_SLOT_LEGACY_BASE,
               "playback/legacy ranges must be contiguous");
_Static_assert(REC_SLOT_LEGACY_BASE < REC_FLASH_SLOTS, "recorder needs at least one slot");

static bool recorder_slot_writable(uint8_t slot)
{
    return slot >= REC_SLOT_LEGACY_BASE && slot < REC_FLASH_SLOTS;
}

esp_err_t recorder_save_slot(uint8_t slot)
{
    if (!recorder_slot_writable(slot)) {
        ESP_LOGW(TAG, "blocked: slot %u is protected (曲库/语音区), recorder 只能用 slot>=%u",
                 (unsigned)slot, (unsigned)REC_SLOT_LEGACY_BASE);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count == 0) {
        return ESP_ERR_INVALID_STATE; /* 没录到东西，没得存 */
    }
    const esp_partition_t *part = rec_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = (uint32_t)slot * REC_SLOT_SIZE;
    if (off + REC_SLOT_SIZE > part->size) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 打包：头(魔数+数量) + 事件(note 1B, delta 2B) */
    uint8_t hdr[REC_HDR_BYTES];
    const uint32_t magic = REC_MAGIC;
    const uint16_t cnt = (uint16_t)s_count;
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &cnt, 2);

    const size_t body_len = (size_t)s_count * 3;
    uint8_t *body = malloc(body_len ? body_len : 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; ++i) {
        body[i * 3] = s_events[i].note;
        memcpy(body + i * 3 + 1, &s_events[i].delta_ms, 2);
    }
    /* 整块擦写这一槽（不碰其他槽），符合铁律：固定分区、不无限增长 */
    esp_err_t e = esp_partition_erase_range(part, off, REC_SLOT_SIZE);
    if (e == ESP_OK) {
        e = esp_partition_write(part, off, hdr, REC_HDR_BYTES);
    }
    if (e == ESP_OK) {
        e = esp_partition_write(part, off + REC_HDR_BYTES, body, body_len);
    }
    free(body);
    ESP_LOGI(TAG, "saved slot %u (%d notes) -> %s", slot, s_count, esp_err_to_name(e));
    return e;
}

esp_err_t recorder_load_slot(uint8_t slot)
{
    if (!recorder_slot_writable(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = rec_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = (uint32_t)slot * REC_SLOT_SIZE;
    uint8_t hdr[REC_HDR_BYTES];
    if (esp_partition_read(part, off, hdr, REC_HDR_BYTES) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t magic = 0;
    uint16_t cnt = 0;
    memcpy(&magic, hdr, 4);
    memcpy(&cnt, hdr + 4, 2);
    if (magic != REC_MAGIC) {
        return ESP_ERR_NOT_FOUND; /* 空槽或损坏 */
    }
    if (cnt > RECORDER_MAX_EVENTS) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < cnt; ++i) {
        uint8_t note = 0;
        uint16_t delta = 0;
        esp_partition_read(part, off + REC_HDR_BYTES + (uint32_t)i * 3, &note, 1);
        esp_partition_read(part, off + REC_HDR_BYTES + (uint32_t)i * 3 + 1, &delta, 2);
        s_events[i].note = note;
        s_events[i].delta_ms = delta;
    }
    s_count = cnt;
    s_recording = false;
    s_playing = false;
    s_rec_last_us = 0;
    ESP_LOGI(TAG, "loaded slot %u (%d notes)", slot, s_count);
    return ESP_OK;
}

esp_err_t recorder_delete_slot(uint8_t slot)
{
    if (!recorder_slot_writable(slot)) {
        ESP_LOGW(TAG, "blocked: slot %u is protected (曲库/语音区)", (unsigned)slot);
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = rec_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = (uint32_t)slot * REC_SLOT_SIZE;
    return esp_partition_erase_range(part, off, REC_SLOT_SIZE);
}

bool recorder_slot_used(uint8_t slot)
{
    if (slot >= REC_FLASH_SLOTS) {
        return false;
    }
    const esp_partition_t *part = rec_partition();
    if (part == NULL) {
        return false;
    }
    const uint32_t off = (uint32_t)slot * REC_SLOT_SIZE;
    uint32_t magic = 0;
    if (esp_partition_read(part, off, &magic, 4) != ESP_OK) {
        return false;
    }
    return magic == REC_MAGIC;
}

uint8_t recorder_slot_count(void)
{
    uint8_t n = 0;
    for (uint8_t s = 0; s < REC_FLASH_SLOTS; ++s) {
        if (recorder_slot_used(s)) {
            n++;
        }
    }
    return n;
}
