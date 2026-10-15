/* 宿主测试用的 pattern/clock 替身：host_link.c 的发送函数会引用这些符号，
 * 但本测试只测命令“解析+分发”（song/demo/voice_* 等），不调用发送函数，
 * 所以这里只提供能链接的空实现即可。 */
#include "pattern.h"

const pattern_bank_t *pattern_active_bank(void) { return NULL; }
const pattern_bank_t *pattern_bank(uint8_t bank) { (void)bank; return NULL; }

esp_err_t pattern_init(void) { return 0; }
uint32_t pattern_revision(void) { return 0; }
uint8_t pattern_swing(void) { return 50; }
uint8_t pattern_variation(void) { return 0; }
bool pattern_fill_active(void) { return false; }
bool pattern_click_enabled(void) { return false; }

void pattern_set_swing(uint8_t swing) { (void)swing; }
void pattern_set_click(bool enabled) { (void)enabled; }
void pattern_set_fill(bool held) { (void)held; }
void pattern_request_variation(uint8_t var) { (void)var; }
void pattern_poll_variation(uint16_t tick) { (void)tick; }

esp_err_t pattern_set_bank(uint8_t bank, uint32_t rev, const uint8_t *bytes)
{
    (void)bank; (void)rev; (void)bytes;
    return 0;
}

void pattern_encode_hex(const pattern_bank_t *bank, char *out, size_t out_len)
{
    (void)bank;
    if (out_len > 0) {
        out[0] = '\0';
    }
}

esp_err_t pattern_decode_hex(const char *hex, uint8_t *bytes)
{
    (void)hex; (void)bytes;
    return 0;
}

void pattern_load_default(void) {}
