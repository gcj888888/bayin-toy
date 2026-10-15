#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 宿主测试用的 esp_partition 替身：只保留 voice_store.c 用到的字段与函数原型。 */
typedef struct {
    uint32_t address;
    uint32_t size;
    uint8_t type;
    uint8_t subtype;
    const char *label;
} esp_partition_t;

#define ESP_PARTITION_TYPE_DATA 0x40
#define ESP_PARTITION_SUBTYPE_ANY 0xFF

const esp_partition_t *esp_partition_find_first(uint32_t type, uint32_t subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, uint32_t src_offset, void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, uint32_t dst_offset, const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, uint32_t start_addr, uint32_t size);
