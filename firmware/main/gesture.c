#include "gesture.h"

void gesture_init(gesture_state_t *st)
{
    st->phase = GESTURE_PHASE_IDLE;
    st->down_ms = 0;
}

gesture_action_t gesture_update(gesture_state_t *st, bool pressed_edge, bool held,
                                uint32_t now_ms, uint32_t long_ms)
{
    if (pressed_edge) {
        st->phase = GESTURE_PHASE_PENDING;
        st->down_ms = now_ms;
        return GESTURE_ACTION_NONE;
    }

    switch (st->phase) {
    case GESTURE_PHASE_IDLE:
        return GESTURE_ACTION_NONE;

    case GESTURE_PHASE_PENDING:
        if (held && (now_ms - st->down_ms) >= long_ms) {
            /* 按住够久 → 触发长按，进入"已触发"，之后松手忽略 */
            st->phase = GESTURE_PHASE_TRIGGERED;
            return GESTURE_ACTION_LONG_PRESS;
        }
        if (!held) {
            /* 提前松手 → 短按 */
            st->phase = GESTURE_PHASE_IDLE;
            return GESTURE_ACTION_SHORT_PRESS;
        }
        return GESTURE_ACTION_NONE;

    case GESTURE_PHASE_TRIGGERED:
        if (!held) {
            /* 长按已触发过，这次松手要忽略，不能又当成短按 */
            st->phase = GESTURE_PHASE_IDLE;
        }
        return GESTURE_ACTION_NONE;
    }

    return GESTURE_ACTION_NONE;
}
