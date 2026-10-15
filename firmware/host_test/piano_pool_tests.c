#include "play_pool.h"
#include "song_pool.h"
#include "esp_partition.h"
#include "rec_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * 八音琴：【试听池 = flash 曲库】 vs 【回放池 = RAM FIFO】 双池划分场景测试。
 *
 * 复现并守护 P1 回归：以前试听/回放共用同一 5 槽 pool，导致"回放里先出现内置 3 首"。
 * 用与 main 同构的调用序列（seed 内置曲 → 自弹/游戏 push → 分别读两池）断言：
 *   - 内置 3 首只进试听池，绝不出现在回放池
 *   - 自弹/游戏记谱只进回放池，绝不污染试听池
 *   - 回放能"看到刚录的新曲"；播放（读池）互不影响
 */

static song_ev_t ev[PLAY_POOL_MAX_EVENTS];

static void make_ev(int n, uint8_t base)
{
    for (int i = 0; i < n; ++i) {
        ev[i].note = (uint8_t)((base + i) % 8);
        ev[i].delta_ms = (uint16_t)((i == 0) ? 0 : 400);
    }
}

/* 模拟 main 开机播种 3 首内置曲到试听池（flash 曲库） */
static void seed_builtins(void)
{
    make_ev(42, 0);
    songpool_set(0, "小星星", ev, 42);
    make_ev(32, 1);
    songpool_set(1, "两只老虎", ev, 32);
    make_ev(15, 3);
    songpool_set(2, "欢乐颂", ev, 15);
}

/* 1) 内置 3 首只在试听池；回放池出厂为空 */
static void test_builtins_only_in_audition(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();

    assert(songpool_used_count() == 3);
    assert(strcmp(songpool_name(0), "小星星") == 0);

    /* ★ 回放池必须在出厂时为空，内置曲不得出现 */
    assert(playpool_used_count() == 0);
    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        assert(!playpool_slot_used(i));
        assert(strcmp(playpool_name(i), "") == 0);
        assert(playpool_event_count(i) == 0);
    }
}

/* 2) 自弹"录音→保存" → 回放可见新曲；试听池不受影响 */
static void test_self_record_visible_in_playback(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();

    /* 自弹记谱 12 音 → 结束保存 → push 回放池（main 的 toggle_record 逻辑） */
    make_ev(12, 2);
    const int slot = playpool_push("自弹", ev, 12);
    assert(slot == 0);
    assert(playpool_used_count() == 1);
    assert(playpool_slot_used(0));
    assert(strcmp(playpool_name(0), "自弹") == 0);
    assert(playpool_event_count(0) == 12);
    const song_ev_t *pe = playpool_events(0);
    assert(pe != NULL && pe[0].note == 2 && pe[11].note == (uint8_t)((2 + 11) % 8));

    /* ★ 试听池（flash 曲库）完全不受影响：仍是 3 首，内容不变 */
    assert(songpool_used_count() == 3);
    assert(strcmp(songpool_name(0), "小星星") == 0);
    assert(songpool_event_count(0) == 42);
    assert(!songpool_slot_used(3)); /* 自弹不再写进 flash 曲库槽 */
}

/* 3) 游戏通关记谱（record:1→record:0）→ 进回放池，不污染试听池 */
static void test_game_record_into_playback_not_audition(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();

    /* 先自弹一首 */
    make_ev(10, 0);
    playpool_push("自弹", ev, 10);
    /* 再来一局游戏通关 */
    make_ev(20, 4);
    const int slot = playpool_push("游戏", ev, 20);
    assert(slot == 1);
    assert(playpool_used_count() == 2);
    assert(strcmp(playpool_name(1), "游戏") == 0);
    assert(playpool_event_count(1) == 20);

    /* 试听池仍只有内置 3 首 */
    assert(songpool_used_count() == 3);
    for (int i = 0; i < 3; ++i) {
        assert(songpool_slot_used(i));
    }
    assert(!songpool_slot_used(3));
}

