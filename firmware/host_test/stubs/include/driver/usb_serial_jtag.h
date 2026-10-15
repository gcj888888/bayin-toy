#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 宿主测试用的 USB-Serial-JTAG 驱动替身：host_link.c 只在 host_link_init 里安装驱动，
 * 测试不调用 host_link_init，所以这些函数只需能编译/链接即可。 */
typedef struct {
    int dummy;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() ((usb_serial_jtag_driver_config_t){0})

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
