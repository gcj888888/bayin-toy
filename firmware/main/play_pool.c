#include "play_pool.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "rec_layout.h"

/*
 * 回放池：RAM 本体 + **写穿到 rec 分区槽 21~25** 的持久化。
 * 每槽带头部（magic/version/seq/count/checksum/name），"最旧"按 seq 判断。
 */

static const char *TAG = "playpool";

typedef struct {
    bool used;
    int count;
    uint32_t seq; /* 写序（单调递增）：越大越新，FIFO 覆盖最旧的（不看槽序） */
    char name[PLAY_POOL_NAME_MAX];
    song_ev_t ev[PLAY_POOL_MAX_EVENTS];
} play_slot_t;

static play_slot_t s_slots[PLAY_POOL_SLOTS];
static uint32_t s_seq_top;

/* ---- flash 槽头格式 ---- */
#define PLAY_FLASH_MAGIC 0x4C504250u /* 'PBPL'（内存字节序 P B P L） */
#define PLAY_FLASH_VERSION 1u
#define PLAY_FLASH_HDR 32u /* magic(4) ver(2) count(2) seq(4) chk(4) name_len(1) 保留(15) */

static const esp_partition_t *play_flash_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                    REC_FLASH_PARTITION);
}

static uint32_t play_flash_offset(int slot)
{
    return (REC_SLOT_PLAYBACK_BASE + (uint32_t)slot) * REC_SLOT_SIZE;
}

/* 简单校验：名字 + 事件的字节和 */
static uint32_t play_body_checksum(const uint8_t *body, size_t name_len, int count)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < name_len; ++i) {
        sum += body[i];
    }
    for (size_t i = 0; i < (size_t)count * 3u; ++i) {
        sum += body[name_len + i];
    }
    return sum;
}

static void ram_clear(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_seq_top = 0;
}

void playpool_init(void)
{
    ram_clear(); /* 仅清 RAM；由调用方随后 playpool_load_from_flash() 重建 */
}

void playpool_clear_all(void)
{
    ram_clear();
    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        (void)playpool_flash_clear(i);
    }
}

static void copy_events(play_slot_t *dst, const song_ev_t *ev, int count)
{
    if (count > PLAY_POOL_MAX_EVENTS) {
        count = PLAY_POOL_MAX_EVENTS;
    }
    if (count < 0) {
        count = 0;
    }
    for (int i = 0; i < count; ++i) {
        dst->ev[i] = ev[i];
    }
    dst->count = count;
}

static void copy_name(char *dst, const char *name)
{
    if (name == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(name);
    if (n >= PLAY_POOL_NAME_MAX) {
        n = PLAY_POOL_NAME_MAX - 1;
    }
    memcpy(dst, name, n);
    dst[n] = '\0';
}

int playpool_next_slot(void)
{
    /* 有空槽则用最前的空槽；否则选 seq 最小的（最旧） */
    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        if (!s_slots[i].used) {
            return i;
        }
    }
    int target = 0;
    for (int i = 1; i < PLAY_POOL_SLOTS; ++i) {
        if (s_slots[i].seq < s_slots[target].seq) {
            target = i;
        }
    }
    return target;
}

esp_err_t playpool_flash_save(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_slots[slot].used || s_slots[slot].count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = play_flash_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = play_flash_offset(slot);
    if (off + REC_SLOT_SIZE > part->size) {
        return ESP_ERR_INVALID_ARG;
    }

    const int count = s_slots[slot].count;
    size_t name_len = strlen(s_slots[slot].name);
    if (name_len > PLAY_POOL_NAME_MAX - 1u) {
        name_len = PLAY_POOL_NAME_MAX - 1u;
    }

    const size_t body_len = name_len + (size_t)count * 3u; /* 每音 note(1)+delta(2) */
    uint8_t *body = malloc(body_len ? body_len : 1u);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, s_slots[slot].name, name_len);
    for (int i = 0; i < count; ++i) {
        body[name_len + (size_t)i * 3u] = s_slots[slot].ev[i].note;
        memcpy(body + name_len + (size_t)i * 3u + 1u, &s_slots[slot].ev[i].delta_ms, 2);
    }

    uint8_t hdr[PLAY_FLASH_HDR];
    memset(hdr, 0, sizeof(hdr));
    uint32_t magic = PLAY_FLASH_MAGIC;
    uint16_t ver = PLAY_FLASH_VERSION;
    uint16_t cnt = (uint16_t)count;
    uint32_t seq = s_slots[slot].seq;
    uint32_t chk = play_body_checksum(body, name_len, count);
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &ver, 2);
    memcpy(hdr + 6, &cnt, 2);
    memcpy(hdr + 8, &seq, 4);
    memcpy(hdr + 12, &chk, 4);
    hdr[16] = (uint8_t)name_len;

    esp_err_t e = esp_partition_erase_range(part, off, REC_SLOT_SIZE);
    if (e == ESP_OK) {
        e = esp_partition_write(part, off, hdr, PLAY_FLASH_HDR);
    }
    if (e == ESP_OK) {
        e = esp_partition_write(part, off + PLAY_FLASH_HDR, body, body_len);
    }
    free(body);
    ESP_LOGI(TAG, "flash save slot %d '%s' (%d notes, seq=%lu) -> %s", slot, s_slots[slot].name,
             count, (unsigned long)seq, esp_err_to_name(e));
    return e;
}

