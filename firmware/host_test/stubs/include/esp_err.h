#pragma once

#include <stdint.h>

/* 宿主测试用的极简 esp_err 替身：真实固件里是枚举，这里只关心“成功/失败”。 */
typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x1000
#define ESP_ERR_INVALID_STATE 0x106
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
