#include "board_power.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_power";
static bool s_enabled = false;

static esp_err_t latch_command_outputs_low(void)
{
    const gpio_num_t outs[] = {
        BOARD_GPIO_MIC_BCLK,
        BOARD_GPIO_MIC_WS,
        BOARD_GPIO_LED_DIN,
        BOARD_GPIO_SPK_WS,
        BOARD_GPIO_SPK_BCLK,
        BOARD_GPIO_SPK_DOUT,
        BOARD_GPIO_PWR_EN,
    };

    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); ++i) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << outs[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config %d", outs[i]);
        ESP_RETURN_ON_ERROR(gpio_set_level(outs[i], 0), TAG, "gpio_set_level %d", outs[i]);
    }

    /* MIC data is an input; keep floating during rail transitions. */
    gpio_config_t mic_din = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_MIC_DIN,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&mic_din);
}

esp_err_t board_power_enable_peripherals(void)
{
    if (s_enabled) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(latch_command_outputs_low(), TAG, "safe latch failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_GPIO_PWR_EN, 1), TAG, "PWR_EN high failed");
    vTaskDelay(pdMS_TO_TICKS(BOARD_PWR_SETTLE_MS));
    s_enabled = true;
    ESP_LOGI(TAG, "GPIO8 peripheral rail enabled (settle=%d ms)", BOARD_PWR_SETTLE_MS);
    return ESP_OK;
}

esp_err_t board_power_disable_peripherals(void)
{
    if (!s_enabled) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(latch_command_outputs_low(), TAG, "safe latch failed");
    s_enabled = false;
    ESP_LOGI(TAG, "GPIO8 peripheral rail disabled");
    return ESP_OK;
}

bool board_power_peripherals_enabled(void)
{
    return s_enabled;
}
