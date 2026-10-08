#include "chord_detector.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t mask;
    const char *suffix;
} ChordTemplate;

/*
 * Table of supported chord interval patterns relative to root (0).
 * Interval bitmask: bit i is set if interval i (0..11 semitones) is in the chord.
 */
static const ChordTemplate chord_templates[] = {
    /* Major: 1 3 5 (0, 4, 7) */
    { (1U << 0) | (1U << 4) | (1U << 7), "" },
    /* Minor: 1 b3 5 (0, 3, 7) */
    { (1U << 0) | (1U << 3) | (1U << 7), "m" },
    /* Dominant 7: 1 3 5 b7 (0, 4, 7, 10) */
    { (1U << 0) | (1U << 4) | (1U << 7) | (1U << 10), "7" },
    /* Major 7: 1 3 5 7 (0, 4, 7, 11) */
    { (1U << 0) | (1U << 4) | (1U << 7) | (1U << 11), "maj7" },
    /* Minor 7: 1 b3 5 b7 (0, 3, 7, 10) */
    { (1U << 0) | (1U << 3) | (1U << 7) | (1U << 10), "m7" },
    /* Half-diminished / m7b5: 1 b3 b5 b7 (0, 3, 6, 10) */
    { (1U << 0) | (1U << 3) | (1U << 6) | (1U << 10), "m7b5" },
    /* Diminished 7: 1 b3 b5 bb7 (0, 3, 6, 9) */
    { (1U << 0) | (1U << 3) | (1U << 6) | (1U << 9), "dim7" },
    /* Diminished: 1 b3 b5 (0, 3, 6) */
    { (1U << 0) | (1U << 3) | (1U << 6), "dim" },
    /* Augmented: 1 3 #5 (0, 4, 8) */
    { (1U << 0) | (1U << 4) | (1U << 8), "aug" },
    /* sus4: 1 4 5 (0, 5, 7) */
    { (1U << 0) | (1U << 5) | (1U << 7), "sus4" },
    /* sus2: 1 2 5 (0, 2, 7) */
    { (1U << 0) | (1U << 2) | (1U << 7), "sus2" },
    /* add9: 1 3 5 9 (0, 2, 4, 7) */
    { (1U << 0) | (1U << 2) | (1U << 4) | (1U << 7), "add9" },
    /* minor add9: 1 b3 5 9 (0, 2, 3, 7) */
    { (1U << 0) | (1U << 2) | (1U << 3) | (1U << 7), "madd9" }
};

#define CHORD_TEMPLATE_COUNT (sizeof(chord_templates) / sizeof(chord_templates[0]))

static const char * const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

bool ChordDetector_Detect(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    char *name,
    size_t name_size)
{
    if (active_notes == NULL || name == NULL || name_size == 0)
    {
        return false;
    }

    if (active_note_count < 3)
    {
        name[0] = '\0';
        return false;
    }

    uint16_t actual_mask = 0;
    int bass_note = -1;

    for (int i = 0; i < 128; i++)
    {
        if (active_notes[i] != 0)
        {
            if (bass_note < 0)
            {
                bass_note = i;
            }
            actual_mask |= (uint16_t)(1U << (i % 12));
        }
    }

    if (bass_note < 0)
    {
        name[0] = '\0';
        return false;
    }

    const uint8_t bass_pc = (uint8_t)(bass_note % 12);

    bool match_found = false;
    uint8_t best_has_slash = 1;
    size_t best_template_idx = CHORD_TEMPLATE_COUNT;
    uint8_t best_root_pc = 0;

    for (size_t t = 0; t < CHORD_TEMPLATE_COUNT; t++)
    {
        const uint16_t t_mask = chord_templates[t].mask;

        for (uint8_t r = 0; r < 12; r++)
        {
            const uint16_t candidate_mask = (uint16_t)(((t_mask << r) | (t_mask >> (12 - r))) & 0x0FFFU);

            if (actual_mask == candidate_mask)
            {
                const uint8_t has_slash = (r == bass_pc) ? 0U : 1U;

                if (!match_found)
                {
                    match_found = true;
                    best_has_slash = has_slash;
                    best_template_idx = t;
                    best_root_pc = r;
                }
                else
                {
                    /* Prefer no-slash over slash */
                    if (has_slash < best_has_slash)
                    {
                        best_has_slash = has_slash;
                        best_template_idx = t;
                        best_root_pc = r;
                    }
                    else if (has_slash == best_has_slash)
                    {
                        /* Prefer earlier template in table (more common chord) */
                        if (t < best_template_idx)
                        {
                            best_template_idx = t;
                            best_root_pc = r;
                        }
                        else if (t == best_template_idx)
                        {
                            /* Deterministic tie-breaker: smaller root pitch class */
                            if (r < best_root_pc)
                            {
                                best_root_pc = r;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!match_found)
    {
        name[0] = '\0';
        return false;
    }

    const char *root_str = note_names[best_root_pc];
    const char *suffix_str = chord_templates[best_template_idx].suffix;

    if (best_has_slash == 0)
    {
        snprintf(name, name_size, "%s%s", root_str, suffix_str);
    }
    else
    {
        const char *bass_str = note_names[bass_pc];
        snprintf(name, name_size, "%s%s/%s", root_str, suffix_str, bass_str);
    }

    return true;
}
