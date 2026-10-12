#ifndef CHORD_DETECTOR_H
#define CHORD_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Detect chord from active MIDI notes array.
 *
 * @param active_notes Array of 128 elements, active_notes[i] != 0 if note i is active.
 * @param active_note_count Total count of active keys.
 * @param name Output buffer for chord name (e.g., "C", "Am/C", "Cmaj7").
 * @param name_size Size of the output buffer.
 * @return true if a chord was successfully detected, false otherwise.
 */
bool ChordDetector_Detect(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    char *name,
    size_t name_size
);

#ifdef __cplusplus
}
#endif

#endif /* CHORD_DETECTOR_H */
