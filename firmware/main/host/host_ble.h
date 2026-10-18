#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 蓝牙（BLE）链路 —— Nordic UART Service 风格的 GATT 服务。
 *
 * 为什么要有它：网页端原本只能通过 USB 数据线（Web Serial）连板子，
 * 用户要求"不插数据线也能玩"。这里用 NimBLE 起一路 BLE 外设，
 * 收发依旧是**同一套换行 JSON 协议**（与 USB 完全一致）：
 *   - 电脑 → 板子：写 NUS RX 特征 → 入环形缓冲 → 主循环 host_ble_poll() 取走
 *   - 板子 → 电脑：host_link 的每一行输出 → host_ble_notify() → notify NUS TX 特征
 *
 * ★ 所有 BLE 收到的命令都**不在 BLE 回调里直接执行**，而是回主循环执行，
 *   这样与 USB 侧的"主任务上下文"保持一致，避免与 LED/音频/NVS 并发访问。
 */

/** 初始化 BLE 栈并开始广播（失败不影响 USB 链路，可继续用数据线）。 */
esp_err_t host_ble_init(void);

/** 主循环调用：把 BLE 收到的字节喂进 host_link 的换行 JSON 解析器。 */
void host_ble_poll(void);

/** host_link 的输出落点回调（由 host_link_set_sink 注册）。 */
void host_ble_notify(const char *line);

#ifdef __cplusplus
}
#endif
