#include "prompt.h"

#include "audio_click.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "rec_layout.h"

static const char *TAG = "prompt";

static const esp_partition_t *s_part;
static bool s_ready;
static int s_count;
static prompt_entry_t s_entries[PROMPT_COUNT];
static uint32_t s_region_base; /* 资源区起始绝对偏移 = slot5 × 32KB */
static int16_t *s_buf;         /* 2× 上采样缓冲（SPIRAM），容量 = 2×最长句字节数 */
static uint32_t s_buf_frames;

void prompt_init(void)
{
    s_part = NULL;
    s_ready = false;
    s_count = 0;
    s_buf = NULL;
    s_buf_frames = 0;

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      REC_FLASH_PARTITION);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no %s partition; voice prompts disabled", REC_FLASH_PARTITION);
        return;
    }
    s_region_base = REC_SLOT_PROMPT_BASE * REC_SLOT_SIZE;
    if ((uint64_t)s_region_base + PROMPT_HDR_BYTES > s_part->size) {
        return;
    }

    uint8_t hdr[PROMPT_HDR_BYTES];
    if (esp_partition_read(s_part, s_region_base, hdr, PROMPT_HDR_BYTES) != ESP_OK) {
        return;
    }
    int count = 0;
    if (prompt_parse_header(hdr, sizeof(hdr), &count) < 0) {
        ESP_LOGI(TAG, "no VPMT resource in rec slot%d (fresh board?) -> prompts disabled",
                 (int)REC_SLOT_PROMPT_BASE);
        return;
    }

    uint32_t max_len = 0;
    for (int i = 0; i < count; ++i) {
        uint8_t ent[PROMPT_ENTRY_BYTES];
        const uint32_t off = s_region_base + PROMPT_HDR_BYTES + (uint32_t)i * PROMPT_ENTRY_BYTES;
        if (esp_partition_read(s_part, off, ent, PROMPT_ENTRY_BYTES) != ESP_OK) {
            return;
        }
        if (prompt_parse_entry(ent, sizeof(ent), &s_entries[i]) != 0) {
            ESP_LOGW(TAG, "bad prompt index entry %d", i);
            return;
        }
        if (s_entries[i].length > max_len) {
            max_len = s_entries[i].length;
        }
    }
    if (max_len == 0) {
        return;
    }

    /* 上采样缓冲：2× 最长句（16kHz→32kHz），放 SPIRAM 不占内部 DRAM */
    const uint32_t bytes = max_len * 2u;
    s_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGW(TAG, "no SPIRAM for prompt buffer (%u B); prompts disabled", (unsigned)bytes);
        return;
    }
    s_buf_frames = bytes / 2u;
    s_count = count;
    s_ready = true;
    ESP_LOGI(TAG, "VPMT ready: %d prompts, longest %u B, buffer %u B", count, (unsigned)max_len,
             (unsigned)bytes);
}

bool prompt_ready(void)
{
    return s_ready;
}

int prompt_count(void)
{
    return s_ready ? s_count : 0;
}

int prompt_duration_frames(int id)
{
    if (!s_ready || id < 0 || id >= s_count) {
        return 0;
    }
    return (int)s_entries[id].length; /* 16k 帧 ×2（上采样）= length 字节 */
}

int prompt_play(int id)
{
    if (!s_ready || id < 0 || id >= s_count) {
        return 0;
    }
    const prompt_entry_t *e = &s_entries[id];
    if (e->length == 0 || (e->length & 1u) != 0) {
        return 0;
    }
    const uint32_t n = e->length / 2u; /* 16kHz 帧数 */
    if (n == 0 || n * 2u > s_buf_frames) {
        return 0;
    }

    /* 让位：新提示替换旧 PCM 声道（音频任务先处理 stop_pcm 再处理新请求） */
    (void)audio_click_stop_pcm();
    int16_t *b = s_buf;
    if (esp_partition_read(s_part, s_region_base + e->offset, b, e->length) != ESP_OK) {
        return 0;
    }
    /* 2× 上采样：每样点重复一次（原地从后往前展开，不会覆盖未读源） */
    for (int i = (int)n - 1; i >= 0; --i) {
        b[i * 2] = b[i];
        b[i * 2 + 1] = b[i];
    }
    if (audio_click_play_pcm(b, n * 2u) != ESP_OK) {
        return 0;
    }
    /* 证据打点：一次 prompt_play 应对应一次 PCM 入队（见 audio_click 的 "PCM enqueue"） */
    ESP_LOGI(TAG, "prompt_play id=%d '%s' -> %u frames", id, prompt_text(id), (unsigned)(n * 2u));
    return (int)(n * 2u);
}
