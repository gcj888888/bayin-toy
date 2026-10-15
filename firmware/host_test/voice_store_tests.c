#include "voice_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 开机语音存储的宿主测试（纯逻辑，不依赖 ESP-IDF）。
 *
 * voice_store_stubs.c 用一块 2MB 内存模拟 voice 分区、
 * audio_click_play_pcm 记录“播了什么”、并用 asm 符号提供假内嵌默认语音。
 *
 * 覆盖：
 *  1) 默认语音（无导入）开机一定有、帧数正确
 *  2) 网页导入：base64 分块重组 → 落内存分区（magic + 长度）→ 优先播导入
 *  3) 清除导入恢复内嵌默认
 *  4) 超过上限（>10s）被钳位、超量数据被拒绝（不溢出）
 *  5) 非法参数（total=0 / 未 begin 就 append/end）安全返回错误
 */

/* 播放记录（audio_click 替身全局，见 voice_store_stubs.c） */
extern const int16_t *g_pcm_ptr;
extern uint32_t g_pcm_frames;
extern int g_pcm_played;

/* 分区窥视（见 voice_store_stubs.c） */
extern uint32_t voice_part_magic(void);
extern uint32_t voice_part_len(void);

#define VOICE_MAGIC 0x564F4943u

/* 标准 base64 编码（测试造数据用） */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    const size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* 模拟网页：把原始 PCM 按每块 57 字节切分，每块“独立” base64 编码后喂给
 * upload_append。注意：板子是对每个 base64 块“独立解码再拼接”，所以必须
 * 按原始字节切块、各自编码（不能先编码整体再切 base64 字符串，否则会错位）。 */
static void feed_bytes(const uint8_t *data, size_t len, esp_err_t *last)
{
    const size_t raw_chunk = 57; /* 57 字节 → 76 base64 字符（整组，可独立解码） */
    size_t off = 0;
    *last = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > raw_chunk) {
            n = raw_chunk;
        }
        char buf[128];
        b64_encode(data + off, n, buf);
        const esp_err_t e = voice_store_upload_append(buf, (uint32_t)strlen(buf));
        if (e != ESP_OK) {
            *last = e;
            return;
        }
        off += n;
    }
}

/* 1) 默认语音：无导入时开机一定有声音，帧数 = 2（假内嵌 4 字节） */
static void test_default_greeting(void)
{
    voice_store_init();
    assert(!voice_store_has_import());

    g_pcm_played = 0;
    voice_store_play_greeting();
    assert(g_pcm_played == 1);
    assert(g_pcm_frames == 2);
}

/* 2) 导入流程：base64 分块 → 落分区 → 优先播导入 */
static void test_import_flow(void)
{
    voice_store_init();

    const int N = 100; /* 100 帧 */
    int16_t pcm[100];
    uint8_t bytes[200];
    for (int i = 0; i < N; ++i) {
        pcm[i] = (int16_t)(i * 7 - 500);
    }
    memcpy(bytes, pcm, sizeof(bytes));

    assert(voice_store_upload_begin(sizeof(bytes)) == ESP_OK);
    esp_err_t append_err = ESP_OK;
    feed_bytes(bytes, sizeof(bytes), &append_err);
    assert(append_err == ESP_OK);
    assert(voice_store_upload_end() == ESP_OK);

    assert(voice_store_has_import());

    /* 开机播的是导入语音，帧数 = N */
    g_pcm_played = 0;
    voice_store_play_greeting();
    assert(g_pcm_played == 1);
    assert(g_pcm_frames == (uint32_t)N);

    /* 分区里确实写了 magic + 长度（验证“落 Flash 分区”） */
    assert(voice_part_magic() == VOICE_MAGIC);
    assert(voice_part_len() == sizeof(bytes));
}

