#include "audio_click.h"
#include "board_keys.h"
#include "board_pins.h"
#include "board_power.h"
#include "clock.h"
#include "diag_config.h"
#include "driver/ledc.h"
#include "enc_dial.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture.h"
#include "host_link.h"
#include "led_status.h"
#include "math.h"
#include "music_notes.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pattern.h"
#include "piano_fsm.h"
#include "play_pool.h"
#include "prompt.h"
#include "recorder.h"
#include "song_pool.h"
#include "tempo.h"
#include "voice_store.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "bayin";
static bool s_audio_ready;

/*
 * 日志上报（★ COM5 可见通道）：IDF console 在本板不可见，所有关键打点改走
 * host_link 的换行 JSON 通道：{"t":"log","tag":"...","msg":"..."}。
 */
#define RLOG(tag, ...)                                                                     \
    do {                                                                                   \
        char _lbuf[160];                                                                   \
        snprintf(_lbuf, sizeof(_lbuf), __VA_ARGS__);                                       \
        host_link_send_log((tag), _lbuf);                                                   \
    } while (0)

#if BAYIN_ENABLE_DIAG
/*
 * ★ 台架自测（BAYIN_ENABLE_DIAG）：音频层每起一个音符采样时产生 snd 事件，
 * 经 SPSC 环形缓冲交给主循环落 JSON 日志（不在音频任务里 printf，避免 I2S 抖动）。
 */
static volatile uint8_t s_snd_ring[32];
static volatile uint8_t s_snd_w, s_snd_r;
static void snd_note_log_cb(uint8_t note)
{
    const uint8_t n = (uint8_t)((s_snd_w + 1u) & 31u);
    if (n != s_snd_r) { /* 满则丢弃最旧 */
        s_snd_ring[s_snd_w] = note;
        s_snd_w = n;
    }
}
static void drain_snd_log(void)
{
    while (s_snd_r != s_snd_w) {
        const uint8_t note = s_snd_ring[s_snd_r];
        s_snd_r = (uint8_t)((s_snd_r + 1u) & 31u);
        RLOG("snd", "note=%d", (int)note);
    }
}
#endif

/*
 * 八音琴（新项目）。
 *
 * 五态：主目录 / 试听 / 自弹 / 游戏 / 回放（见 piano_fsm.h），开机即在主目录。
 * 键位（见 docs/八音琴-需求与决策记录.md 第 2 节）：
 *   主目录  S1~S4 选模式，S5~S8 报"本键无功能"
 *   试听/回放 S1~S5 选曲/选槽，S6~S8 无功能
 *   自弹    S1~S8 = 哆唻咪发嗦啦唏高音哆
 * 旋钮：旋转 = 音量（全局）；短按 = 上下文动作；长按 ≥5s = 回主目录。
 * 八度固定 0 档（已删除"长按旋钮升八度"）。
 * ★ 本工程内禁用固件切换（fw_switch glue）：不再调用，5s 手势不再被抢走。
 */

/* 旋钮长按 = 回主目录（终极按钮） */
#define ENC_LONG_PRESS_MS 5000
#define ENC_VOLUME_STEP 4

/* 空闲/主目录灯颜色（"亮几颗=数量"；颜色不固定 D3） */
static const uint8_t MENU_LED[3] = {20, 60, 80}; /* 主目录 D1~D4：钢蓝 */
static const uint8_t IDLE_LED[3] = {60, 60, 20}; /* 试听/回放空闲：琥珀 */

/* 节拍/传输（沿用 host 通道；八音琴不再用其驱动灯） */
static uint8_t s_last_beat_in_bar;
static uint8_t s_last_step;
static uint32_t s_last_bar;
static uint16_t s_last_tick;
static uint32_t s_quarter_count;
static bool s_fill_held;
static bool s_drum_mode;

static gesture_state_t s_enc_gesture;
static bool s_prev_keys[8];

/* 回放引擎：起播时把事件指针/条数缓存下来（试听池或回放池皆可），tick 直接读缓存 */
static int64_t s_pb_next_us;
static int64_t s_pb_remaining_us;
static int s_pb_idx;
static int s_pb_slot = -1;
static int s_pb_n;
static const song_ev_t *s_pb_ev;
static bool s_pb_active;

/* 最近一次发声的音（用于"只亮当前那颗，颜色随音符变"） */
static uint8_t s_last_play_note = MUSIC_NOTE_DO;

/* 灯亮度（旋钮在主目录调它；对 WS2812 D1~D5 与状态灯同时生效）。
 * 掉电记忆走 NVS "piano"/"brightness"：脏标记 + 停转/离开主目录时一次性落盘。 */
static uint8_t s_brightness = 80;
static enc_flush_t s_bri_flush;
static enc_flush_t s_vol_beep; /* 音量反馈音节流（停转 ~300ms 后一声） */

/* 「Pad 因语音忙被丢弃」的反馈（方案 A）：D3+D2 同时快闪一次（~300ms，节流 200ms）。
 * 不用短音——短音会经音频引擎的"让位"规则掐断正在播的提示语音。 */
static feedback_gate_t s_drop_gate;
static int64_t s_drop_flash_until_us;
#define DROP_FLASH_US 300000 /* D3+D2 快闪 300ms（更明显，用户一眼可辨） */

/* 语音提示：素材见 prompt_format.h，资源在 rec 分区 slot5+。
 * ★ 所有播报都经 piano_fsm 的"一次性语音闩锁"：main 每轮回调 piano_take_voice()
 *   取走待播句（取走即清，绝不重复），再交给 prompt_play 播放。 */

/* 内置 3 首儿歌占 slot0~2：选曲时播报歌名；其它槽无歌名素材 → -1 */
static int prompt_for_song_slot(int slot)
{
    switch (slot) {
    case 0:
        return PROMPT_SONG_TWINKLE;
    case 1:
        return PROMPT_SONG_TWO_TIGERS;
    case 2:
        return PROMPT_SONG_ODE;
    default:
        return -1;
    }
}

