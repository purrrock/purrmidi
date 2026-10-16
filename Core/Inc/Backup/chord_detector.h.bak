#ifndef CHORD_DETECTOR_H
#define CHORD_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file    chord_detector.h
 * @brief   Модуль распознавания аккордов и дробей (slash chords) по массиву нажатых нот.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Детектирование аккорда по активным MIDI-нотам.
 *
 * @param active_notes Массив из 128 элементов (active_notes[i] != 0, если нота i нажата).
 * @param active_note_count Общее число удерживаемых нот.
 * @param name Выходной буфер для имени аккорда (например, "C", "Am/C", "Cmaj7").
 * @param name_size Размер выходного буфера.
 * @return true — аккорд успешно распознан; false — аккорд не определён (активных нот < 3).
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
