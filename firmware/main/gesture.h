#pragma once

/*
 * 旋钮"短按 / 长按"手势状态机（纯逻辑，可宿主测试）。
 *
 * 照搬 easy-input-maker 的 EncoderPressGesture 思路：
 *   按下 → Pending；按住够久 → 触发长按（之后松手忽略）；
 *   提前松手 → 触发短按。两者不会重复触发。
 * 代价：短按必须等松手才生效——按下那一刻无法预知你要短按还是长按。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次 update 返回的动作 */
typedef enum {
    GESTURE_ACTION_NONE = 0,
    GESTURE_ACTION_SHORT_PRESS, /* 松手且未达长按阈值 */
    GESTURE_ACTION_LONG_PRESS,  /* 按住达到阈值 */
} gesture_action_t;

/* 状态机内部阶段 */
typedef enum {
    GESTURE_PHASE_IDLE = 0,
    GESTURE_PHASE_PENDING,   /* 已按下，等待判定长短 */
    GESTURE_PHASE_TRIGGERED, /* 长按已触发，松手时忽略 */
} gesture_phase_t;

typedef struct {
    gesture_phase_t phase;
    uint32_t down_ms;
} gesture_state_t;

/* 复位到空闲态 */
void gesture_init(gesture_state_t *st);

/*
 * 推进一次手势判定。
 * @param pressed_edge 本次是否出现"新按下"的上升沿
 * @param held         当前是否仍按住（已去抖）
 * @param now_ms       当前时间（毫秒）
 * @param long_ms      长按阈值（毫秒）
 * @return 触发的动作（NONE / SHORT_PRESS / LONG_PRESS）
 */
gesture_action_t gesture_update(gesture_state_t *st, bool pressed_edge, bool held,
                                uint32_t now_ms, uint32_t long_ms);

#ifdef __cplusplus
}
#endif
