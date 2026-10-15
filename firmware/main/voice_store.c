#include "voice_store.h"

#include "audio_click.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "string.h"

static const char *TAG = "voice_store";

/* 内嵌默认语音（编译进固件，断电不丢） */
extern const uint8_t s_voice_default_start[] asm("_binary_voice_default_raw_start");
extern const uint8_t s_voice_default_end[] asm("_binary_voice_default_raw_end");

#define VOICE_PARTITION_LABEL "voice"
#define VOICE_MAGIC 0x564F4943u /* 'VOIC' */
#define VOICE_HEADER_BYTES 8u   /* [magic u32][length u32] */

static const int16_t *s_active_buf;   /* 当前生效的 PCM（导入优先，否则内嵌默认） */
static uint32_t s_active_frames;

/* 导入上传累计缓冲（放在 SPIRAM，避免占内部 DRAM） */
static uint8_t *s_upload_buf;
static uint32_t s_upload_cap;
static uint32_t s_upload_len;

/* ---- base64 解码（标准，含 '=' 填充） ---- */
static int base64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static uint32_t base64_decode(const char *in, uint32_t in_len, uint8_t *out, uint32_t out_cap)
{
    uint32_t out_len = 0;
    int32_t acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < in_len; ++i) {
        const char c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        const int v = base64_val(c);
        if (v < 0) {
            continue; /* 忽略非法字符 */
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len < out_cap) {
                out[out_len++] = (uint8_t)((acc >> bits) & 0xFF);
            }
        }
    }
    return out_len;
}

static const esp_partition_t *voice_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                     VOICE_PARTITION_LABEL);
}

void voice_store_init(void)
{
    /* 先尝试读 voice 分区里的导入语音 */
    const esp_partition_t *part = voice_partition();
    if (part == NULL) {
        ESP_LOGW(TAG, "no voice partition found");
        s_active_buf = (const int16_t *)s_voice_default_start;
        s_active_frames = (uint32_t)((s_voice_default_end - s_voice_default_start) / 2);
        return;
    }
    uint32_t header[2] = {0, 0};
    if (esp_partition_read(part, 0, header, VOICE_HEADER_BYTES) != ESP_OK) {
        goto use_default;
    }
    if (header[0] != VOICE_MAGIC) {
        ESP_LOGI(TAG, "no imported voice (fresh board) -> use embedded default");
        goto use_default;
    }
    const uint32_t bytes = header[1];
    if (bytes == 0 || bytes > VOICE_MAX_BYTES || bytes > (part->size - VOICE_HEADER_BYTES)) {
        ESP_LOGW(TAG, "imported voice size invalid -> use embedded default");
        goto use_default;
    }
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "no mem for imported voice -> use embedded default");
        goto use_default;
    }
    if (esp_partition_read(part, VOICE_HEADER_BYTES, buf, bytes) != ESP_OK) {
        free(buf);
        goto use_default;
    }
    s_active_buf = (const int16_t *)buf;
    s_active_frames = bytes / 2;
    ESP_LOGI(TAG, "loaded imported voice: %u frames (%.1fs)", s_active_frames,
             s_active_frames / 32000.0f);
    return;

use_default:
    s_active_buf = (const int16_t *)s_voice_default_start;
    s_active_frames = (uint32_t)((s_voice_default_end - s_voice_default_start) / 2);
}

void voice_store_play_greeting(void)
{
    if (s_active_buf == NULL || s_active_frames == 0) {
        return;
    }
    /* 复用发声引擎的混音通道，音量/限幅自动生效 */
    (void)audio_click_play_pcm(s_active_buf, s_active_frames);
    ESP_LOGI(TAG, "play greeting (%u frames, %.1fs)", s_active_frames,
             s_active_frames / 32000.0f);
}