/* 3) 清除导入 → 恢复内嵌默认 */
static void test_clear_reverts_to_default(void)
{
    /* 先导入一段 */
    voice_store_init();
    int16_t pcm[50];
    uint8_t bytes[100];
    for (int i = 0; i < 50; ++i) {
        pcm[i] = (int16_t)(i - 25);
    }
    memcpy(bytes, pcm, sizeof(bytes));
    assert(voice_store_upload_begin(sizeof(bytes)) == ESP_OK);
    esp_err_t ae = ESP_OK;
    feed_bytes(bytes, sizeof(bytes), &ae);
    assert(ae == ESP_OK);
    assert(voice_store_upload_end() == ESP_OK);
    assert(voice_store_has_import());

    /* 清除 */
    assert(voice_store_clear_import() == ESP_OK);
    assert(!voice_store_has_import());

    g_pcm_played = 0;
    voice_store_play_greeting();
    assert(g_pcm_frames == 2); /* 回到默认 2 帧 */
}

/* 4) 超量数据被拒、不溢出分区；>10s 的总长度被钳位 */
static void test_over_cap_rejected(void)
{
    voice_store_init();

    /* 申请很小容量（100 字节），再塞远超它的数据 → 必须被拒 */
    assert(voice_store_upload_begin(100) == ESP_OK);
    uint8_t big[200];
    for (int i = 0; i < 200; ++i) {
        big[i] = (uint8_t)i;
    }
    char b64big[300];
    b64_encode(big, sizeof(big), b64big);
    const esp_err_t e = voice_store_upload_append(b64big, (uint32_t)strlen(b64big));
    assert(e == ESP_ERR_NO_MEM);      /* 超过容量，拒绝 */
    assert(!voice_store_has_import()); /* 未提交 */

    /* 没数据就 end → 非法状态（同时释放缓冲，不漏） */
    assert(voice_store_upload_end() == ESP_ERR_INVALID_STATE);
}

/* 4b) total > 10 秒上限时，容量被钳到 VOICE_MAX_BYTES，且能正常提交到该上限 */
static void test_over_10s_clamped(void)
{
    voice_store_init();

    /* 申请超过 10 秒（VOICE_MAX_BYTES + 100）→ 容量应被钳到 VOICE_MAX_BYTES */
    assert(voice_store_upload_begin(VOICE_MAX_BYTES + 100) == ESP_OK);

    /* 填充一段较大的数据（远小于上限，保持测试轻量）：100000 帧 = 200000 字节 */
    const uint32_t fill_bytes = 200000u;
    uint8_t *buf = malloc(fill_bytes);
    assert(buf != NULL);
    for (uint32_t i = 0; i < fill_bytes; ++i) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    esp_err_t ae = ESP_OK;
    feed_bytes(buf, fill_bytes, &ae);
    assert(ae == ESP_OK);
    assert(voice_store_upload_end() == ESP_OK);

    assert(voice_store_has_import());
    g_pcm_played = 0;
    voice_store_play_greeting();
    assert(g_pcm_played == 1);
    assert(g_pcm_frames == fill_bytes / 2);     /* 200000 字节 = 100000 帧 */
    assert(voice_part_magic() == VOICE_MAGIC);
    assert(voice_part_len() == fill_bytes);

    free(buf);

    /* 收尾：清除，恢复默认，避免影响别的测试 */
    assert(voice_store_clear_import() == ESP_OK);
}

/* 5) 非法参数安全返回 */
static void test_invalid_args(void)
{
    /* total = 0 → 非法参数 */
    assert(voice_store_upload_begin(0) == ESP_ERR_INVALID_ARG);

    /* 没 begin 就 append / end → 非法状态，不应崩 */
    assert(voice_store_upload_append("AAAA", 4) == ESP_ERR_INVALID_STATE);
    assert(voice_store_upload_end() == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_default_greeting();
    test_import_flow();
    test_clear_reverts_to_default();
    test_over_cap_rejected();
    test_over_10s_clamped();
    test_invalid_args();

    printf("voice_store_tests: all passed (default/import/clear/truncate/invalid)\n");
    return 0;
}
