#include "song_pool.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "rec_layout.h" /* rec 分区槽位分配（复用其 flash 区常量）*/

/*
 * 逻辑部分不依赖 ESP-IDF，可直接在电脑上编译测试；
 * flash 持久化复用 recorder 的 "rec" 分区前 5 槽（不新建分区、不改分区表、不擦 NVS）。
 */

static const char *TAG = "songpool";

typedef struct {
    bool used;
    int count;
    uint32_t seq; /* 写入顺序：越大越新，FIFO 覆盖最旧的 */
    char name[SONGP_NAME_MAX];
    song_ev_t ev[SONGP_MAX_EVENTS];
} song_slot_t;

static song_slot_t s_slots[SONGP_SLOTS];
static uint32_t s_seq_top;

void songpool_init(void)
{
    songpool_clear_all();
}

void songpool_clear_all(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_seq_top = 0;
}

static void copy_events(song_slot_t *dst, const song_ev_t *ev, int count)
{
    if (count > SONGP_MAX_EVENTS) {
        count = SONGP_MAX_EVENTS;
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
    if (n >= SONGP_NAME_MAX) {
        n = SONGP_NAME_MAX - 1;
    }
    memcpy(dst, name, n);
    dst[n] = '\0';
}

int songpool_set(int slot, const char *name, const song_ev_t *ev, int count)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return -1;
    }
    if (count <= 0 || ev == NULL) {
        return -1;
    }
    copy_events(&s_slots[slot], ev, count);
    copy_name(s_slots[slot].name, name);
    s_slots[slot].used = true;
    s_slots[slot].seq = ++s_seq_top;
    return slot;
}

int songpool_del(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return -1;
    }
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    return slot;
}

int songpool_next_slot(void)
{
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        if (!s_slots[i].used) {
            return i;
        }
    }
    int target = 0;
    for (int i = 1; i < SONGP_SLOTS; ++i) {
        if (s_slots[i].seq < s_slots[target].seq) {
            target = i;
        }
    }
    return target;
}

int songpool_push(const char *name, const song_ev_t *ev, int count)
{
    if (count <= 0 || ev == NULL) {
        return -1;
    }
    /* 优先用空槽（从前往后）；全满则覆盖 seq 最小的（最旧）。 */
    const int target = songpool_next_slot();
    copy_events(&s_slots[target], ev, count);
    copy_name(s_slots[target].name, name);
    s_slots[target].used = true;
    s_slots[target].seq = ++s_seq_top;
    return target;
}

int songpool_used_count(void)
{
    int n = 0;
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        if (s_slots[i].used) {
            n++;
        }
    }
    return n;
}

bool songpool_slot_used(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return false;
    }
    return s_slots[slot].used;
}

const char *songpool_name(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS || !s_slots[slot].used) {
        return "";
    }
    return s_slots[slot].name;
}

const song_ev_t *songpool_events(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS || !s_slots[slot].used) {
        return NULL;
    }
    return s_slots[slot].ev;
}

int songpool_event_count(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS || !s_slots[slot].used) {
        return 0;
    }
    return s_slots[slot].count;
}

/* ==== flash 持久化：复用 recorder 的 "rec" 分区前 5 槽 ==== */

#define SONG_FLASH_MAGIC 0x53475030u /* 'SGP0' */
#define SONG_FLASH_HDR 16u           /* magic(4) + count(2) + name_len(1) + 保留(9) */

static const esp_partition_t *song_flash_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                    REC_FLASH_PARTITION);
}

static uint32_t song_flash_offset(int slot)
{
    return (uint32_t)slot * REC_SLOT_SIZE;
}

/* 读 flash 槽头：有效则输出 count 与 name_len */
static bool song_flash_read_header(const esp_partition_t *part, int slot, uint16_t *count_out,
                                   uint8_t *name_len_out)
{
    uint8_t hdr[SONG_FLASH_HDR];
    if (esp_partition_read(part, song_flash_offset(slot), hdr, SONG_FLASH_HDR) != ESP_OK) {
        return false;
    }
    uint32_t magic = 0;
    uint16_t cnt = 0;
    memcpy(&magic, hdr, 4);
    memcpy(&cnt, hdr + 4, 2);
    if (magic != SONG_FLASH_MAGIC) {
        return false; /* 空槽或非本格式 */
    }
    if (cnt > SONGP_MAX_EVENTS || hdr[6] >= SONGP_NAME_MAX) {
        return false;
    }
    *count_out = cnt;
    *name_len_out = hdr[6];
    return true;
}