/* 4) 回放池 FIFO：满 5 覆盖最旧；试听池始终不变 */
static void test_playback_fifo_overwrite(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();

    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        make_ev(3, (uint8_t)i);
        char nm[8];
        snprintf(nm, sizeof(nm), "r%d", i);
        assert(playpool_push(nm, ev, 3) == i);
    }
    assert(playpool_used_count() == PLAY_POOL_SLOTS);
    assert(playpool_next_slot() == 0); /* 最旧是槽 0 */
    make_ev(5, 7);
    assert(playpool_push("r5", ev, 5) == 0); /* 覆盖最旧 */
    assert(playpool_used_count() == PLAY_POOL_SLOTS);
    assert(strcmp(playpool_name(0), "r5") == 0);
    assert(playpool_event_count(0) == 5);

    /* 试听池依旧 3 首内置曲 */
    assert(songpool_used_count() == 3);
    assert(strcmp(songpool_name(0), "小星星") == 0);
}

/* 5) song_play（试听池）读的是 flash 曲库：与回放池互不影响 */
static void test_song_play_reads_audition_pool(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();
    make_ev(7, 1);
    playpool_push("自弹", ev, 7); /* 回放池有 1 首 */

    /* song_play 语义 = 试听池：读 songpool */
    assert(songpool_slot_used(0) && songpool_event_count(0) == 42);
    /* 同时回放池独立存在 */
    assert(playpool_slot_used(0) && playpool_event_count(0) == 7);

    /* 网页 song_del 只清试听池，不影响回放池 */
    songpool_del(0);
    assert(!songpool_slot_used(0));
    assert(playpool_slot_used(0)); /* 回放池仍在 */
}

/* 6) 回放池条数（state 的 `pb`）随 push/清空正确变化；试听池不受影响 */
static void test_playback_count_changes(void)
{
    songpool_init();
    playpool_init();
    seed_builtins();

    assert(playpool_used_count() == 0); /* 出厂 pb=0 */
    make_ev(3, 0);
    assert(playpool_push("a", ev, 3) == 0);
    assert(playpool_used_count() == 1);
    make_ev(4, 0);
    assert(playpool_push("b", ev, 4) == 1);
    assert(playpool_used_count() == 2);

    playpool_clear_all();
    assert(playpool_used_count() == 0);
    /* 试听池（flash 曲库）不受回放池影响 */
    assert(songpool_used_count() == 3);
}

/* 7) 回放推进按 delta_ms 依序（含 delta_ms=0 的首音立即触发） */
static void test_playback_advance_order(void)
{
    playpool_init();
    song_ev_t e[3] = {{0, 0}, {2, 300}, {4, 700}}; /* 首音 delta=0 边界 */
    assert(playpool_push("t", e, 3) == 0);

    const song_ev_t *pe = playpool_events(0);
    const int n = playpool_event_count(0);
    assert(pe != NULL && n == 3);

    /* 复刻 main 的 playback_tick：首个 next = delta[0]；每播完 idx 后 next += delta[idx] */
    int64_t next_us = (int64_t)pe[0].delta_ms * 1000;
    int idx = 0;
    uint8_t order[3];
    int64_t fire_us[3];
    for (int64_t t = 0; t <= 3000000 && idx < n; t += 1000) {
        while (idx < n && t >= next_us) {
            order[idx] = pe[idx].note;
            fire_us[idx] = t;
            idx++;
            if (idx < n) {
                next_us += (int64_t)pe[idx].delta_ms * 1000;
            }
        }
    }
    assert(idx == 3);
    /* 音序正确 */
    assert(order[0] == 0 && order[1] == 2 && order[2] == 4);
    /* 时序正确：delta=0 → 立即；300ms；+700ms */
    assert(fire_us[0] == 0);
    assert(fire_us[1] == 300000);
    assert(fire_us[2] == 1000000);
}

/* 8) 台架自测链：rec_inject（注入）→ 池计数 → pb_play（按 delta 依序）→ snd 与 tick 一致。
 *    snd 与 tick 都在"起一个音"的同一点产生，故应逐音 1:1。 */
