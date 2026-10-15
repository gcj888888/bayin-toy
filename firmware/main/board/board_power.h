#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Prepare command outputs in a safe state, then enable GPIO8 shared rail.
 * Call before WS2812 or speaker I2S init.
 */
esp_err_t board_power_enable_peripherals(void);

/**
 * Stop shared consumers first at call sites, then disable the rail.
 */
esp_err_t board_power_disable_peripherals(void);

bool board_power_peripherals_enabled(void);

#ifdef __cplusplus
}
#endif
