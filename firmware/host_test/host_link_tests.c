#include "host_link.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * host_link 命令“解析 + 分发”的宿主测试。
 *
 * 直接喂一行 JSON 给 host_link_handle_line，验证每个命令被正确识别并转发到
 * 对应 handler；同时回归旧命令（volume/bpm/note）不被破坏，且 "n" 不会被
 * 误判成 "note" 里的子串。
 *
 * 只测解析/分发，不调用发送函数，因此依赖由 pattern_stub.c / usb_serial_jtag_stub.c
 * 提供能链接的空实现即可。
 */

/* ---- handler 记录 ---- */
static int g_song = -1;
static int g_song_calls = 0;
static int g_demo_calls = 0;
static uint32_t g_vb_total = 0;
static int g_vb_calls = 0;
static char g_vd[1024];
static int g_vd_calls = 0;
static int g_ve_calls = 0;
static int g_vr_calls = 0;
static int g_vdft_calls = 0;
static int g_vol = -1;
static int g_vol_calls = 0;
static int g_bpm = -1;
static int g_bpm_calls = 0;
static int g_bri = -1;
static int g_bri_calls = 0;
static int g_note_n = -1;
static int g_note_v = -1;
static int g_note_calls = 0;

/* ---- 八音琴曲库 handler 记录 ---- */
static int g_sa_slot = -1;
static char g_sa_name[64];
static int g_sa_cnt = 0;
static int g_sa_calls = 0;
static song_ev_t g_sa_ev[8];
static int g_sd_slot = -1;
static int g_sd_calls = 0;
static int g_sl_calls = 0;
static int g_sp_slot = -1;
static int g_sp_calls = 0;
/* ---- 台架自测命令 ---- */
static int g_ri_cnt = 0;
static int g_ri_calls = 0;
static song_ev_t g_ri_ev[8];
static int g_pp_slot = -1;
static int g_pp_calls = 0;

static void h_song(int idx)
{
    g_song = idx;
    g_song_calls++;
}
static void h_demo(void) { g_demo_calls++; }
static void h_vb(uint32_t total)
{
    g_vb_total = total;
    g_vb_calls++;
}
static void h_vd(const char *b64)
{
    strncpy(g_vd, b64, sizeof(g_vd) - 1);
    g_vd[sizeof(g_vd) - 1] = '\0';
    g_vd_calls++;
}
static void h_ve(void) { g_ve_calls++; }
static void h_vr(void) { g_vr_calls++; }
static void h_vdft(void) { g_vdft_calls++; }
static void h_vol(uint8_t v)
{
    g_vol = v;
    g_vol_calls++;
}
static void h_bpm(uint16_t b)
{
    g_bpm = b;
    g_bpm_calls++;
}
static void h_bri(uint8_t v)
{
    g_bri = v;
    g_bri_calls++;
}
static void h_note(uint8_t n, uint8_t v)
{
    g_note_n = n;
    g_note_v = v;
    g_note_calls++;
}

static void h_song_add(uint8_t slot, const char *name, const song_ev_t *ev, int count)
{
    g_sa_slot = slot;
    g_sa_cnt = count;
    g_sa_calls++;
    g_sa_name[0] = '\0';
    if (name != NULL) {
        strncpy(g_sa_name, name, sizeof(g_sa_name) - 1);
        g_sa_name[sizeof(g_sa_name) - 1] = '\0';
    }
    for (int i = 0; i < count && i < 8; ++i) {
        g_sa_ev[i] = ev[i];
    }
}
static void h_song_del(uint8_t slot)
{
    g_sd_slot = slot;
    g_sd_calls++;
}
static void h_song_list(void) { g_sl_calls++; }
static void h_song_play(uint8_t slot)
{
    g_sp_slot = slot;
    g_sp_calls++;
}
static void h_rec_inject(const song_ev_t *ev, int count)
{
    g_ri_cnt = count;
    g_ri_calls++;
    for (int i = 0; i < count && i < 8; ++i) {
        g_ri_ev[i] = ev[i];
    }
}
static void h_pb_play(uint8_t slot)
{
    g_pp_slot = slot;
    g_pp_calls++;
}

