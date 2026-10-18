#include "host_link.h"

#include "clock.h"
#include "diag_config.h"
#include "json_field.h"
#include "pattern.h"

#include "esp_log.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "host";

static host_link_handlers_t s_handlers;
static bool s_ready;
static char s_rx_buf[768];
static size_t s_rx_len;
static int s_stdin_flags_set;

/* 额外的输出落点（BLE)：每写一行 JSON，除 USB 外再回调一次 */
static host_line_sink_fn s_sink;

static void host_write(const char *line)
{
    if (!s_ready || line == NULL) {
        return;
    }
    printf("%s\n", line);
    fflush(stdout);
    if (s_sink) {
        s_sink(line);
    }
}

esp_err_t host_link_init(void)
{
    /* Install the USB-Serial-JTAG driver so RX actually receives bytes sent
     * from the host. Without it, the console VFS runs in "no_driver" mode and
     * inbound host commands are never read (PC-side writes time out). */
    usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usj_config);
    usb_serial_jtag_vfs_use_driver();

    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        s_stdin_flags_set = 1;
    }

    s_ready = true;
    s_rx_len = 0;
    ESP_LOGI(TAG, "USB Serial host link ready (protocol v2)");
    host_link_send_hello();
    return ESP_OK;
}

bool host_link_ready(void)
{
    return s_ready;
}

void host_link_send_hello(void)
{
    host_write(
        "{\"t\":\"hello\",\"v\":2,\"name\":\"EasyInput Piano\",\"caps\":[\"drum\",\"pattern\","
        "\"swing\",\"volume\",\"brightness\",\"record\",\"play\",\"learn\",\"song\",\"voice\",\"demo\"]}");
}

void host_link_send_start(void)
{
    host_write("{\"t\":\"start\"}");
}

void host_link_send_continue(void)
{
    host_write("{\"t\":\"continue\"}");
}

void host_link_send_stop(void)
{
    host_write("{\"t\":\"stop\"}");
}

void host_link_send_beat(bool accent, uint8_t beat_in_bar, uint8_t step)
{
    char line[128];
    snprintf(line, sizeof(line),
             "{\"t\":\"beat\",\"accent\":%d,\"beat\":%u,\"step\":%u}", accent ? 1 : 0,
             (unsigned)(beat_in_bar & 0x03), (unsigned)(step & 0x0f));
    host_write(line);
}

void host_link_send_position(uint32_t bar, uint8_t step, uint8_t beat, uint16_t tick,
                             bool accent)
{
    char line[160];
    snprintf(line, sizeof(line),
             "{\"t\":\"position\",\"bar\":%lu,\"step\":%u,\"beat\":%u,\"tick\":%u,\"accent\":%d}",
             (unsigned long)bar, (unsigned)(step & 0x0f), (unsigned)(beat & 0x03),
             (unsigned)tick, accent ? 1 : 0);
    host_write(line);
}

void host_link_send_note(uint8_t note, uint8_t velocity)
{
    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"note\",\"n\":%u,\"v\":%u}", (unsigned)note,
             (unsigned)velocity);
    host_write(line);
}

void host_link_send_key(uint8_t index, bool down)
{
    char line[72];
    snprintf(line, sizeof(line), "{\"t\":\"key\",\"i\":%u,\"v\":%d}", (unsigned)(index & 0x07),
             down ? 1 : 0);
    host_write(line);
}

void host_link_send_status(uint16_t bpm, bool running, uint8_t beat_in_bar, uint8_t step,
                           uint32_t bar, uint16_t tick, bool drum_mode, uint8_t volume,
                           uint8_t brightness, uint8_t playback_count)
{
    char line[304];
    snprintf(line, sizeof(line),
             "{\"t\":\"state\",\"bpm\":%u,\"run\":%d,\"beat\":%u,\"step\":%u,\"bar\":%lu,"
             "\"tick\":%u,\"swing\":%u,\"var\":%u,\"fill\":%d,\"rev\":%lu,\"click\":%d,"
             "\"mode\":%u,\"vol\":%u,\"bri\":%u,\"pb\":%u}",
             (unsigned)bpm, running ? 1 : 0, (unsigned)(beat_in_bar & 0x03),
             (unsigned)(step & 0x0f), (unsigned long)bar, (unsigned)tick,
             (unsigned)pattern_swing(), (unsigned)pattern_variation(),
             pattern_fill_active() ? 1 : 0, (unsigned long)pattern_revision(),
             pattern_click_enabled() ? 1 : 0, drum_mode ? 1u : 0u,
             (unsigned)(volume > BEATBOX_VOLUME_MAX ? BEATBOX_VOLUME_MAX : volume),
             (unsigned)brightness, (unsigned)playback_count);
    host_write(line);
}

