#include <assert.h>
#include <stdint.h>

#include "clock.h"

static void test_clamp_bpm(void)
{
    assert(beatbox_clamp_bpm(30) == BEATBOX_BPM_MIN);
    assert(beatbox_clamp_bpm(500) == BEATBOX_BPM_MAX);
    assert(beatbox_clamp_bpm(120) == 120);
}

static void test_clamp_swing(void)
{
    assert(beatbox_clamp_swing(0) == BEATBOX_SWING_MIN);
    assert(beatbox_clamp_swing(100) == BEATBOX_SWING_MAX);
    assert(beatbox_clamp_swing(60) == 60);
}

static void test_swing_delay_ticks(void)
{
    /* 50% -> 0, 75% -> 12 ticks, linear in between. */
    assert(beatbox_swing_delay_ticks(50) == 0);
    assert(beatbox_swing_delay_ticks(75) == 12);
    assert(beatbox_swing_delay_ticks(62) == 5); /* (62-50)*12/25 = 5 */
}

static void test_track_note(void)
{
    assert(beatbox_track_note(0) == BEATBOX_NOTE_KICK);
    assert(beatbox_track_note(1) == BEATBOX_NOTE_SNARE);
    assert(beatbox_track_note(2) == BEATBOX_NOTE_CHH);
    assert(beatbox_track_note(3) == BEATBOX_NOTE_OHH);
    assert(beatbox_track_note(4) == BEATBOX_NOTE_CLAP);
    assert(beatbox_track_note(5) == BEATBOX_NOTE_RIM);
    assert(beatbox_track_note(6) == 0); /* out of range */
}

static void test_note_track(void)
{
    assert(beatbox_note_track(BEATBOX_NOTE_KICK) == 0);
    assert(beatbox_note_track(BEATBOX_NOTE_SNARE) == 1);
    assert(beatbox_note_track(BEATBOX_NOTE_RIM) == 5);
    assert(beatbox_note_track(99) == -1); /* unknown note */
}

int main(void)
{
    test_clamp_bpm();
    test_clamp_swing();
    test_swing_delay_ticks();
    test_track_note();
    test_note_track();
    return 0;
}