static void send_status(void)
{
    const uint8_t volume = s_audio_ready ? audio_click_get_volume() : 100;
    host_link_send_status(tempo_get_bpm(), tempo_is_running(), s_last_beat_in_bar, s_last_step,
                          s_last_bar, s_last_tick, s_drum_mode, volume,
                          led_status_get_brightness(), (uint8_t)playpool_used_count());
}

static void render_event(const audio_beat_event_t *event, int64_t now_us)
{
    s_last_beat_in_bar = event->beat_in_bar;
    s_last_step = event->step;
    s_last_bar = event->bar;
    s_last_tick = event->tick;

    if (event->is_quarter) {
        (void)now_us;
        host_link_send_beat(event->accent, event->beat_in_bar, event->step);
        s_quarter_count++;
    }
    if (event->is_step) {
        host_link_send_position(event->bar, event->step, event->beat_in_bar, event->tick,
                                event->accent);
    }
}

static void transport_set(bool running, bool restart, bool from_host)
{
    if (running) {
        if (tempo_is_running() && !restart) {
            return;
        }
        tempo_set_running(true);
        if (restart) {
            s_last_beat_in_bar = 0;
            s_last_step = 0;
            s_last_bar = 0;
            s_last_tick = 0;
            s_quarter_count = 0;
        }
        if (s_audio_ready) {
            (void)audio_click_set_bpm(tempo_get_bpm());
            (void)audio_click_set_running(true, restart);
        }
        ESP_LOGI(TAG, "%s @ %u BPM", restart ? "PLAY" : "CONTINUE", tempo_get_bpm());
        if (!from_host) {
            if (restart) {
                host_link_send_start();
            } else {
                host_link_send_continue();
            }
        }
        send_status();
        return;
    }

    if (!tempo_is_running()) {
        return;
    }
    tempo_set_running(false);
    if (s_audio_ready) {
        (void)audio_click_stop();
    }
    if (s_audio_ready) {
        audio_click_get_position(&s_last_bar, &s_last_step, &s_last_beat_in_bar, &s_last_tick);
    }
    ESP_LOGI(TAG, "STOP");
    if (!from_host) {
        host_link_send_stop();
    }
    send_status();
}

static void on_host_transport(bool start, bool restart)
{
    transport_set(start, restart, true);
}

static void on_host_bpm(uint16_t bpm)
{
    tempo_set_bpm(beatbox_clamp_bpm(bpm));
    if (s_audio_ready) {
        (void)audio_click_set_bpm(tempo_get_bpm());
    }
    send_status();
}

static void on_host_swing(uint8_t swing)
{
    pattern_set_swing(swing);
    send_status();
}

static void on_host_variation(uint8_t var)
{
    pattern_request_variation(var);
    send_status();
}

static void on_host_fill(bool held)
{
    s_fill_held = held;
    pattern_set_fill(held);
    send_status();
}

static void on_host_note(uint8_t note, uint8_t velocity)
{
    if (s_audio_ready) {
        (void)audio_click_play_note(note, velocity ? velocity : 127);
    }
    host_link_send_note(note, velocity ? velocity : 127);
}

static void on_host_click(bool enabled)
{
    pattern_set_click(enabled);
    if (s_audio_ready) {
        (void)audio_click_set_metronome(enabled);
    }
    send_status();
}

static void on_host_mode(bool drum_mode)
{
    s_drum_mode = drum_mode;
    if (s_audio_ready) {
        (void)audio_click_set_mode(drum_mode ? AUDIO_MODE_DRUM : AUDIO_MODE_METRONOME);
    }
    send_status();
}