static void test_inject_then_play_consistent(void)
{
    playpool_init();
    song_ev_t inj[4] = {{0, 0}, {1, 250}, {2, 250}, {3, 500}};
    const int slot = playpool_push("inject", inj, 4); /* 等价 rec_inject */
    assert(slot == 0);
    assert(playpool_used_count() == 1); /* pb=1 */

    const song_ev_t *pe = playpool_events(slot);
    const int n = playpool_event_count(slot);
    assert(pe != NULL && n == 4);

    uint8_t tick_seq[8];
    uint8_t snd_seq[8];
    int ticks = 0;
    int snds = 0;
    int64_t next_us = (int64_t)pe[0].delta_ms * 1000;
    int idx = 0;
    for (int64_t t = 0; t <= 2000000 && idx < n; t += 1000) {
        while (idx < n && t >= next_us) {
            tick_seq[ticks++] = pe[idx].note; /* main 的 tick 打点 */
            snd_seq[snds++] = pe[idx].note;   /* 音频层 snd 打点（同一起音点） */
            idx++;
            if (idx < n) {
                next_us += (int64_t)pe[idx].delta_ms * 1000;
            }
        }
    }
    assert(ticks == n && snds == n);                   /* 逐音各一条 */
    assert(memcmp(tick_seq, snd_seq, (size_t)n) == 0); /* snd 与 tick 一致 */
    for (int i = 0; i < n; ++i) {
        assert(tick_seq[i] == inj[i].note); /* 音序与注入一致 */
    }
}

/* 9) 回放池 flash 持久化：push 写穿 → 重启（init）→ load_from_flash 重建 */
static uint32_t play_flash_off(int slot)
{
    return (REC_SLOT_PLAYBACK_BASE + (uint32_t)slot) * REC_SLOT_SIZE;
}

static void test_flash_roundtrip_rebuild(void)
{
    playpool_clear_all(); /* 清 RAM + 擦 flash */
    assert(playpool_used_count() == 0);

    make_ev(3, 0);
    assert(playpool_push("A", ev, 3) == 0);
    make_ev(5, 2);
    assert(playpool_push("B", ev, 5) == 1);
    assert(playpool_used_count() == 2);
    assert(playpool_flash_used(0) && playpool_flash_used(1));

    /* 模拟重启：只清 RAM，再从 flash 重建 */
    playpool_init();
    assert(playpool_used_count() == 0); /* RAM 空了 */
    assert(playpool_load_from_flash() == 2);
    assert(playpool_used_count() == 2);
    assert(strcmp(playpool_name(0), "A") == 0 && playpool_event_count(0) == 3);
    assert(strcmp(playpool_name(1), "B") == 0 && playpool_event_count(1) == 5);
    const song_ev_t *pe = playpool_events(1);
    assert(pe != NULL && pe[4].note == (uint8_t)((2 + 4) % 8));
    /* 写序保留 → FIFO 判据跨重启一致：最旧仍是槽 0 */
    assert(playpool_next_slot() == 2); /* 有空槽时优先空槽 */
    playpool_push("C", ev, 3);
    assert(playpool_next_slot() == 3);
}

/* 10) 头解析/校验失败 → 按空槽处理 */
static void test_flash_corrupt_treated_empty(void)
{
    playpool_clear_all();
    make_ev(4, 0);
    assert(playpool_push("X", ev, 4) == 0);
    assert(playpool_flash_used(0));

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rec");
    assert(part != NULL);

    /* 10a) 擦掉该槽（空/0xFF）→ 按空槽 */
    assert(esp_partition_erase_range(part, play_flash_off(0), REC_SLOT_SIZE) == 0);
    assert(!playpool_flash_used(0));
    playpool_init();
    assert(playpool_load_from_flash() == 0);
    assert(!playpool_slot_used(0) && playpool_used_count() == 0); /* 空槽 → 暂无记录 */

    /* 10b) 写回合法槽后改坏 magic → 按空槽 */
    make_ev(4, 0);
    assert(playpool_push("Y", ev, 4) == 0);
    uint32_t bad = 0xDEADBEEFu;
    assert(esp_partition_write(part, play_flash_off(0), &bad, 4) == 0); /* 破坏 magic */
    assert(!playpool_flash_used(0));
    playpool_init();
    assert(playpool_load_from_flash() == 0);

    /* 10c) 破坏校验和（改名字首字节；'Z'=0x5A，故用 0x11 使其真正变化）→ 按空槽 */
    make_ev(4, 0);
    assert(playpool_push("Z", ev, 4) == 0);
    const uint8_t junk = 0x11;
    assert(esp_partition_write(part, play_flash_off(0) + 32u, &junk, 1) == 0); /* 改名字首字节 */
    playpool_init();
    assert(playpool_load_from_flash() == 0);
}

