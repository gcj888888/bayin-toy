/* 阶段② 录音 / 回放 —— 宿主测试（零 ESP-IDF 依赖）。
 * 覆盖：空录保存失败、录→存→读往返一致、槽位独立、回放 tick 按点发声、
 * 删除后未用、slot 越界、发电脑 JSON，以及**受保护槽位钳制**（曲库/语音区）。
 * flash 读写经 voice_store_stubs.c 的内存分区模拟。 */
#include "recorder.h"
#include "esp_partition.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 来自桩的 peek / 发送记录 */
extern bool rec_part_used(uint8_t slot);
extern char g_last_line[512];

/* recorder 旧 flash 槽只能用 legacy 段（>= REC_SLOT_LEGACY_BASE） */
#define SLOT_A REC_SLOT_LEGACY_BASE      /* 21 */
#define SLOT_B (REC_SLOT_LEGACY_BASE + 1) /* 22 */

static int s_played;

static void test_play_cb(uint8_t note)
{
    (void)note;
    s_played++;
}

int main(void)
{
    recorder_init();

    /* 0) 受保护段（曲库 slot0~4 + 语音 slot5~20）一律不得被 recorder 写/读 */
    for (uint8_t s = 0; s < REC_SLOT_PROTECTED_END; ++s) {
        assert(recorder_save_slot(s) == ESP_ERR_INVALID_ARG);
        assert(recorder_delete_slot(s) == ESP_ERR_INVALID_ARG);
        assert(recorder_load_slot(s) == ESP_ERR_INVALID_ARG);
    }
    assert(REC_SLOT_PROTECTED_END == REC_SLOT_LEGACY_BASE); /* 边界：>=26 才可用 */

    /* 1) 空录保存应失败（没录到东西） */
    recorder_start();
    recorder_stop();
    assert(recorder_save_slot(SLOT_A) == ESP_ERR_INVALID_STATE);

    /* 2) 录 3 个音并保存到 SLOT_A（★ 时间戳用【微秒】，存储必须是【毫秒】） */
    recorder_start();
    recorder_add_note(0, 1000000);  /* t=1.000s：首音 delta=0 */
    recorder_add_note(2, 1300000);  /* t=1.300s：delta=300ms */
    recorder_add_note(4, 2000000);  /* t=2.000s：delta=700ms */
    recorder_stop();

    /* ★★ 单位 bug 闸门：微秒输入必须 /1000 存成毫秒。
     * 若去掉 /1000，则会存 400000/700000 并被钳到 65535 → 本断言立即变红。 */
    assert(recorder_event_count() == 3);
    const recorder_event_t *rev = recorder_events();
    assert(rev[0].delta_ms == 0);
    assert(rev[1].delta_ms == 300); /* 而不是 300000 或 65535 */
    assert(rev[2].delta_ms == 700);

    assert(recorder_has_recording());
    assert(recorder_save_slot(SLOT_A) == ESP_OK);
    assert(recorder_slot_used(SLOT_A));
    assert(rec_part_used(SLOT_A));     /* 真落进 flash 分区 */

    /* 3) 回放 tick 按 delta【毫秒】依序触发 3 次发声 */
    recorder_set_play_note_cb(test_play_cb);
    assert(recorder_load_slot(SLOT_A) == ESP_OK);
    s_played = 0;
    recorder_playback_start(0);
    recorder_playback_tick(0);        /* note 0 (delta 0) */
    assert(s_played == 1);
    recorder_playback_tick(299999);   /* 距 300ms 差 1µs → 仍只 1 声 */
    assert(s_played == 1);
    recorder_playback_tick(300000);   /* note 2 (delta 300ms) */
    assert(s_played == 2);
    recorder_playback_tick(999999);   /* 距 1000ms 差 1µs → 仍只 2 声 */
    assert(s_played == 2);
    recorder_playback_tick(1000000);  /* note 4 (delta 700ms) */
    assert(s_played == 3);

    /* 4) 槽位独立：SLOT_B 存不同内容，SLOT_A 不受影响 */
    recorder_start();
    recorder_add_note(6, 5000);
    recorder_stop();
    assert(recorder_save_slot(SLOT_B) == ESP_OK);
    assert(recorder_slot_used(SLOT_B));

    char buf0[400], buf1[400];
    assert(recorder_load_slot(SLOT_A) == ESP_OK);
    recorder_export_json(buf0, sizeof(buf0));
    assert(recorder_load_slot(SLOT_B) == ESP_OK);
    recorder_export_json(buf1, sizeof(buf1));
    assert(strcmp(buf0, buf1) != 0);  /* 两个槽内容不同 */
    assert(recorder_slot_count() == 2);

    /* 5) 删除 SLOT_A：变未用、load 失败、count 减一 */
    assert(recorder_delete_slot(SLOT_A) == ESP_OK);
    assert(!recorder_slot_used(SLOT_A));
    assert(recorder_load_slot(SLOT_A) == ESP_ERR_NOT_FOUND);
    assert(recorder_slot_count() == 1);

    /* 6) 越界槽位应被拒绝 */
    assert(recorder_save_slot(32) == ESP_ERR_INVALID_ARG);
    assert(recorder_load_slot(32) == ESP_ERR_INVALID_ARG);
    assert(recorder_delete_slot(32) == ESP_ERR_INVALID_ARG);

    /* 7) 发电脑：recorder_send_to_host 经 host_link_send_line 输出 rec JSON */
    recorder_start();
    recorder_add_note(1, 0);
    recorder_stop();
    g_last_line[0] = '\0';
    recorder_send_to_host();
    assert(strstr(g_last_line, "\"t\":\"rec\"") != NULL);

    printf("recorder_tests: ALL PASS (roundtrip/slots/playback/protected-guard)\n");
    return 0;
}