static void persist_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open("piano", NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u8(handle, key, value);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

static void on_host_volume(uint8_t volume)
{
    if (s_audio_ready) {
        (void)audio_click_set_volume(volume);
    }
    persist_u8("volume", volume);
    send_status();
}

static void on_host_brightness(uint8_t brightness)
{
    /* 网页亮度滑块与旋钮共用同一 NVS 键/同一变量，避免两边打架 */
    s_brightness = brightness;
    led_status_set_brightness(brightness);
    enc_flush_init(&s_bri_flush); /* 已显式落盘，清脏标记 */
    persist_u8("brightness", brightness);
    send_status();
}

static void on_host_pattern_set(uint8_t bank, uint32_t rev, const uint8_t *bytes)
{
    const esp_err_t err = pattern_set_bank(bank, rev, bytes);
    if (err == ESP_OK) {
        host_link_send_ack("pattern_set", true, pattern_revision());
        send_status();
    } else {
        host_link_send_error("pattern_set", "bad_bank");
        host_link_send_ack("pattern_set", false, pattern_revision());
    }
}

static void on_host_save(void)
{
    host_link_send_ack("save", true, pattern_revision());
}

static void on_host_ping(void)
{
    host_link_send_pattern_dump();
    send_status();
}

static void on_host_record(bool armed)
{
    /*
     * 记谱会话（网页在游戏模式发 record:1 → record:0）。
     * 会话结束时：把本次记谱 push 进【回放池】（RAM，不掉电持久），
     * 不再依赖网页 song_add，因而不会污染试听池（flash 曲库）。
     * 游戏失败/中途退出时网页直接不发 record:0，本次记录自然不落槽。
     */
    const piano_mode_t mode = piano_get_mode();
    if (armed) {
        recorder_start();
        RLOG("rec", "host start (mode=%d)", (int)mode);
        return;
    }
    recorder_stop();
    const recorder_event_t *ev = recorder_events();
    int n = recorder_event_count();
    if (n > PLAY_POOL_MAX_EVENTS) {
        n = PLAY_POOL_MAX_EVENTS;
    }
    if (n > 0) {
        static song_ev_t buf[PLAY_POOL_MAX_EVENTS];
        for (int i = 0; i < n; ++i) {
            buf[i].note = ev[i].note;
            buf[i].delta_ms = ev[i].delta_ms;
        }
        const char *nm = (mode == PIANO_MODE_GAME) ? "游戏" : "记谱";
        const int slot = playpool_push(nm, buf, n);
        RLOG("rec", "host end (mode=%d) -> 回放池 slot=%d notes=%d", (int)mode, slot, n);
    } else {
        RLOG("rec", "host end (mode=%d) 0 notes, 丢弃", (int)mode);
    }
}

static void on_host_voice_begin(uint32_t total)
{
    const esp_err_t e = voice_store_upload_begin(total);
    host_link_send_ack("voice", e == ESP_OK, 0);
}

static void on_host_voice_data(const char *b64)
{
    voice_store_upload_append(b64, (uint32_t)strlen(b64));
}

static void on_host_voice_end(void)
{
    const esp_err_t e = voice_store_upload_end();
    host_link_send_ack("voice", e == ESP_OK, 0);
    send_status();
}

static void on_host_voice_replay(void)
{
    voice_store_play_greeting();
}

static void on_host_voice_default(void)
{
    voice_store_clear_import();
    send_status();
}

/* ---- 旧录音 flash 槽命令（沿用协议；八音琴回放池不再写 flash）---- */
static void on_host_rec_save(uint8_t slot)
{
    const esp_err_t e = recorder_save_slot(slot);
    host_link_send_ack("rec_save", e == ESP_OK, 0);
    send_status();
}

static void on_host_rec_load(uint8_t slot)
{
    const esp_err_t e = recorder_load_slot(slot);
    host_link_send_ack("rec_load", e == ESP_OK, 0);
    send_status();
}

static void on_host_rec_del(uint8_t slot)
{
    const esp_err_t e = recorder_delete_slot(slot);
    host_link_send_ack("rec_del", e == ESP_OK, 0);
    send_status();
}

static void on_host_rec_list(void)
{
    char line[192];
    int off = snprintf(line, sizeof(line), "{\"t\":\"rec_list\",\"n\":%u,\"used\":[",
                       (unsigned)recorder_slot_count());
    for (uint8_t s = 0; s < REC_FLASH_SLOTS; ++s) {
        if (recorder_slot_used(s)) {
            off += snprintf(line + off, sizeof(line) - (size_t)off, "%u,", (unsigned)s);
        }
    }
    if (off > 0 && line[off - 1] == ',') {
        off--;
    }
    snprintf(line + off, sizeof(line) - (size_t)off, "]}");
    host_link_send_line(line);
}

static void on_host_rec_play(uint8_t slot)
{
    if (recorder_load_slot(slot) == ESP_OK) {
        recorder_playback_start(esp_timer_get_time());
    }
    send_status();
}

/* ==== 八音琴曲库：5 槽（网页 song_add / song_del / song_list / song_play）==== */

static void on_host_song_list(void)
{
    /* song_list 以 flash 为准：只回报 flash 里有有效曲的槽 */
    char line[384];
    char names[SONGP_SLOTS][SONGP_NAME_MAX];
    int counts[SONGP_SLOTS];
    int n = 0;
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        counts[i] = songpool_flash_meta(i, names[i], SONGP_NAME_MAX);
        if (counts[i] > 0) {
            n++;
        }
    }
    int off = snprintf(line, sizeof(line), "{\"type\":\"song_list\",\"n\":%d,\"slots\":[", n);
    bool first = true;
    for (int i = 0; i < SONGP_SLOTS; ++i) {
        if (counts[i] <= 0) {
            continue;
        }
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        "%s{\"slot\":%d,\"name\":\"%s\",\"count\":%d}", first ? "" : ",", i,
                        names[i], counts[i]);
        first = false;
    }
    snprintf(line + off, sizeof(line) - (size_t)off, "]}");
    host_link_send_line(line);
}

static void on_host_song_add(uint8_t slot, const char *name, const song_ev_t *ev, int count)
{
    const int r = songpool_set((int)slot, name, ev, count);
    if (r >= 0) {
        (void)songpool_flash_save(r); /* 网页显式写入 → 同步落 flash，掉电不丢 */
    }
    ESP_LOGI(TAG, "song_add slot %u '%s' count %d -> %d", (unsigned)slot, name ? name : "",
             count, r);
    on_host_song_list();
}

static void on_host_song_del(uint8_t slot)
{
    songpool_del((int)slot);
    (void)songpool_flash_clear((int)slot); /* 清 RAM 槽 + 清 flash 槽 */
    ESP_LOGI(TAG, "song_del slot %u", (unsigned)slot);
    on_host_song_list();
}

static void on_host_song_play(uint8_t slot);

/* ==== 发声 / 点灯 / 回放 / 记谱 ==== */

/* 弹一个音：S1~S7 用离线采样；S8（高音哆）复用 do 采样升八度（无额外采样文件）。 */
static void play_note(uint8_t note)
{
    if (note >= MUSIC_NOTE_COUNT) {
        return;
    }
    if (s_audio_ready) {
        (void)audio_click_stop_pcm(); /* 一弹琴，开机欢迎语让位 */
        if (note == MUSIC_NOTE_DO_HIGH) {
            (void)audio_click_play_note_sample((uint8_t)MUSIC_NOTE_DO, 127, 1);
        } else {
            (void)audio_click_play_note_sample(note, 127, 0);
        }
    }
    s_last_play_note = note;
}