esp_err_t voice_store_upload_begin(uint32_t total_bytes)
{
    if (s_upload_buf != NULL) {
        free(s_upload_buf);
        s_upload_buf = NULL;
    }
    s_upload_len = 0;
    s_upload_cap = total_bytes > VOICE_MAX_BYTES ? VOICE_MAX_BYTES : total_bytes;
    if (s_upload_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_upload_buf = heap_caps_malloc(s_upload_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_upload_buf == NULL) {
        s_upload_cap = 0;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice upload begin: %u bytes (cap %u)", total_bytes, s_upload_cap);
    return ESP_OK;
}

esp_err_t voice_store_upload_append(const char *b64, uint32_t b64_len)
{
    if (s_upload_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 精确（且为上界）的 base64 解码后字节数。
     * 不能用 (b64_len*3+3)/4 这种“最坏膨胀”估值：最后一块带 '=' 填充时，
     * 真实解码字节更少，估值会偏大，把本来能放下的最后一块误判成“超限”而拒绝，
     * 导致几乎每个真实语音导入（末块多半是 1~2 字节的部分组）都失败。 */
    const uint32_t groups = b64_len / 4;
    const uint32_t rem = b64_len % 4;
    uint32_t pad = 0;
    if (rem == 0) {
        for (uint32_t i = b64_len; i > 0 && b64[i - 1] == '='; --i) {
            pad++;
        }
    }
    uint32_t need = groups * 3;
    if (rem != 0) {
        need += rem; /* 非整组（无填充）：实际解码字节 <= rem，取上界 */
    } else {
        need -= pad; /* 整组且有填充：去掉 '=' 才是真实字节数 */
    }
    if (s_upload_len + need > s_upload_cap) {
        ESP_LOGW(TAG, "voice upload exceeds cap, truncating");
        return ESP_ERR_NO_MEM;
    }
    const uint32_t got = base64_decode(b64, b64_len, s_upload_buf + s_upload_len,
                                       s_upload_cap - s_upload_len);
    s_upload_len += got;
    return ESP_OK;
}

esp_err_t voice_store_upload_end(void)
{
    if (s_upload_buf == NULL || s_upload_len == 0) {
        if (s_upload_buf) {
            free(s_upload_buf);
            s_upload_buf = NULL;
        }
        s_upload_cap = 0;
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = voice_partition();
    if (part == NULL) {
        free(s_upload_buf);
        s_upload_buf = NULL;
        s_upload_cap = 0;
        return ESP_ERR_NOT_FOUND;
    }
    /* 写 Flash：先擦后写（4 字节对齐） */
    const uint32_t write_bytes = (s_upload_len + 3u) & ~3u;
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice erase failed: %s", esp_err_to_name(err));
        free(s_upload_buf);
        s_upload_buf = NULL;
        s_upload_cap = 0;
        return err;
    }
    uint32_t header[2] = {VOICE_MAGIC, s_upload_len};
    err = esp_partition_write(part, 0, header, VOICE_HEADER_BYTES);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_partition_write(part, VOICE_HEADER_BYTES, s_upload_buf, write_bytes);
    if (err != ESP_OK) {
        goto fail;
    }

    /* 切换生效缓冲：释放旧导入（若有），指向刚收到的 */
    if (s_active_buf != (const int16_t *)s_voice_default_start && s_active_buf != NULL) {
        free((void *)s_active_buf);
    }
    s_active_buf = (const int16_t *)s_upload_buf;
    s_active_frames = s_upload_len / 2;
    s_upload_buf = NULL; /* 已交给 s_active_buf，别再 free */
    s_upload_cap = 0;
    ESP_LOGI(TAG, "voice import saved: %u frames (%.1fs)", s_active_frames,
             s_active_frames / 32000.0f);
    return ESP_OK;

fail:
    free(s_upload_buf);
    s_upload_buf = NULL;
    s_upload_cap = 0;
    return err;
}

bool voice_store_has_import(void)
{
    return s_active_buf != NULL && s_active_buf != (const int16_t *)s_voice_default_start;
}

esp_err_t voice_store_clear_import(void)
{
    const esp_partition_t *part = voice_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 写一段非法 magic，等于“没有导入” */
    uint32_t zero[2] = {0, 0};
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err == ESP_OK) {
        err = esp_partition_write(part, 0, zero, VOICE_HEADER_BYTES);
    }
    if (s_active_buf != (const int16_t *)s_voice_default_start && s_active_buf != NULL) {
        free((void *)s_active_buf);
    }
    s_active_buf = (const int16_t *)s_voice_default_start;
    s_active_frames = (uint32_t)((s_voice_default_end - s_voice_default_start) / 2);
    ESP_LOGI(TAG, "voice import cleared -> use embedded default");
    return err;
}
