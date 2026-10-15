/* 宿主测试用的 USB-Serial-JTAG 驱动替身实现。
 * host_link.c 只在 host_link_init 里安装驱动，测试不调用 host_link_init，
 * 这里只需给出能链接的空实现，让 host_link_tests 严格链接（不靠忽略未定义符号）。 */
#include "esp_err.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{
    (void)config;
    return 0;
}

void usb_serial_jtag_vfs_use_driver(void) {}