/* 起播核心：试听池/回放池共用（缓存事件并预约首音延时） */
static void begin_song_playback(const song_ev_t *ev, int n, int slot, int name_id,
                                const char *pool_tag)
{
    piano_set_selected(slot);

    /* 选曲：登记播报歌名（内置 3 首有素材），并把首音延后到播完，避免被琴音掐断。 */
    int64_t intro_us = 0;
    if (name_id >= 0) {
        piano_request_voice((prompt_id_t)name_id);
        const int frames = prompt_duration_frames(name_id);
        if (frames > 0) {
            intro_us = (int64_t)frames * 1000000 / 32000 + 150000; /* 播完 + 150ms 余量 */
        }
    }

    s_pb_slot = slot;
    s_pb_ev = ev;
    s_pb_n = n;
    s_pb_idx = 0;
    /* ★ 首音延迟钳制：真实录音首音间隔可能很大（开头静默被误当"没播放"），
     * 起播时钳到 ≤300ms（唯一钳制点，不改已存数据）。 */
    const int64_t first_delay_ms = play_first_delay_clamp(ev[0].delta_ms);
    s_pb_next_us = esp_timer_get_time() + intro_us + first_delay_ms * 1000;
    s_pb_active = true;
    s_last_play_note = ev[0].note;
    piano_set_play(PIANO_PLAY_PLAYING);
    RLOG("play", "start pool=%s slot=%d notes=%d intro=%lldus first=%lldms", pool_tag, slot, n,
         (long long)intro_us, (long long)first_delay_ms);
}

/* 试听：只能来自 flash 曲库（试听池）；歌名播报内置 3 首 */
static void start_audition_slot(int slot)
{
    if (slot < 0 || slot >= SONGP_SLOTS || !songpool_slot_used(slot) ||
        songpool_event_count(slot) <= 0) {
        piano_request_voice(PROMPT_NO_RECORD);
        RLOG("play", "audition empty slot=%d", slot);
        return;
    }
    begin_song_playback(songpool_events(slot), songpool_event_count(slot), slot,
                        prompt_for_song_slot(slot), "audition");
}

/* 回放：只能来自独立 RAM 回放池（自弹/游戏）；无歌名素材 */
static void start_playback_slot(int slot)
{
    if (slot < 0 || slot >= PLAY_POOL_SLOTS || !playpool_slot_used(slot) ||
        playpool_event_count(slot) <= 0) {
        piano_request_voice(PROMPT_NO_RECORD);
        RLOG("play", "playback empty slot=%d", slot);
        return;
    }
    begin_song_playback(playpool_events(slot), playpool_event_count(slot), slot, -1, "playback");
}

/* 按当前模式选池起播（试听/回放短按用） */
static void start_current_pool_slot(int slot)
{
    if (piano_get_mode() == PIANO_MODE_PLAYBACK) {
        start_playback_slot(slot);
    } else {
        start_audition_slot(slot);
    }
}

static void stop_playback(void)
{
    /*
     * ★ 只停止推进，**不 flush 音频队列**：修复 stop/start 竞态——
     * audio_click_stop() 会让音频任务 reset 请求队列，而紧接着入队的"提示语音"
     * 可能在同一块内被 reset 掉（起播了提示却没声）。旧实现就是这种竞态。
     * 停止播放只需置 s_pb_active=false；残留的尾音由新提示/新音自然替换。
     */
    s_pb_active = false;
}

/* 试听/回放短按：暂停 / 继续（空闲时则开始播放当前选中槽） */
static void toggle_pause(void)
{
    const piano_play_t st = piano_get_play();
    if (st == PIANO_PLAY_PLAYING) {
        const int64_t now = esp_timer_get_time();
        s_pb_remaining_us = s_pb_next_us > now ? (s_pb_next_us - now) : 0;
        s_pb_active = false;
        piano_set_play(PIANO_PLAY_PAUSED);
        RLOG("play", "pause slot=%d", s_pb_slot);
    } else if (st == PIANO_PLAY_PAUSED) {
        s_pb_next_us = esp_timer_get_time() + s_pb_remaining_us;
        s_pb_active = true;
        piano_set_play(PIANO_PLAY_PLAYING);
        RLOG("play", "resume slot=%d", s_pb_slot);
    } else {
        int slot = piano_get_selected();
        if (slot < 0) {
            slot = 0;
        }
        start_current_pool_slot(slot);
    }
}

/* 自弹短按：开始记谱 / 再按结束并保存（进【回放池】RAM FIFO，绝不写 flash） */
static void toggle_record(void)
{
    if (piano_is_recording()) {
        recorder_stop();
        const recorder_event_t *ev = recorder_events();
        int n = recorder_event_count();
        piano_set_recording(false);
        if (n > PLAY_POOL_MAX_EVENTS) {
            n = PLAY_POOL_MAX_EVENTS;
        }
        if (n > 0) {
            static song_ev_t buf[PLAY_POOL_MAX_EVENTS];
            for (int i = 0; i < n; ++i) {
                buf[i].note = ev[i].note;
                buf[i].delta_ms = ev[i].delta_ms;
            }
            const int slot = playpool_push("自弹", buf, n);
            RLOG("rec", "end 自弹 -> 回放池 slot=%d notes=%d", slot, n);
            piano_request_voice(PROMPT_SAVED);
        } else {
            RLOG("rec", "end 自弹 (0 notes, 丢弃)");
            piano_request_voice(PROMPT_NO_FUNCTION);
        }
    } else {
        recorder_start();
        piano_set_recording(true);
        RLOG("rec", "start 自弹 (回放池)");
        piano_request_voice(PROMPT_START_RECORD);
    }
}

static void enter_mode(piano_mode_t m)
{
    const int from = (int)piano_get_mode();
    (void)audio_click_stop_pcm();
    stop_playback();
    piano_enter_mode(m);
    RLOG("mode", "switch %d -> %d", from, (int)m);
    /* 离开主目录：若亮度有脏改动，立即一次性落盘（不是每格写） */
    if (from == PIANO_MODE_MENU && enc_flush_on_leave_menu(&s_bri_flush)) {
        persist_u8("brightness", s_brightness);
        RLOG("enc", "brightness flushed on leave menu = %d", s_brightness);
    }
}

static void on_host_song_play(uint8_t slot)
{
    RLOG("song_play", "slot=%u (试听池)", (unsigned)slot);
    (void)audio_click_stop_pcm();
    stop_playback();
    piano_enter_mode(PIANO_MODE_AUDITION); /* song_play 维持"试听池"语义 */
    start_audition_slot((int)slot);
    send_status();
}