static void reset_counters(void)
{
    g_song = -1;
    g_song_calls = 0;
    g_demo_calls = 0;
    g_vb_total = 0;
    g_vb_calls = 0;
    g_vd[0] = '\0';
    g_vd_calls = 0;
    g_ve_calls = 0;
    g_vr_calls = 0;
    g_vdft_calls = 0;
    g_vol = -1;
    g_vol_calls = 0;
    g_bpm = -1;
    g_bpm_calls = 0;
    g_bri = -1;
    g_bri_calls = 0;
    g_note_n = -1;
    g_note_v = -1;
    g_note_calls = 0;
    g_sa_slot = -1;
    g_sa_name[0] = '\0';
    g_sa_cnt = 0;
    g_sa_calls = 0;
    g_sd_slot = -1;
    g_sd_calls = 0;
    g_sl_calls = 0;
    g_sp_slot = -1;
    g_sp_calls = 0;
    g_ri_cnt = 0;
    g_ri_calls = 0;
    g_pp_slot = -1;
    g_pp_calls = 0;
}

static void install_handlers(void)
{
    host_link_handlers_t h;
    memset(&h, 0, sizeof(h));
    h.on_song = h_song;
    h.on_demo = h_demo;
    h.on_voice_begin = h_vb;
    h.on_voice_data = h_vd;
    h.on_voice_end = h_ve;
    h.on_voice_replay = h_vr;
    h.on_voice_default = h_vdft;
    h.on_volume = h_vol;
    h.on_brightness = h_bri;
    h.on_bpm = h_bpm;
    h.on_note = h_note;
    h.on_song_add = h_song_add;
    h.on_song_del = h_song_del;
    h.on_song_list = h_song_list;
    h.on_song_play = h_song_play;
    h.on_rec_inject = h_rec_inject;
    h.on_pb_play = h_pb_play;
    host_link_set_handlers(&h);
}

static void test_song_command(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"song\",\"v\":2}");
    assert(g_song_calls == 1 && g_song == 2);
}

static void test_demo_command(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"demo\"}");
    assert(g_demo_calls == 1);
}

static void test_voice_begin(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"voice_begin\",\"n\":12345}");
    assert(g_vb_calls == 1 && g_vb_total == 12345);
}

static void test_voice_data(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"voice_data\",\"d\":\"QUFBQQ==\"}");
    assert(g_vd_calls == 1);
    assert(strcmp(g_vd, "QUFBQQ==") == 0);
}

static void test_voice_end(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"voice_end\"}");
    assert(g_ve_calls == 1);
}

static void test_voice_replay(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"voice_replay\"}");
    assert(g_vr_calls == 1);
}

static void test_voice_default(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"voice_default\"}");
    assert(g_vdft_calls == 1);
}

/* 旧命令回归：volume 解析 + 上限钳位（>127 拉回 127） */
static void test_volume_regression(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"volume\",\"v\":300}");
    assert(g_vol_calls == 1 && g_vol == 127);

    reset_counters();
    host_link_handle_line("{\"t\":\"volume\",\"v\":55}");
    assert(g_vol_calls == 1 && g_vol == 55);
}

static void test_bpm_regression(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"bpm\",\"v\":140}");
    assert(g_bpm_calls == 1 && g_bpm == 140);
}

/* note：key 必须是 "n":，不能被 "note" 误匹配 */
static void test_note_key_not_confused(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"note\",\"n\":3,\"v\":100}");
    assert(g_note_calls == 1 && g_note_n == 3 && g_note_v == 100);
}

/* 未知命令 / 畸形行：不应调用任何 handler，也不崩 */
static void test_unknown_and_malformed(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"frobnicate\"}");
    assert(g_demo_calls == 0 && g_song_calls == 0 && g_vol_calls == 0);

    reset_counters();
    host_link_handle_line("{not json at all}");
    assert(g_demo_calls == 0 && g_song_calls == 0 && g_vol_calls == 0);
}

