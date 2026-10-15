#include "song_pool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * 八音琴 5 槽曲库 / 回放池的宿主测试（纯逻辑，零 ESP-IDF 依赖）。
 * 覆盖：song_add 写入 / song_del 清空 / 自弹+FIFO 追加满 5 覆盖最旧 /
 *       song_next_slot 预览 / 事件读回 / 非法参数。
 */

static song_ev_t ev[8];

static void make_ev(uint8_t base, int n)
{
    for (int i = 0; i < n; ++i) {
        ev[i].note = (uint8_t)((base + i) % 8);
        ev[i].delta_ms = (uint16_t)((i == 0) ? 0 : 100 * (i + 1));
    }
}

int main(void)
{
    songpool_init();
    assert(songpool_used_count() == 0);
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        assert(!songpool_slot_used(i));
        assert(strcmp(songpool_name(i), "") == 0);
        assert(songpool_event_count(i) == 0);
        assert(songpool_events(i) == NULL);
    }

    /* 1) song_add：写入指定槽，名字/事件读回一致 */
    make_ev(0, 4);
    assert(songpool_set(2, "小星星", ev, 4) == 2);
    assert(songpool_slot_used(2));
    assert(songpool_used_count() == 1);
    assert(strcmp(songpool_name(2), "小星星") == 0);
    assert(songpool_event_count(2) == 4);
    const song_ev_t *r = songpool_events(2);
    assert(r != NULL && r[0].note == 0 && r[3].note == 3);
    assert(r[3].delta_ms == 400);

    /* 2) song_add 覆盖同一槽 */
    make_ev(5, 2);
    assert(songpool_set(2, "欢乐颂", ev, 2) == 2);
    assert(strcmp(songpool_name(2), "欢乐颂") == 0);
    assert(songpool_event_count(2) == 2);

    /* 3) song_del：清空 */
    assert(songpool_del(2) == 2);
    assert(!songpool_slot_used(2));
    assert(songpool_used_count() == 0);

    /* 4) 非法槽位/空数据 */
    assert(songpool_set(-1, "x", ev, 2) == -1);
    assert(songpool_set(SONGP_SLOTS, "x", ev, 2) == -1);
    assert(songpool_set(0, "x", ev, 0) == -1);
    assert(songpool_del(-1) == -1);
    assert(songpool_del(SONGP_SLOTS) == -1);
    assert(songpool_push("x", ev, 0) == -1);

    /* 5) next_slot 预览与实际 push 落点一致（先填空槽 0..4） */
    songpool_init();
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        const int preview = songpool_next_slot();
        assert(preview == i);
        make_ev((uint8_t)i, 2);
        char nm[8];
        snprintf(nm, sizeof(nm), "r%d", i);
        assert(songpool_push(nm, ev, 2) == i);
    }
    assert(songpool_used_count() == SONGP_SLOTS);

    /* 6) 满 5 后 FIFO 覆盖最旧：push#6 落回槽 0（seq 最旧） */
    assert(songpool_next_slot() == 0);
    make_ev(7, 3);
    assert(songpool_push("r5", ev, 3) == 0);
    assert(songpool_used_count() == SONGP_SLOTS);
    assert(strcmp(songpool_name(0), "r5") == 0);
    assert(songpool_event_count(0) == 3);
    /* 其余槽保持原内容 */
    assert(strcmp(songpool_name(1), "r1") == 0);
    assert(strcmp(songpool_name(4), "r4") == 0);

    /* 7) 再 push#7：此时最旧是槽 1 */
    assert(songpool_next_slot() == 1);
    make_ev(1, 2);
    assert(songpool_push("r6", ev, 2) == 1);
    assert(strcmp(songpool_name(1), "r6") == 0);
    assert(strcmp(songpool_name(0), "r5") == 0);

    /* 8) set 会刷新 seq：把槽 4 重新写入后它不是最旧 */
    make_ev(3, 2);
    assert(songpool_set(4, "web", ev, 2) == 4);
    assert(songpool_next_slot() == 2); /* 现在最旧是槽 2 */

    /* 9) flash 持久化：song_add = 写 RAM + 写 flash；清 RAM 后 load 仍能恢复 */
    songpool_clear_all();
    make_ev(0, 3);
    assert(songpool_set(2, "persist", ev, 3) == 2);
    assert(songpool_flash_save(2) == ESP_OK);
    assert(songpool_flash_used(2));
    char nm[SONGP_NAME_MAX];
    assert(songpool_flash_meta(2, nm, sizeof(nm)) == 3);
    assert(strcmp(nm, "persist") == 0);

    songpool_init(); /* 清 RAM（模拟重启） */
    assert(!songpool_slot_used(2));
    assert(songpool_load_from_flash() == 1);
    assert(songpool_slot_used(2));
    assert(strcmp(songpool_name(2), "persist") == 0);
    assert(songpool_event_count(2) == 3);
    const song_ev_t *pr = songpool_events(2);
    assert(pr != NULL && pr[0].note == 0 && pr[2].note == 2 && pr[2].delta_ms == 300);

    /* 10) song_del = 清 RAM + 清 flash；再 load 为空 */
    assert(songpool_del(2) == 2);
    assert(songpool_flash_clear(2) == ESP_OK);
    assert(!songpool_flash_used(2));
    assert(songpool_flash_meta(2, nm, sizeof(nm)) == 0);
    songpool_init();
    assert(songpool_load_from_flash() == 0);
    assert(!songpool_slot_used(2));

    /* 11) 自弹/游戏 push 只进 RAM，不写 flash */
    songpool_init();
    make_ev(4, 2);
    assert(songpool_push("ramonly", ev, 2) == 0);
    assert(songpool_slot_used(0));
    assert(!songpool_flash_used(0)); /* push 不落 flash */
    songpool_init();
    assert(songpool_load_from_flash() == 0); /* 重启后 RAM 里没有（掉电即失） */

    /* 12) 非法槽位 / 空 RAM 槽的 flash 操作 */
    assert(songpool_flash_save(-1) == ESP_ERR_INVALID_ARG);
    assert(songpool_flash_save(SONGP_SLOTS) == ESP_ERR_INVALID_ARG);
    assert(songpool_flash_save(0) == ESP_ERR_INVALID_STATE); /* RAM 槽空，没得存 */
    assert(songpool_flash_clear(-1) == ESP_ERR_INVALID_ARG);
    assert(!songpool_flash_used(-1));
    assert(!songpool_flash_used(SONGP_SLOTS));

    printf("song_pool_tests: all passed (set/del/push/fifo/next_slot/flash persist/invalid)\n");
    return 0;
}