/* 旧 song 命令：按槽位试听（与 song_play 同义） */
static void on_host_song(int idx)
{
    on_host_song_play(idx < 0 ? 0 : (uint8_t)idx);
}

/* 旧 demo 命令：试听当前选中槽（无则槽 0） */
static void on_host_demo(void)
{
    int slot = piano_get_selected();
    if (slot < 0) {
        slot = 0;
    }
    on_host_song_play((uint8_t)slot);
}

/* 旧 playback 命令：进回放模式，播回放池当前选中槽 */
static void on_host_playback(void)
{
    (void)audio_click_stop_pcm();
    stop_playback();
    piano_enter_mode(PIANO_MODE_PLAYBACK);
    int slot = piano_get_selected();
    if (slot < 0) {
        slot = 0;
    }
    start_playback_slot(slot);
    send_status();
}

#if BAYIN_ENABLE_DIAG
/* ★ 台架自测命令（关 BAYIN_ENABLE_DIAG 即整体移除，见 diag_config.h） */
static void on_host_rec_inject(const song_ev_t *ev, int count)
{
    /* rec_inject：等价自弹记谱产物 → 注入回放池（RAM） */
    const int slot = playpool_push("inject", ev, count);
    RLOG("diag", "rec_inject -> 回放池 slot=%d notes=%d pb=%d", slot, count,
         playpool_used_count());
    send_status();
}
static void on_host_pb_play(uint8_t slot)
{
    /* pb_play：等价回放模式按 S1 → 直接起播回放池第 slot 槽 */
    RLOG("diag", "pb_play slot=%u pb=%d", (unsigned)slot, playpool_used_count());
    (void)audio_click_stop_pcm();
    stop_playback();
    piano_enter_mode(PIANO_MODE_PLAYBACK);
    start_playback_slot((int)slot);
    send_status();
}
#endif

/* 音量反馈音：合成通道的短音（更轻更短：velocity 40 / 60ms），settle 后只发一次。
 * ★ 刻意**不用 play_note()**——那条会改 s_last_play_note → LED 跟着闪（副作用）。 */
static void volume_beep_emit(void)
{
    if (!s_audio_ready) {
        return;
    }
    (void)audio_click_play_pitch(music_note_frequency_hz(MUSIC_NOTE_DO, 0), 40, 60);
    RLOG("enc", "volume=%d", audio_click_get_volume()); /* 并入 settle：只报一条，不逐格刷串口 */
}

/*
 * 旋钮旋转（规格变更后，按模式分流）：
 *   - 主目录 PIANO_MODE_MENU → 调「灯亮度」（WS2812 D1~D5 + 状态灯，掉电记忆）
 *   - 其它模式 → 调「音量」（原逻辑；变值只标脏，停转 ~300ms 后由主循环播一声反馈）
 * 旋转不被任何模式屏蔽。
 */
static void apply_encoder_dial(int8_t delta)
{
    if (delta == 0) {
        return;
    }
    if (enc_dial_target(piano_get_mode() == PIANO_MODE_MENU) == ENC_DIAL_BRIGHTNESS) {
        /* —— 主目录：亮度（无需提示音，灯本身就即时反馈） —— */
        s_brightness = enc_brightness_clamp((int)s_brightness + (int)delta * ENC_BRIGHTNESS_STEP);
        led_status_set_brightness(s_brightness); /* WS2812 D1~D5 */
        enc_flush_mark(&s_bri_flush, (uint32_t)(esp_timer_get_time() / 1000)); /* 只标脏，不写 NVS */
        RLOG("enc", "brightness=%d", s_brightness);
        send_status(); /* bri 字段与旋钮同源 */
        return;
    }

    /* —— 其它模式：音量 —— */
    if (!s_audio_ready) {
        return;
    }
    /* (b) 让位：仅"确有 PCM 语音在播"时置位一次；不再逐格调 stop_pcm（杂音嫌疑） */
    if (audio_click_pcm_busy()) {
        (void)audio_click_stop_pcm();
    }
    const int next = (int)audio_click_get_volume() + (int)delta * ENC_VOLUME_STEP;
    const uint8_t vol = music_clamp_volume(next, BEATBOX_VOLUME_MAX);
    const uint8_t before = audio_click_get_volume();
    (void)audio_click_set_volume(vol);
    if (vol != before) {
        /* 只标脏：settle ~300ms 后由主循环播一声反馈并报一条日志（不逐格串口写） */
        enc_flush_mark(&s_vol_beep, (uint32_t)(esp_timer_get_time() / 1000));
    }
}

/*
 * 旋钮按下手势：短按 = 上下文动作；长按 ≥5s = 回主目录。
 * 手势判定走 gesture_update 纯函数（宿主测试覆盖）。
 */
static void handle_encoder_gesture(const board_input_snapshot_t *in, int64_t now_us)
{
    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    const gesture_action_t action =
        gesture_update(&s_enc_gesture, in->enc_press, in->enc_held, now_ms, ENC_LONG_PRESS_MS);

    if (action == GESTURE_ACTION_LONG_PRESS) {
        (void)audio_click_stop_pcm();
        /* 自弹记谱中：先自动保存再退出 */
        if (piano_is_recording()) {
            toggle_record();
        }
        stop_playback();
        piano_enter_menu();
        piano_request_voice(PROMPT_BACK_HOME); /* 长按旋钮：回到主目录（覆盖"请选择模式"） */
        ESP_LOGI(TAG, "long press -> main menu");
        RLOG("enc", "long>=5s -> main menu");
        send_status();
        return;
    }
    if (action == GESTURE_ACTION_SHORT_PRESS) {
        (void)audio_click_stop_pcm();
        const piano_short_action_t act =
            piano_short_press_action(piano_get_mode(), piano_get_play(), piano_is_recording());
        switch (act) {
        case PIANO_SHORT_MENU_REPROMPT:
            piano_request_voice(PROMPT_SELECT_MODE);
            break;
        case PIANO_SHORT_TOGGLE_PAUSE:
            toggle_pause();
            break;
        case PIANO_SHORT_TOGGLE_RECORD:
            toggle_record();
            break;
        case PIANO_SHORT_INVALID_PROMPT:
            piano_request_voice(PROMPT_NO_FUNCTION);
            break;
        default:
            break;
        }
        send_status();
    }
}