void host_link_send_pattern_dump(void)
{
    /* One bank per line — long composite dumps get truncated on USB-Serial/JTAG. */
    char hex[BEATBOX_PATTERN_HEX_CHARS + 1];
    char line[280];
    for (uint8_t bank = 0; bank < BEATBOX_BANK_COUNT; ++bank) {
        pattern_encode_hex(pattern_bank(bank), hex, sizeof(hex));
        snprintf(line, sizeof(line),
                 "{\"t\":\"pattern\",\"bank\":%u,\"rev\":%lu,\"p\":\"%s\"}", (unsigned)bank,
                 (unsigned long)pattern_revision(), hex);
        host_write(line);
    }
}

void host_link_send_ack(const char *cmd, bool ok, uint32_t rev)
{
    char line[128];
    snprintf(line, sizeof(line), "{\"t\":\"ack\",\"cmd\":\"%s\",\"ok\":%d,\"rev\":%lu}",
             cmd ? cmd : "", ok ? 1 : 0, (unsigned long)rev);
    host_write(line);
}

void host_link_send_error(const char *cmd, const char *msg)
{
    char line[192];
    snprintf(line, sizeof(line), "{\"t\":\"error\",\"cmd\":\"%s\",\"msg\":\"%s\"}",
             cmd ? cmd : "", msg ? msg : "");
    host_write(line);
}

void host_link_send_line(const char *line)
{
    host_write(line);
}

void host_link_send_log(const char *tag, const char *msg)
{
    /* 注意：tag/msg 不含引号/反斜杠（本工程日志内容如此），无需转义。
     * t 值 "log" 与现有 hello/state/beat/position/note/key/pattern/ack/error/rec_list
     * 等不冲突；song_list 用的是 "type" 键，也不冲突。 */
    char line[192];
    snprintf(line, sizeof(line), "{\"t\":\"log\",\"tag\":\"%s\",\"msg\":\"%s\"}",
             tag ? tag : "", msg ? msg : "");
    host_write(line);
}

void host_link_set_handlers(const host_link_handlers_t *handlers)
{
    if (handlers) {
        s_handlers = *handlers;
    } else {
        memset(&s_handlers, 0, sizeof(s_handlers));
    }
}

void host_link_set_sink(host_line_sink_fn fn)
{
    s_sink = fn;
}

/* 按 '\n' 切行 → 交给 host_link_handle_line。USB 与 BLE 共用这一套切行逻辑。 */
static void rx_push(const char *data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (s_rx_len > 0) {
                s_rx_buf[s_rx_len] = '\0';
                host_link_handle_line(s_rx_buf);
                s_rx_len = 0;
            }
            continue;
        }
        if (s_rx_len + 1 < sizeof(s_rx_buf)) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            s_rx_len = 0; /* 超长行：丢弃，避免污染后续解析 */
        }
    }
}

void host_link_feed_rx(const char *data, size_t n)
{
    if (data == NULL || n == 0) {
        return;
    }
    rx_push(data, n);
}