/* 11) FIFO"最旧"按 seq 判断：满 5 覆盖最旧槽，重启后仍正确 */
static void test_fifo_oldest_by_seq(void)
{
    playpool_clear_all();
    const char *names[6] = {"a", "b", "c", "d", "e", "f"};
    for (int i = 0; i < 5; ++i) {
        make_ev(2, (uint8_t)i);
        assert(playpool_push(names[i], ev, 2) == i);
        assert(playpool_slot_seq(i) == (uint32_t)(i + 1));
    }
    assert(playpool_next_slot() == 0); /* 最旧 = seq 最小 = 槽 0 */
    make_ev(7, 7);
    assert(playpool_push("f", ev, 7) == 0); /* 覆盖最旧槽 0 */
    assert(playpool_slot_seq(0) == 6);
    assert(playpool_next_slot() == 1); /* 现在最旧是槽 1（seq=2） */

    /* 重启重建：seq 保留 → 仍指向槽 1 */
    playpool_init();
    assert(playpool_load_from_flash() == 5);
    assert(strcmp(playpool_name(0), "f") == 0);
    assert(strcmp(playpool_name(1), "b") == 0 && playpool_slot_seq(1) == 2);
    assert(playpool_next_slot() == 1);
}

/* 12) pb:N 与 push / 清空联动；空槽=无记录 */
static void test_pb_and_clear(void)
{
    playpool_clear_all();
    assert(playpool_used_count() == 0); /* pb=0 */
    for (int i = 0; i < PLAY_POOL_SLOTS; ++i) {
        assert(!playpool_slot_used(i)); /* 空槽：播"暂无记录" */
        assert(!playpool_flash_used(i));
    }
    make_ev(3, 1);
    playpool_push("u", ev, 3);
    assert(playpool_used_count() == 1); /* pb=1 */
    playpool_push("v", ev, 3);
    assert(playpool_used_count() == 2); /* pb=2 */

    playpool_clear_all(); /* 清空：RAM + flash 一起 */
    assert(playpool_used_count() == 0);
    assert(playpool_load_from_flash() == 0); /* flash 也空了 */
}

/* 13) 起播首音延迟钳制：≤300ms（真实录音首音间隔可能很大 → 开头静默被误当"没播放"） */
static void test_first_delay_clamp(void)
{
    assert(PLAY_FIRST_NOTE_DELAY_MAX_MS == 300u);
    assert(play_first_delay_clamp(0) == 0);
    assert(play_first_delay_clamp(120) == 120);
    assert(play_first_delay_clamp(300) == 300);
    assert(play_first_delay_clamp(301) == 300);
    assert(play_first_delay_clamp(5000) == 300); /* 开头数秒静默 → 钳到 300ms */
    assert(play_first_delay_clamp(65535) == 300);

    /* 与起播公式一致（intro=0）：存了首音间隔 5000ms 的谱，起播首音延迟仍 ≤300ms */
    song_ev_t e[2] = {{0, 5000}, {3, 400}};
    playpool_clear_all();
    assert(playpool_push("slow", e, 2) == 0);
    const song_ev_t *pe = playpool_events(0);
    assert(pe != NULL && pe[0].delta_ms == 5000);
    const uint16_t used_delay = play_first_delay_clamp(pe[0].delta_ms);
    assert(used_delay <= PLAY_FIRST_NOTE_DELAY_MAX_MS);
}

int main(void)
{
    test_builtins_only_in_audition();
    test_self_record_visible_in_playback();
    test_game_record_into_playback_not_audition();
    test_playback_fifo_overwrite();
    test_song_play_reads_audition_pool();
    test_playback_count_changes();
    test_playback_advance_order();
    test_inject_then_play_consistent();
    test_flash_roundtrip_rebuild();
    test_flash_corrupt_treated_empty();
    test_fifo_oldest_by_seq();
    test_pb_and_clear();
    test_first_delay_clamp();

    printf("piano_pool_tests: all passed (dual-pool/pb/advance/flash/fifo/first-delay)\n");
    return 0;
}