static void handle_pads(const board_input_snapshot_t *in, int64_t now_us, bool voice_busy)
{
    /* 按键状态始终回报给上位机（网页游戏判定用） */
    for (int i = 0; i < 8; ++i) {
        if (in->s[i] != s_prev_keys[i]) {
            host_link_send_key((uint8_t)i, in->s[i]);
        }
    }

    /*
     * 按键路由统一走 piano_fsm 的纯函数 piano_pad_event（与场景宿测共用）：
     * 语音播报期间返回 DROPPED（丢弃、不排队）；旋钮在别处处理，不受影响。
     */
    for (int i = 0; i < 8; ++i) {
        if (!in->s[i] || s_prev_keys[i]) {
            continue; /* 只要"按下"的上升沿 */
        }
        switch (piano_pad_event(i, voice_busy)) {
        case PIANO_PAD_ENTER_MODE: {
            const piano_mode_t target = piano_menu_key_mode(i);
            enter_mode(target); /* 内部登记"模式名"语音 + 记录 from→to */
            break;
        }
        case PIANO_PAD_PLAY_SLOT:
            start_current_pool_slot(i); /* 试听→试听池(flash)；回放→回放池(RAM) */
            break;
        case PIANO_PAD_PLAY_NOTE: {
            const int note = piano_note_key(i);
            if (note >= 0) {
                play_note((uint8_t)note);
                if (piano_get_mode() == PIANO_MODE_SELF && piano_is_recording()) {
                    recorder_add_note((uint8_t)note, now_us);
                }
                RLOG("pad", "note key=%d note=%d", i, note);
            }
            break;
        }
        case PIANO_PAD_NO_FUNCTION:
            piano_request_voice(PROMPT_NO_FUNCTION); /* S5~S8/ S6~S8 无功能 */
            RLOG("pad", "no-function key=%d", i);
            break;
        case PIANO_PAD_DROPPED:
            /* 方案 A：丢弃反馈 = D3 快闪一次（节流 200ms，连按不刷屏） */
            if (feedback_gate_allow(&s_drop_gate, (uint32_t)(now_us / 1000), FEEDBACK_DROP_MIN_MS)) {
                s_drop_flash_until_us = now_us + DROP_FLASH_US;
                RLOG("pad", "drop key=%d (voice busy) -> D3 flash", i);
            }
            break;
        default:
            break;
        }
    }

    for (int i = 0; i < 8; ++i) {
        s_prev_keys[i] = in->s[i];
    }
}

static void playback_tick(int64_t now_us)
{
    if (!s_pb_active) {
        return;
    }
    const song_ev_t *ev = s_pb_ev; /* 起播时缓存的池事件（试听池或回放池） */
    const int n = s_pb_n;
    if (ev == NULL || n <= 0) {
        s_pb_active = false;
        piano_set_play(PIANO_PLAY_IDLE);
        return;
    }
    while (s_pb_idx < n && now_us >= s_pb_next_us) {
        const int idx = s_pb_idx;
        play_note(ev[idx].note);
        RLOG("play", "tick idx=%d note=%d", idx, ev[idx].note); /* 逐音打点：区分"没推进"vs"推进了没声" */
        s_pb_idx++;
        if (s_pb_idx < n) {
            s_pb_next_us += (int64_t)ev[s_pb_idx].delta_ms * 1000;
        } else {
            s_pb_active = false;
            piano_set_play(PIANO_PLAY_IDLE);
            piano_request_voice(PROMPT_PLAY_END); /* 播放到末尾：播报"播放结束" */
            RLOG("play", "end slot=%d", s_pb_slot);
            break;
        }
    }
}

/* ==== 五灯整帧（灯铁律：播放/弹奏态只亮当前那颗；空闲态亮几颗=数量）==== */

static void frame_zero(uint8_t frame[15])
{
    memset(frame, 0, 15);
}

static void frame_one(uint8_t frame[15], int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < 0 || index >= BOARD_WS2812_COUNT) {
        return;
    }
    frame[index * 3 + 0] = r;
    frame[index * 3 + 1] = g;
    frame[index * 3 + 2] = b;
}

static void frame_count(uint8_t frame[15], int count, const uint8_t rgb[3])
{
    for (int i = 0; i < count && i < BOARD_WS2812_COUNT; ++i) {
        frame_one(frame, i, rgb[0], rgb[1], rgb[2]);
    }
}

static void refresh_leds(void)
{
    uint8_t frame[15];
    frame_zero(frame);

    const piano_mode_t mode = piano_get_mode();
    if (mode == PIANO_MODE_MENU) {
        frame_count(frame, 4, MENU_LED); /* 主目录：D1~D4 亮 = 4 个入口 */
    } else if (mode == PIANO_MODE_AUDITION || mode == PIANO_MODE_PLAYBACK) {
        const int sel = piano_get_selected();
        const int pool_slots =
            (mode == PIANO_MODE_PLAYBACK) ? playpool_used_count() : songpool_used_count();
        if (piano_get_play() != PIANO_PLAY_IDLE && sel >= 0 && sel < PLAY_POOL_SLOTS) {
            /* 播放/选中：只亮对应 D_N，颜色随音符变，其余灭 */
            const music_color_t c = music_note_color((music_note_t)s_last_play_note);
            frame_one(frame, sel, c.r, c.g, c.b);
        } else {
            /* 空闲：亮几颗 = 本池已存槽数（试听=flash曲库；回放=RAM回放池） */
            frame_count(frame, piano_idle_leds(mode, pool_slots), IDLE_LED);
        }
    } else if (mode == PIANO_MODE_SELF) {
        if (piano_is_recording()) {
            /* 记谱中：只亮"回放池目标槽位" D_N，颜色随音符变 */
            const music_color_t c = music_note_color((music_note_t)s_last_play_note);
            frame_one(frame, playpool_next_slot(), c.r, c.g, c.b);
        }
        /* 未记谱：不亮音色灯 */
    }
    /* PIANO_MODE_GAME：由网页呈现，板子不点灯 */

    /* 「按键因语音忙被丢弃」反馈：窗口内把 D3+D2 同时覆盖成亮白快闪一次（~300ms） */
    if (esp_timer_get_time() < s_drop_flash_until_us) {
        frame_one(frame, 2, 120, 120, 120); /* D3 */
        frame_one(frame, 1, 120, 120, 120); /* D2 */
    }

    (void)led_status_set_frame(frame);
}

