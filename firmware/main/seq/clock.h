#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Internal transport resolution derived from the I2S sample clock. */
#define BEATBOX_PPQN_INTERNAL 96
/** Standard MIDI beat clock. */
#define BEATBOX_PPQN_MIDI 24
/** Internal ticks per MIDI clock pulse. */
#define BEATBOX_TICKS_PER_MIDI_CLOCK 4
/** 16th note = one sequencer step. */
#define BEATBOX_TICKS_PER_16TH 24
/** Quarter note. */
#define BEATBOX_TICKS_PER_QUARTER 96
/** 4/4 bar. */
#define BEATBOX_BEATS_PER_BAR 4
#define BEATBOX_STEPS_PER_BAR 16
#define BEATBOX_TICKS_PER_BAR (BEATBOX_TICKS_PER_QUARTER * BEATBOX_BEATS_PER_BAR)

#define BEATBOX_TRACK_COUNT 6
#define BEATBOX_BANK_A 0
#define BEATBOX_BANK_B 1
#define BEATBOX_BANK_FILL 2
#define BEATBOX_BANK_COUNT 3
#define BEATBOX_PATTERN_BYTES (BEATBOX_TRACK_COUNT * BEATBOX_STEPS_PER_BAR)
#define BEATBOX_PATTERN_HEX_CHARS (BEATBOX_PATTERN_BYTES * 2)

#define BEATBOX_BPM_MIN 60
#define BEATBOX_BPM_MAX 240
#define BEATBOX_SWING_MIN 50
#define BEATBOX_SWING_MAX 75
#define BEATBOX_VOLUME_MAX 127

#define BEATBOX_NOTE_KICK 36
#define BEATBOX_NOTE_RIM 37
#define BEATBOX_NOTE_SNARE 38
#define BEATBOX_NOTE_CLAP 39
#define BEATBOX_NOTE_CHH 42
#define BEATBOX_NOTE_OHH 46
#define BEATBOX_NOTE_CLICK_ACCENT 76
#define BEATBOX_NOTE_CLICK_NORMAL 77

static inline uint16_t beatbox_clamp_bpm(uint16_t bpm)
{
    if (bpm < BEATBOX_BPM_MIN) {
        return BEATBOX_BPM_MIN;
    }
    if (bpm > BEATBOX_BPM_MAX) {
        return BEATBOX_BPM_MAX;
    }
    return bpm;
}

static inline uint8_t beatbox_clamp_swing(uint8_t swing)
{
    if (swing < BEATBOX_SWING_MIN) {
        return BEATBOX_SWING_MIN;
    }
    if (swing > BEATBOX_SWING_MAX) {
        return BEATBOX_SWING_MAX;
    }
    return swing;
}

/** Delay applied to odd 16th notes: 50% -> 0, 75% -> 12 ticks. */
static inline uint8_t beatbox_swing_delay_ticks(uint8_t swing)
{
    swing = beatbox_clamp_swing(swing);
    return (uint8_t)(((uint16_t)(swing - BEATBOX_SWING_MIN) * 12u) / 25u);
}

static inline uint8_t beatbox_track_note(uint8_t track)
{
    static const uint8_t notes[BEATBOX_TRACK_COUNT] = {
        BEATBOX_NOTE_KICK,
        BEATBOX_NOTE_SNARE,
        BEATBOX_NOTE_CHH,
        BEATBOX_NOTE_OHH,
        BEATBOX_NOTE_CLAP,
        BEATBOX_NOTE_RIM,
    };
    if (track >= BEATBOX_TRACK_COUNT) {
        return 0;
    }
    return notes[track];
}

static inline int beatbox_note_track(uint8_t note)
{
    for (uint8_t t = 0; t < BEATBOX_TRACK_COUNT; ++t) {
        if (beatbox_track_note(t) == note) {
            return (int)t;
        }
    }
    return -1;
}

#ifdef __cplusplus
}
#endif