esp_err_t playpool_flash_clear(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = play_flash_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = play_flash_offset(slot);
    if (off + REC_SLOT_SIZE > part->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_erase_range(part, off, REC_SLOT_SIZE);
}

bool playpool_flash_used(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS) {
        return false;
    }
    const esp_partition_t *part = play_flash_partition();
    if (part == NULL) {
        return false;
    }
    uint8_t hdr[PLAY_FLASH_HDR];
    if (esp_partition_read(part, play_flash_offset(slot), hdr, PLAY_FLASH_HDR) != ESP_OK) {
        return false;
    }
    uint32_t magic = 0;
    uint16_t ver = 0;
    uint16_t cnt = 0;
    memcpy(&magic, hdr, 4);
    memcpy(&ver, hdr + 4, 2);
    memcpy(&cnt, hdr + 6, 2);
    return magic == PLAY_FLASH_MAGIC && ver == PLAY_FLASH_VERSION && cnt > 0 &&
           cnt <= PLAY_POOL_MAX_EVENTS && hdr[16] < PLAY_POOL_NAME_MAX;
}

uint32_t playpool_slot_seq(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS || !s_slots[slot].used) {
        return 0;
    }
    return s_slots[slot].seq;
}

int playpool_push(const char *name, const song_ev_t *ev, int count)
{
    if (count <= 0 || ev == NULL) {
        return -1;
    }
    const int target = playpool_next_slot();
    copy_events(&s_slots[target], ev, count);
    copy_name(s_slots[target].name, name);
    s_slots[target].used = true;
    s_slots[target].seq = ++s_seq_top;
    (void)playpool_flash_save(target); /* ★ 写穿 flash：持久化 */
    return target;
}

int playpool_used_count(void)
{
    int n = 0;
    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        if (s_slots[i].used) {
            n++;
        }
    }
    return n;
}

bool playpool_slot_used(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS) {
        return false;
    }
    return s_slots[slot].used;
}

const char *playpool_name(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS || !s_slots[slot].used) {
        return "";
    }
    return s_slots[slot].name;
}

const song_ev_t *playpool_events(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS || !s_slots[slot].used) {
        return NULL;
    }
    return s_slots[slot].ev;
}

int playpool_event_count(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS || !s_slots[slot].used) {
        return 0;
    }
    return s_slots[slot].count;
}

int playpool_load_from_flash(void)
{
    const esp_partition_t *part = play_flash_partition();
    if (part == NULL) {
        return 0;
    }
    int loaded = 0;
    uint32_t max_seq = 0;
    for (int slot = 0; slot < PLAY_POOL_SLOTS; ++slot) {
        uint8_t hdr[PLAY_FLASH_HDR];
        const uint32_t off = play_flash_offset(slot);
        if (esp_partition_read(part, off, hdr, PLAY_FLASH_HDR) != ESP_OK) {
            continue;
        }
        uint32_t magic = 0;
        uint16_t ver = 0;
        uint16_t cnt = 0;
        uint32_t seq = 0;
        uint32_t chk = 0;
        memcpy(&magic, hdr, 4);
        memcpy(&ver, hdr + 4, 2);
        memcpy(&cnt, hdr + 6, 2);
        memcpy(&seq, hdr + 8, 4);
        memcpy(&chk, hdr + 12, 4);
        const uint8_t nl = hdr[16];
        if (magic != PLAY_FLASH_MAGIC || ver != PLAY_FLASH_VERSION) {
            continue; /* 空槽或非本格式 → 按空槽 */
        }
        if (cnt == 0 || cnt > PLAY_POOL_MAX_EVENTS || nl >= PLAY_POOL_NAME_MAX) {
            continue;
        }
        const size_t body_len = (size_t)nl + (size_t)cnt * 3u;
        uint8_t *body = malloc(body_len ? body_len : 1u);
        if (body == NULL) {
            continue;
        }
        if (esp_partition_read(part, off + PLAY_FLASH_HDR, body, body_len) != ESP_OK) {
            free(body);
            continue;
        }
        if (play_body_checksum(body, nl, (int)cnt) != chk) {
            free(body); /* 校验失败 → 按空槽处理 */
            continue;
        }
        memcpy(s_slots[slot].name, body, nl);
        s_slots[slot].name[nl] = '\0';
        for (int i = 0; i < (int)cnt; ++i) {
            s_slots[slot].ev[i].note = body[nl + (size_t)i * 3u];
            s_slots[slot].ev[i].delta_ms =
                (uint16_t)(body[nl + (size_t)i * 3u + 1u] | ((uint16_t)body[nl + (size_t)i * 3u + 2u] << 8));
        }
        s_slots[slot].count = (int)cnt;
        s_slots[slot].used = true;
        s_slots[slot].seq = seq; /* ★ 保留写序，FIFO 判据跨重启一致 */
        if (seq > max_seq) {
            max_seq = seq;
        }
        free(body);
        loaded++;
    }
    s_seq_top = max_seq; /* 之后 push 的 seq 从这里继续递增 */
    ESP_LOGI(TAG, "flash load: %d slots (pb=%d)", loaded, playpool_used_count());
    return loaded;
}
