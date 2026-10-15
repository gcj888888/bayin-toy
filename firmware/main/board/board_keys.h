#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool s[8];
    bool enc_press;
    bool enc_held; /* 编码器当前是否按住（已去抖），电子琴用它判定长按 */
    int8_t enc_delta; /* +1 / -1 steps since last poll */
} board_input_snapshot_t;

esp_err_t board_keys_init(void);
esp_err_t board_keys_poll(board_input_snapshot_t *out);

#ifdef __cplusplus
}
#endif