/*
 * 状态灯（GPIO42，单色 PWM）：录谱快闪 / 播放慢闪 / 其余柔和常亮。
 */
static void status_led_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BOARD_GPIO_STATUS_LED,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void status_led_update(int64_t now_us)
{
    const uint32_t max_duty = 1023;
    uint32_t duty;
    const piano_play_t play = piano_get_play();

    if (piano_is_recording()) {
        const uint32_t phase_ms = (uint32_t)((now_us / 1000) % 200);
        duty = (phase_ms < 100) ? (uint32_t)(max_duty * 0.9f) : 0;
    } else if (play == PIANO_PLAY_PLAYING) {
        const uint32_t phase_ms = (uint32_t)((now_us / 1000) % 500);
        duty = (phase_ms < 250) ? (uint32_t)(max_duty * 0.6f) : 0;
    } else {
        duty = max_duty / 3;
    }

    /*
     * 状态灯也受"主目录旋钮亮度"控制（与 WS2812 D1~D5 同源）：
     * 按 s_brightness/255 缩放占空比；亮度>0 时保底 1，避免完全看不见。
     */
    duty = (uint32_t)(((uint64_t)duty * (uint64_t)s_brightness) / 255u);
    if (duty == 0 && s_brightness > 0) {
        duty = 1;
    }
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

/* 开机曲库：内置 3 首儿歌（供"试听"直接可用） */
static void seed_builtin_songs(void)
{
    static const uint8_t k_twinkle[] = {
        0, 0, 4, 4, 5, 5, 4, 3, 3, 2, 2, 1, 1, 0,
        4, 4, 3, 3, 2, 2, 1, 4, 4, 3, 3, 2, 2, 1,
        0, 0, 4, 4, 5, 5, 4, 3, 3, 2, 2, 1, 1, 0};
    static const uint8_t k_tigers[] = {
        1, 2, 3, 1, 1, 2, 3, 1, 3, 4, 5, 3, 4, 5,
        5, 6, 5, 4, 3, 1, 5, 6, 5, 4, 3, 1, 1, 5, 1, 1, 5, 1};
    static const uint8_t k_joy[] = {3, 3, 4, 5, 5, 4, 3, 2, 1, 1, 2, 3, 3, 2, 2};
    static const struct {
        const char *name;
        const uint8_t *notes;
        int count;
    } builtins[] = {
        {"小星星", k_twinkle, (int)(sizeof(k_twinkle) / sizeof(k_twinkle[0]))},
        {"两只老虎", k_tigers, (int)(sizeof(k_tigers) / sizeof(k_tigers[0]))},
        {"欢乐颂", k_joy, (int)(sizeof(k_joy) / sizeof(k_joy[0]))},
    };

    static song_ev_t ev[64];
    for (int s = 0; s < 3; ++s) {
        int n = builtins[s].count;
        if (n > 64) {
            n = 64;
        }
        for (int i = 0; i < n; ++i) {
            ev[i].note = builtins[s].notes[i];
            ev[i].delta_ms = (i == 0) ? 0 : 400; /* 内置曲无节奏信息，统一 400ms */
        }
        if (songpool_set(s, builtins[s].name, ev, n) >= 0) {
            (void)songpool_flash_save(s); /* 首启：内置曲一并落 flash，之后走 flash 加载 */
        }
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(board_keys_init());
    ESP_ERROR_CHECK(board_power_enable_peripherals());
    ESP_ERROR_CHECK(pattern_init());
    ESP_ERROR_CHECK(host_link_init());

    const host_link_handlers_t handlers = {
        .on_transport = on_host_transport,
        .on_bpm = on_host_bpm,
        .on_swing = on_host_swing,
        .on_variation = on_host_variation,
        .on_fill = on_host_fill,
        .on_note = on_host_note,
        .on_click = on_host_click,
        .on_mode = on_host_mode,
        .on_volume = on_host_volume,
        .on_brightness = on_host_brightness,
        .on_pattern_set = on_host_pattern_set,
        .on_save = on_host_save,
        .on_ping = on_host_ping,
        .on_record = on_host_record,
        .on_playback = on_host_playback,
        .on_song = on_host_song,
        .on_demo = on_host_demo,
        .on_voice_begin = on_host_voice_begin,
        .on_voice_data = on_host_voice_data,
        .on_voice_end = on_host_voice_end,
        .on_voice_replay = on_host_voice_replay,
        .on_voice_default = on_host_voice_default,
        .on_rec_save = on_host_rec_save,
        .on_rec_load = on_host_rec_load,
        .on_rec_del = on_host_rec_del,
        .on_rec_list = on_host_rec_list,
        .on_rec_play = on_host_rec_play,
        .on_song_add = on_host_song_add,
        .on_song_del = on_host_song_del,
        .on_song_list = on_host_song_list,
        .on_song_play = on_host_song_play,
#if BAYIN_ENABLE_DIAG
        .on_rec_inject = on_host_rec_inject,
        .on_pb_play = on_host_pb_play,
#endif
    };
    host_link_set_handlers(&handlers);

    ESP_ERROR_CHECK(led_status_init());
    status_led_init();
    gesture_init(&s_enc_gesture);
    recorder_init();
    piano_init();          /* 开机即在主目录 */
    songpool_init();
    playpool_init(); /* 回放池：5 槽，持久化在 rec 槽 21~25（满 5 FIFO 覆盖最旧） */
    /* 回放池：开机从 flash 重建 5 槽（按 seq 恢复 FIFO 顺序） */
    RLOG("boot", "playpool loaded %d slots (pb=%d)", playpool_load_from_flash(),
         playpool_used_count());
    /* 曲库：优先从 flash 加载（网页 song_add 会持久化）；flash 为空时用内置 3 首儿歌播种 */
    (void)songpool_load_from_flash();
    if (songpool_used_count() == 0) {
        seed_builtin_songs();
    }

    const esp_err_t audio_err = audio_click_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed (%s); LEDs/keys still run", esp_err_to_name(audio_err));
    } else {
        s_audio_ready = true;
    }
    prompt_init(); /* 语音提示：从 rec 分区 slot5+ 读 VPMT 资源（缺资源则静默禁用） */
#if BAYIN_ENABLE_DIAG
    audio_click_set_note_log_cb(snd_note_log_cb); /* 台架自测：音符采样起播 → snd 事件 */
#endif

    uint8_t saved_volume = 100;
    uint8_t saved_brightness = 80;
    nvs_handle_t nvs_handle;
    if (nvs_open("piano", NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs_handle, "volume", &v) == ESP_OK) {
            saved_volume = v;
        }
        if (nvs_get_u8(nvs_handle, "brightness", &v) == ESP_OK) {
            saved_brightness = v;
        }
        nvs_close(nvs_handle);
    }
    if (s_audio_ready) {
        (void)audio_click_set_volume(saved_volume);
    }
    s_brightness = enc_brightness_clamp(saved_brightness); /* 掉电记忆：沿用上次亮度 */
    enc_flush_init(&s_bri_flush);
    enc_flush_init(&s_vol_beep);
    feedback_gate_init(&s_drop_gate);
    s_drop_flash_until_us = 0;
    led_status_set_brightness(s_brightness);

    ESP_ERROR_CHECK(tempo_init(120));
    if (s_audio_ready) {
        ESP_ERROR_CHECK(audio_click_set_bpm(tempo_get_bpm()));
        ESP_ERROR_CHECK(audio_click_set_metronome(pattern_click_enabled()));
        ESP_ERROR_CHECK(audio_click_set_mode(AUDIO_MODE_METRONOME));
    }

    ESP_ERROR_CHECK(led_status_clear());
    vTaskDelay(pdMS_TO_TICKS(300));

    voice_store_init();
    /* ★ D1：删掉旧欢迎语，避免与"请选择模式"叠播（语音二选一 → 保留 prompt） */

    host_link_send_hello();
    host_link_send_pattern_dump();
    refresh_leds();
    send_status();
    /* 开机"请选择模式"由 piano_init() 登记，经主循环闩锁播放一次 */
    ESP_LOGI(TAG, "Ready. 主目录 S1~S4 选模式; 试听/回放 S1~S5; 自弹 S1~S8=8音; "
                  "旋钮=音量/短按上下文/长按5s回主目录");

    int64_t last_status_us = 0;
    int64_t last_hello_us = 0;
    int64_t last_led_frame_us = 0;

    while (true) {
        board_input_snapshot_t in = {0};
        ESP_ERROR_CHECK(board_keys_poll(&in));
        const int64_t now = esp_timer_get_time();

        /* 旋钮始终可用（主目录=亮度 / 其它=音量 / 短按上下文 / 长按≥5s回主目录） */
        apply_encoder_dial(in.enc_delta);
        /* 亮度脏标记：停转 ≥1.5s 一次性落盘（绝不每格写 NVS） */
        if (enc_flush_poll(&s_bri_flush, (uint32_t)(now / 1000), ENC_BRIGHTNESS_SETTLE_MS)) {
            persist_u8("brightness", s_brightness);
            RLOG("enc", "brightness flushed (settle) = %d", s_brightness);
        }
        /* 音量反馈音：停转 ~300ms 后恰好一声（节流；不碰 LED/s_last_play_note） */
        (void)enc_beep_poll(&s_vol_beep, (uint32_t)(now / 1000), ENC_VOLUME_BEEP_SETTLE_MS,
                            volume_beep_emit);
        handle_encoder_gesture(&in, now);
        /* Pad：语音播报期间丢弃（busy 由音频任务刷新） */
        handle_pads(&in, now, audio_click_pcm_busy());
        playback_tick(now);

        /* ★ 语音闩锁：取走待播句（一次性），交给 prompt_play 播放 */
        const int voice_id = piano_take_voice();
        if (voice_id >= 0) {
            RLOG("voice", "play id=%d '%s'", voice_id, prompt_text(voice_id));
            (void)prompt_play(voice_id);
        }

        host_link_poll_rx();
#if BAYIN_ENABLE_DIAG
        drain_snd_log(); /* 台架自测：把音频层 snd 事件落成 JSON 日志 */
#endif

        audio_beat_event_t beat_event;
        while (s_audio_ready && audio_click_poll_beat(&beat_event)) {
            render_event(&beat_event, now);
        }

        if (now - last_led_frame_us >= 20000) {
            last_led_frame_us = now;
            status_led_update(now);
            refresh_leds();
        }

        if (now - last_status_us > 500000) {
            last_status_us = now;
            if (s_audio_ready && tempo_is_running()) {
                audio_click_get_position(&s_last_bar, &s_last_step, &s_last_beat_in_bar,
                                         &s_last_tick);
            }
            send_status();
        }
        if (now - last_hello_us > 5000000) {
            last_hello_us = now;
            host_link_send_hello();
        }

        vTaskDelay(1);
    }
}