void host_link_handle_line(const char *line)
{
    char type[32];
    if (!extract_quoted_type(line, type, sizeof(type))) {
        return;
    }

    if (strcmp(type, "start") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(true, true);
        }
        return;
    }
    if (strcmp(type, "continue") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(true, false);
        }
        return;
    }
    if (strcmp(type, "stop") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(false, false);
        }
        return;
    }
    if (strcmp(type, "ping") == 0) {
        host_link_send_hello();
        if (s_handlers.on_ping) {
            s_handlers.on_ping();
        } else {
            host_link_send_pattern_dump();
        }
        return;
    }
    if (strcmp(type, "bpm") == 0 && s_handlers.on_bpm) {
        long bpm = 120;
        if (extract_int_field(line, "v", &bpm)) {
            s_handlers.on_bpm((uint16_t)bpm);
        }
        return;
    }
    if (strcmp(type, "swing") == 0 && s_handlers.on_swing) {
        long swing = 50;
        if (extract_int_field(line, "v", &swing)) {
            s_handlers.on_swing((uint8_t)swing);
        }
        return;
    }
    if (strcmp(type, "variation") == 0 && s_handlers.on_variation) {
        long var = 0;
        if (extract_int_field(line, "v", &var)) {
            s_handlers.on_variation((uint8_t)var);
        }
        return;
    }
    if (strcmp(type, "fill") == 0 && s_handlers.on_fill) {
        long fill = 0;
        if (extract_int_field(line, "v", &fill)) {
            s_handlers.on_fill(fill != 0);
        }
        return;
    }
    if (strcmp(type, "click") == 0 && s_handlers.on_click) {
        long click = 1;
        if (extract_int_field(line, "v", &click)) {
            s_handlers.on_click(click != 0);
        }
        return;
    }
    if (strcmp(type, "mode") == 0 && s_handlers.on_mode) {
        long mode = 0;
        if (extract_int_field(line, "v", &mode)) {
            s_handlers.on_mode(mode != 0);
        }
        return;
    }
    if (strcmp(type, "volume") == 0 && s_handlers.on_volume) {
        long volume = 100;
        if (extract_int_field(line, "v", &volume)) {
            if (volume < 0) {
                volume = 0;
            }
            if (volume > BEATBOX_VOLUME_MAX) {
                volume = BEATBOX_VOLUME_MAX;
            }
            s_handlers.on_volume((uint8_t)volume);
        }
        return;
    }
    if (strcmp(type, "brightness") == 0 && s_handlers.on_brightness) {
        long brightness = 185;
        if (extract_int_field(line, "v", &brightness)) {
            if (brightness < 0) {
                brightness = 0;
            }
            if (brightness > 255) {
                brightness = 255;
            }
            s_handlers.on_brightness((uint8_t)brightness);
        }
        return;
    }
    if (strcmp(type, "note") == 0 && s_handlers.on_note) {
        long n = 0;
        long v = 127;
        if (extract_int_field(line, "n", &n)) {
            (void)extract_int_field(line, "v", &v);
            s_handlers.on_note((uint8_t)n, (uint8_t)v);
        }
        return;
    }
    if (strcmp(type, "pattern_get") == 0) {
        host_link_send_pattern_dump();
        return;
    }
    if (strcmp(type, "pattern_set") == 0 && s_handlers.on_pattern_set) {
        long bank = 0;
        long rev = 0;
        char hex[BEATBOX_PATTERN_HEX_CHARS + 1];
        if (!extract_int_field(line, "bank", &bank) || !extract_int_field(line, "rev", &rev) ||
            !extract_quoted_field(line, "p", hex, sizeof(hex))) {
            host_link_send_error("pattern_set", "bad_args");
            return;
        }
        uint8_t bytes[BEATBOX_PATTERN_BYTES];
        if (pattern_decode_hex(hex, bytes) != ESP_OK) {
            host_link_send_error("pattern_set", "bad_hex");
            return;
        }
        s_handlers.on_pattern_set((uint8_t)bank, (uint32_t)rev, bytes);
        return;
    }
    if (strcmp(type, "save") == 0 && s_handlers.on_save) {
        s_handlers.on_save();
        return;
    }
    if (strcmp(type, "record") == 0 && s_handlers.on_record) {
        long armed = 0;
        if (extract_int_field(line, "v", &armed)) {
            s_handlers.on_record(armed != 0);
        }
        return;
    }
    if (strcmp(type, "play") == 0 && s_handlers.on_playback) {
        s_handlers.on_playback();
        return;
    }
    if (strcmp(type, "song") == 0 && s_handlers.on_song) {
        long idx = 0;
        if (extract_int_field(line, "v", &idx)) {
            s_handlers.on_song((int)idx);
        }
        return;
    }
    if (strcmp(type, "demo") == 0 && s_handlers.on_demo) {
        s_handlers.on_demo();
        return;
    }
    if (strcmp(type, "voice_begin") == 0 && s_handlers.on_voice_begin) {
        long total = 0;
        if (extract_int_field(line, "n", &total)) {
            s_handlers.on_voice_begin((uint32_t)total);
        }
        return;
    }
    if (strcmp(type, "voice_data") == 0 && s_handlers.on_voice_data) {
        char b64[1024];
        if (extract_quoted_field(line, "d", b64, sizeof(b64))) {
            s_handlers.on_voice_data(b64);
        }
        return;
    }
    if (strcmp(type, "voice_end") == 0 && s_handlers.on_voice_end) {
        s_handlers.on_voice_end();
        return;
    }
    if (strcmp(type, "voice_replay") == 0 && s_handlers.on_voice_replay) {
        s_handlers.on_voice_replay();
        return;
    }
    if (strcmp(type, "voice_default") == 0 && s_handlers.on_voice_default) {
        s_handlers.on_voice_default();
        return;
    }
    if (strcmp(type, "rec_save") == 0 && s_handlers.on_rec_save) {
        long s = 0;
        if (extract_int_field(line, "s", &s)) {
            s_handlers.on_rec_save((uint8_t)s);
        }
        return;
    }
    if (strcmp(type, "rec_load") == 0 && s_handlers.on_rec_load) {
        long s = 0;
        if (extract_int_field(line, "s", &s)) {
            s_handlers.on_rec_load((uint8_t)s);
        }
        return;
    }
    if (strcmp(type, "rec_del") == 0 && s_handlers.on_rec_del) {
        long s = 0;
        if (extract_int_field(line, "s", &s)) {
            s_handlers.on_rec_del((uint8_t)s);
        }
        return;
    }
    if (strcmp(type, "rec_list") == 0 && s_handlers.on_rec_list) {
        s_handlers.on_rec_list();
        return;
    }
    if (strcmp(type, "rec_play") == 0 && s_handlers.on_rec_play) {
        long s = 0;
        if (extract_int_field(line, "s", &s)) {
            s_handlers.on_rec_play((uint8_t)s);
        }
        return;
    }
    /* ---- 八音琴曲库（网页发 {"v":2,"cmd":"song_*",...}）---- */
    if (strcmp(type, "song_add") == 0 && s_handlers.on_song_add) {
        long slot = 0;
        if (!extract_int_field(line, "slot", &slot)) {
            host_link_send_error("song_add", "bad_slot");
            return;
        }
        char name[64];
        if (!extract_quoted_field(line, "name", name, sizeof(name))) {
            name[0] = '\0';
        }
        static song_ev_t ev[SONGP_MAX_EVENTS];
        const int count = extract_notes_array(line, ev, SONGP_MAX_EVENTS);
        s_handlers.on_song_add((uint8_t)slot, name, ev, count);
        return;
    }
    if (strcmp(type, "song_del") == 0 && s_handlers.on_song_del) {
        long slot = 0;
        if (extract_int_field(line, "slot", &slot)) {
            s_handlers.on_song_del((uint8_t)slot);
        }
        return;
    }
    if (strcmp(type, "song_list") == 0 && s_handlers.on_song_list) {
        s_handlers.on_song_list();
        return;
    }
    if (strcmp(type, "song_play") == 0 && s_handlers.on_song_play) {
        long slot = 0;
        if (extract_int_field(line, "slot", &slot)) {
            s_handlers.on_song_play((uint8_t)slot);
        }
        return;
    }
