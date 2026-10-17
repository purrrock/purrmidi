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
 * @param active_note_count Общее число удерживаемых нот (с учётом октавных дублей). Если < 3 — сразу false.
 * @param name Выходной буфер для имени аккорда (например, "C", "Am/C", "Cmaj7").
 * @param name_size Размер выходного буфера.
 * @return true — аккорд распознан, name заполнен.
 *         false — аккорд не определён (нот < 3, набор высот не совпал ни с одним шаблоном,
 *         либо имя не поместилось в name); в этом случае name содержит пустую строку.
 *
 * Определение идёт по набору классов высот (октавные дубли не важны), басом считается самая низкая нота.
 * Симметричные аккорды (dim7, aug) и пары sus2/sus4 неоднозначны: корнем выбирается бас, а для sus —
 * шаблон, идущий в таблице раньше.
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
