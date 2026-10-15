#include "prompt_format.h"
#include "rec_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * 八音琴语音提示的纯逻辑宿主测试（零 ESP-IDF 依赖）。
 * 覆盖：14 句清单 / prompt id 映射、资源头与索引解析（含端序、越界、一致性）、
 *       rec 分区槽位布局（曲库/语音/legacy 不重叠、受保护段）。
 */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void make_header(uint8_t *hdr, const char *magic, uint16_t version, uint16_t count)
{
    memset(hdr, 0, PROMPT_HDR_BYTES);
    memcpy(hdr, magic, 4);
    put_u16(hdr + 4, version);
    put_u16(hdr + 6, count);
}

static void make_entry(uint8_t *e, uint16_t slot, uint32_t offset, uint32_t length)
{
    memset(e, 0, PROMPT_ENTRY_BYTES);
    put_u16(e, slot);
    put_u32(e + 2, offset);
    put_u32(e + 6, length);
}

/* 1) rec 分区槽位布局：曲库(slot0~4)/语音(slot5~20)/legacy(slot21~31) 三段连续不重叠 */
static void test_slot_layout(void)
{
    assert(REC_SLOT_SONGBOOK_BASE == 0);
    assert(REC_SLOT_SONGBOOK_BASE + REC_SLOT_SONGBOOK_COUNT == REC_SLOT_PROMPT_BASE);
    assert(REC_SLOT_PROMPT_BASE + REC_SLOT_PROMPT_COUNT == REC_SLOT_PLAYBACK_BASE);
    assert(REC_SLOT_PLAYBACK_BASE + REC_SLOT_PLAYBACK_COUNT == REC_SLOT_LEGACY_BASE);
    assert(REC_SLOT_PROTECTED_END == REC_SLOT_LEGACY_BASE);
    assert(REC_SLOT_LEGACY_BASE < REC_FLASH_SLOTS);
    assert(REC_SLOT_SIZE == 32768u);
    assert(PROMPT_REGION_BYTES == (REC_SLOT_PROMPT_COUNT * REC_SLOT_SIZE));
    /* 受保护段（曲库+语音）确实在 recorder 可用段之前 */
    assert(REC_SLOT_PROTECTED_END < REC_FLASH_SLOTS);
}

/* 2) 清单：14 句、id 顺序、文案映射、非法 id 返回空串 */
static void test_prompt_list(void)
{
    assert(PROMPT_COUNT == 14);
    assert(strcmp(prompt_text(PROMPT_SELECT_MODE), "请选择模式") == 0);
    assert(strcmp(prompt_text(PROMPT_MODE_TRIAL), "试听模式") == 0);
    assert(strcmp(prompt_text(PROMPT_SONG_TWINKLE), "小星星") == 0);
    assert(strcmp(prompt_text(PROMPT_SONG_TWO_TIGERS), "两只老虎") == 0);
    assert(strcmp(prompt_text(PROMPT_SONG_ODE), "欢乐颂") == 0);
    assert(strcmp(prompt_text(PROMPT_START_RECORD), "开始记谱") == 0);
    assert(strcmp(prompt_text(PROMPT_SAVED), "已保存") == 0);
    assert(strcmp(prompt_text(PROMPT_PLAY_END), "播放结束") == 0);
    assert(strcmp(prompt_text(PROMPT_NO_RECORD), "暂无记录") == 0);
    assert(strcmp(prompt_text(PROMPT_NO_FUNCTION), "本键无功能") == 0);
    assert(strcmp(prompt_text(PROMPT_BACK_HOME), "回到主目录") == 0);
    for (int i = 0; i < PROMPT_COUNT; ++i) {
        assert(prompt_text(i) != NULL && prompt_text(i)[0] != '\0');
    }
    assert(strcmp(prompt_text(PROMPT_COUNT), "") == 0);
    assert(strcmp(prompt_text(-1), "") == 0);
}

/* 3) 资源头解析：ASCII 'VPMT' + version + count */
static void test_parse_header(void)
{
    uint8_t hdr[PROMPT_HDR_BYTES];
    int count = -1;

    make_header(hdr, "VPMT", 1, 14);
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == 1);
    assert(count == 14);

    make_header(hdr, "VPMT", 2, 3);
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == 2);
    assert(count == 3);

    /* 错误 magic（含端序坑：写成 "TPMV" 必须被拒） */
    make_header(hdr, "TPMV", 1, 14);
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == -1);

    /* count 越界 */
    make_header(hdr, "VPMT", 1, 0);
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == -1);
    make_header(hdr, "VPMT", 1, (uint16_t)(PROMPT_COUNT + 1));
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == -1);

    /* version=0 */
    make_header(hdr, "VPMT", 0, 14);
    assert(prompt_parse_header(hdr, sizeof(hdr), &count) == -1);

    /* 长度不足 / 空指针 */
    assert(prompt_parse_header(hdr, 8, &count) == -1);
    assert(prompt_parse_header(NULL, PROMPT_HDR_BYTES, &count) == -1);
}

/* 4) 索引解析：slot/offset 一致性、容量、偶数样本 */
static void test_parse_entry(void)
{
    uint8_t e[PROMPT_ENTRY_BYTES];
    prompt_entry_t out;

    /* 合法：slot5 offset0 len100 */
    make_entry(e, REC_SLOT_PROMPT_BASE, 0, 100);
    assert(prompt_parse_entry(e, sizeof(e), &out) == 0);
    assert(out.slot == REC_SLOT_PROMPT_BASE && out.offset == 0 && out.length == 100);

    /* 合法：跨槽（offset=32768 → slot6） */
    make_entry(e, REC_SLOT_PROMPT_BASE + 1, REC_SLOT_SIZE, 200);
    assert(prompt_parse_entry(e, sizeof(e), &out) == 0);
    assert(out.slot == REC_SLOT_PROMPT_BASE + 1);

    /* slot 与 offset 不一致 */
    make_entry(e, REC_SLOT_PROMPT_BASE, REC_SLOT_SIZE, 200);
    assert(prompt_parse_entry(e, sizeof(e), &out) == -1);

    /* 空句 / 非整样本 */
    make_entry(e, REC_SLOT_PROMPT_BASE, 0, 0);
    assert(prompt_parse_entry(e, sizeof(e), &out) == -1);
    make_entry(e, REC_SLOT_PROMPT_BASE, 0, 101);
    assert(prompt_parse_entry(e, sizeof(e), &out) == -1);

    /* 越界：offset + length 超出资源区 */
    const uint32_t near_end = PROMPT_REGION_BYTES - 50;
    make_entry(e, (uint16_t)(REC_SLOT_PROMPT_BASE + near_end / REC_SLOT_SIZE), near_end, 100);
    assert(prompt_parse_entry(e, sizeof(e), &out) == -1);

    /* 长度不足 / 空指针 */
    assert(prompt_parse_entry(e, 4, &out) == -1);
    assert(prompt_parse_entry(NULL, PROMPT_ENTRY_BYTES, &out) == -1);
    assert(prompt_parse_entry(e, sizeof(e), NULL) == -1);
}

int main(void)
{
    test_slot_layout();
    test_prompt_list();
    test_parse_header();
    test_parse_entry();

    printf("prompt_tests: all passed (list/parse/layout)\n");
    return 0;
}