#if BAYIN_ENABLE_DIAG
    /* ---- 台架自测诊断命令（关 BAYIN_ENABLE_DIAG 即整体移除）---- */
    if (strcmp(type, "rec_inject") == 0 && s_handlers.on_rec_inject) {
        static song_ev_t ev[SONGP_MAX_EVENTS];
        const int count = extract_notes_array(line, ev, SONGP_MAX_EVENTS);
        s_handlers.on_rec_inject(ev, count);
        return;
    }
    if (strcmp(type, "pb_play") == 0 && s_handlers.on_pb_play) {
        long slot = 0;
        if (extract_int_field(line, "slot", &slot)) {
            s_handlers.on_pb_play((uint8_t)slot);
        }
        return;
    }
    if (strcmp(type, "pad_key") == 0 && s_handlers.on_pad_key) {
        /* 注入一路按键：{"t":"pad_key","i":0~7,"v":1|0}（与物理按键同路径） */
        long idx = -1;
        long down = 1;
        if (extract_int_field(line, "i", &idx) && extract_int_field(line, "v", &down) &&
            idx >= 0 && idx < 8) {
            s_handlers.on_pad_key((uint8_t)idx, down != 0);
        }
        return;
    }
#endif
}

void host_link_poll_rx(void)
{
    if (!s_ready) {
        return;
    }

    char chunk[128];
    while (true) {
        const ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        rx_push(chunk, (size_t)n);
    }

    (void)s_stdin_flags_set;
    (void)TAG;
}
