#include "gesture.h"

#include <assert.h>
#include <stdio.h>

/*
 * 旋钮"短按 / 长按"手势状态机的宿主测试。
 * 这是电子琴控制逻辑里最容易出 bug 的部分（短按长按会不会误判、会不会重复触发），
 * 必须先在电脑上测过，再烧录。
 */

static void test_short_press(void)
{
    gesture_state_t st;
    gesture_init(&st);

    assert(gesture_update(&st, true, true, 0, 600) == GESTURE_ACTION_NONE);   /* 按下 */
    assert(gesture_update(&st, false, true, 300, 600) == GESTURE_ACTION_NONE); /* 按住未达阈值 */
    assert(gesture_update(&st, false, false, 400, 600) == GESTURE_ACTION_SHORT_PRESS); /* 松手 */
    assert(gesture_update(&st, false, false, 500, 600) == GESTURE_ACTION_NONE); /* 已空闲 */
}

static void test_long_press(void)
{
    gesture_state_t st;
    gesture_init(&st);

    assert(gesture_update(&st, true, true, 0, 600) == GESTURE_ACTION_NONE);   /* 按下 */
    assert(gesture_update(&st, false, true, 600, 600) == GESTURE_ACTION_LONG_PRESS); /* 达阈值 */
    assert(gesture_update(&st, false, true, 700, 600) == GESTURE_ACTION_NONE); /* 不重复触发 */
    assert(gesture_update(&st, false, false, 800, 600) == GESTURE_ACTION_NONE); /* 松手忽略，不误判短按 */
    assert(gesture_update(&st, false, false, 900, 600) == GESTURE_ACTION_NONE); /* 已空闲 */
}

static void test_threshold_exact(void)
{
    gesture_state_t st;
    gesture_init(&st);
    gesture_update(&st, true, true, 1000, 600);
    /* 正好按住 600ms → 长按 */
    assert(gesture_update(&st, false, true, 1600, 600) == GESTURE_ACTION_LONG_PRESS);
}

static void test_repeat_cycle(void)
{
    gesture_state_t st;
    gesture_init(&st);

    /* 第一次：短按 */
    gesture_update(&st, true, true, 0, 600);
    assert(gesture_update(&st, false, false, 200, 600) == GESTURE_ACTION_SHORT_PRESS);

    /* 第二次：长按 */
    gesture_update(&st, true, true, 1000, 600);
    assert(gesture_update(&st, false, true, 1600, 600) == GESTURE_ACTION_LONG_PRESS);
    gesture_update(&st, false, false, 1800, 600);

    /* 第三次：又是短按（状态机要能复位后正常工作） */
    gesture_update(&st, true, true, 2000, 600);
    assert(gesture_update(&st, false, false, 2100, 600) == GESTURE_ACTION_SHORT_PRESS);
}

static void test_long_press_5s(void)
{
    /* 八音琴：长按旋钮 ≥5000ms = 回主目录（终极按钮）。
     * 阈值 5000ms 下，按下 4999ms 不应触发，5000ms 才触发。 */
    gesture_state_t st;
    gesture_init(&st);

    assert(gesture_update(&st, true, true, 0, 5000) == GESTURE_ACTION_NONE);      /* 按下 */
    assert(gesture_update(&st, false, true, 4999, 5000) == GESTURE_ACTION_NONE);  /* 差 1ms 不触发 */
    assert(gesture_update(&st, false, true, 5000, 5000) == GESTURE_ACTION_LONG_PRESS); /* 正好 5s */
    assert(gesture_update(&st, false, true, 6000, 5000) == GESTURE_ACTION_NONE);  /* 不重复 */
    assert(gesture_update(&st, false, false, 7000, 5000) == GESTURE_ACTION_NONE); /* 松手忽略 */
}

int main(void)
{
    test_short_press();
    test_long_press();
    test_threshold_exact();
    test_repeat_cycle();
    test_long_press_5s();

    printf("gesture_tests: all passed (short/long press, no double-trigger, repeat, 5s)\n");
    return 0;
}