esp_err_t songpool_flash_save(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_slots[slot].used || s_slots[slot].count <= 0) {
        return ESP_ERR_INVALID_STATE; /* RAM 槽没内容，没得存 */
    }
    const esp_partition_t *part = song_flash_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = song_flash_offset(slot);
    if (off + REC_SLOT_SIZE > part->size) {
        return ESP_ERR_INVALID_ARG;
    }

    const int count = s_slots[slot].count;
    size_t name_len = strlen(s_slots[slot].name);
    if (name_len > SONGP_NAME_MAX - 1u) {
        name_len = SONGP_NAME_MAX - 1u;
    }

    uint8_t hdr[SONG_FLASH_HDR];
    memset(hdr, 0, sizeof(hdr));
    uint32_t magic = SONG_FLASH_MAGIC;
    uint16_t cnt = (uint16_t)count;
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &cnt, 2);
    hdr[6] = (uint8_t)name_len;

    const size_t body_len = name_len + (size_t)count * 3u; /* 每音：note(1) + delta(2) */
    uint8_t *body = malloc(body_len ? body_len : 1u);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, s_slots[slot].name, name_len);
    for (int i = 0; i < count; ++i) {
        body[name_len + (size_t)i * 3u] = s_slots[slot].ev[i].note;
        memcpy(body + name_len + (size_t)i * 3u + 1u, &s_slots[slot].ev[i].delta_ms, 2);
    }

    /* 只擦写这一槽（32KB），不碰其它槽、不碰分区表/NVS */
    esp_err_t e = esp_partition_erase_range(part, off, REC_SLOT_SIZE);
    if (e == ESP_OK) {
        e = esp_partition_write(part, off, hdr, SONG_FLASH_HDR);
    }
    if (e == ESP_OK) {
        e = esp_partition_write(part, off + SONG_FLASH_HDR, body, body_len);
    }
    free(body);
    ESP_LOGI(TAG, "flash save slot %d '%s' (%d) -> %s", slot, s_slots[slot].name, count,
             esp_err_to_name(e));
    return e;
}

esp_err_t songpool_flash_clear(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = song_flash_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t off = song_flash_offset(slot);
    if (off + REC_SLOT_SIZE > part->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_erase_range(part, off, REC_SLOT_SIZE);
}

bool songpool_flash_used(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return false;
    }
    const esp_partition_t *part = song_flash_partition();
    if (part == NULL) {
        return false;
    }
    uint16_t cnt = 0;
    uint8_t nl = 0;
    if (!song_flash_read_header(part, slot, &cnt, &nl)) {
        return false;
    }
    return cnt > 0;
}

int songpool_flash_meta(int slot, char *name_out, int name_cap)
{
    if (slot < 0 || slot >= SONGP_SLOTS) {
        return 0;
    }
    const esp_partition_t *part = song_flash_partition();
    if (part == NULL) {
        return 0;
    }
    uint16_t cnt = 0;
    uint8_t nl = 0;
    if (!song_flash_read_header(part, slot, &cnt, &nl) || cnt == 0) {
        return 0;
    }
    if (name_out != NULL && name_cap > 0) {
        int copy = (int)nl;
        if (copy > name_cap - 1) {
            copy = name_cap - 1;
        }
        if (copy > 0 &&
            esp_partition_read(part, song_flash_offset(slot) + SONG_FLASH_HDR, name_out,
                               (size_t)copy) != ESP_OK) {
            return 0;
        }
        name_out[copy] = '\0';
    }
    return (int)cnt;
}

int songpool_load_from_flash(void)
{
    const esp_partition_t *part = song_flash_partition();
    if (part == NULL) {
        return 0;
    }
    int loaded = 0;
    for (int slot = 0; slot < SONGP_SLOTS; ++slot) {
        uint16_t cnt = 0;
        uint8_t nl = 0;
        if (!song_flash_read_header(part, slot, &cnt, &nl) || cnt == 0) {
            continue;
        }
        const uint32_t base = song_flash_offset(slot) + SONG_FLASH_HDR;
        if (esp_partition_read(part, base, s_slots[slot].name, nl) != ESP_OK) {
            continue;
        }
        s_slots[slot].name[nl] = '\0';
        bool ok = true;
        for (int i = 0; i < (int)cnt; ++i) {
            uint8_t rec[3];
            if (esp_partition_read(part, base + nl + (uint32_t)i * 3u, rec, 3) != ESP_OK) {
                ok = false;
                break;
            }
            s_slots[slot].ev[i].note = rec[0];
            s_slots[slot].ev[i].delta_ms = (uint16_t)(rec[1] | ((uint16_t)rec[2] << 8));
        }
        if (!ok) {
            continue;
        }
        s_slots[slot].count = (int)cnt;
        s_slots[slot].used = true;
        s_slots[slot].seq = ++s_seq_top;
        loaded++;
    }
    ESP_LOGI(TAG, "flash load: %d slots", loaded);
    return loaded;
}