/*
 * 网页滑块发来的是 JSON 字符串值：{"t":"volume","v":"80"}。
 * 之前 extract_int_field 只认裸数字，字符串值解析失败 → 回调恒用默认值，
 * 滑块拖动时硬件无反应（Bug 2）。修复后须能正确解析引号内的数字。
 */
static void test_string_value_commands(void)
{
    reset_counters();
    host_link_handle_line("{\"t\":\"volume\",\"v\":\"80\"}");
    assert(g_vol_calls == 1 && g_vol == 80);

    reset_counters();
    host_link_handle_line("{\"t\":\"brightness\",\"v\":\"120\"}");
    assert(g_bri_calls == 1 && g_bri == 120);

    /* 字符串值也要走上限钳位：超 127 拉回 127 */
    reset_counters();
    host_link_handle_line("{\"t\":\"volume\",\"v\":\"255\"}");
    assert(g_vol_calls == 1 && g_vol == 127);

    /* 非数字字符串：解析失败，不应调用 handler（也不崩） */
    reset_counters();
    host_link_handle_line("{\"t\":\"volume\",\"v\":\"abc\"}");
    assert(g_vol_calls == 0);
}

/* ---- 八音琴曲库命令：{"v":2,"cmd":"song_*",...}（字段名/命令名固定）---- */

static void test_song_add_command(void)
{
    reset_counters();
    host_link_handle_line(
        "{\"v\":2,\"cmd\":\"song_add\",\"slot\":0,\"name\":\"小星星\",\"notes\":[[0,300],[2,450]]}");
    assert(g_sa_calls == 1 && g_sa_slot == 0);
    assert(strcmp(g_sa_name, "小星星") == 0);
    assert(g_sa_cnt == 2);
    assert(g_sa_ev[0].note == 0 && g_sa_ev[0].delta_ms == 300);
    assert(g_sa_ev[1].note == 2 && g_sa_ev[1].delta_ms == 450);
}

/* 缺 slot：报错、不调用 handler */
static void test_song_add_bad_slot(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"song_add\",\"name\":\"x\",\"notes\":[[0,100]]}");
    assert(g_sa_calls == 0);
}

static void test_song_del_command(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"song_del\",\"slot\":3}");
    assert(g_sd_calls == 1 && g_sd_slot == 3);
}

static void test_song_list_command(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"song_list\"}");
    assert(g_sl_calls == 1);
}

static void test_song_play_command(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"song_play\",\"slot\":4}");
    assert(g_sp_calls == 1 && g_sp_slot == 4);
}

/* ---- 台架自测命令：rec_inject / pb_play ---- */
static void test_rec_inject_command(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"rec_inject\",\"notes\":[[0,0],[3,250],[7,500]]}");
    assert(g_ri_calls == 1 && g_ri_cnt == 3);
    assert(g_ri_ev[0].note == 0 && g_ri_ev[0].delta_ms == 0);
    assert(g_ri_ev[1].note == 3 && g_ri_ev[1].delta_ms == 250);
    assert(g_ri_ev[2].note == 7 && g_ri_ev[2].delta_ms == 500);
}

static void test_pb_play_command(void)
{
    reset_counters();
    host_link_handle_line("{\"v\":2,\"cmd\":\"pb_play\",\"slot\":2}");
    assert(g_pp_calls == 1 && g_pp_slot == 2);
}

int main(void)
{
    install_handlers();

    test_song_command();
    test_demo_command();
    test_voice_begin();
    test_voice_data();
    test_voice_end();
    test_voice_replay();
    test_voice_default();
    test_volume_regression();
    test_bpm_regression();
    test_note_key_not_confused();
    test_unknown_and_malformed();
    test_string_value_commands();
    test_song_add_command();
    test_song_del_command();
    test_song_list_command();
    test_song_play_command();
    test_song_add_bad_slot();
    test_rec_inject_command();
    test_pb_play_command();

    printf("host_link_tests: all passed (song/demo/voice_*/volume/bpm/note, song_add/del/list/"
           "play, no regressions)\n");
    return 0;
}
