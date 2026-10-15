#pragma once

#include <stdint.h>
#include <stdlib.h>

/* 宿主测试用的 heap_caps 替身：直接映射到普通 malloc/free。
 * 真实固件会优先用 PSRAM，但 PC 上普通堆即可验证导入逻辑。 */
#define MALLOC_CAP_SPIRAM 0x1
#define MALLOC_CAP_8BIT 0x2
#define MALLOC_CAP_DEFAULT 0x4

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}
