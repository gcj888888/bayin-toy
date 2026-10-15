#pragma once

/* 宿主测试用的 esp_log 替身：直接变成空操作，避免把固件日志引到 PC 上。 */
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#define esp_err_to_name(e) ("")
