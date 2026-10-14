#ifndef VIRTUAL_KEYBOARD_H
#define VIRTUAL_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "midi_event.h"

namespace purrmidi {

/**
 * @file    virtual_keyboard.h
 * @brief   Виртуальная MIDI-клавиатура для Windows host-сборки.
 *          Преобразует нажатия компьютерных клавиш в MIDI-события (Note On/Off, Program Change).
 */

class VirtualKeyboard {
public:
    using EventCallback = std::function<void(const MIDI_Event_t&)>;

    VirtualKeyboard();
    ~VirtualKeyboard();

    /** Установка callback-функции для отправки MIDI-событий. */
    void SetEventCallback(EventCallback cb) { event_cb_ = cb; }

    /** Установка/получение базовой октавы (0..8, по умолчанию 4 -> C4 = MIDI 60). */
    void SetBaseOctave(int octave);
    int GetBaseOctave() const { return base_octave_; }

    /** Установка/получение velocity для Note On (1..127, по умолчанию 100). */
    void SetVelocity(uint8_t velocity);
    uint8_t GetVelocity() const { return velocity_; }

    /** Обработка нажатия клавиши (key_code — VK код, scan_code — аппаратный сканкод). */
    void HandleKeyDown(uint32_t key_code, uint16_t scan_code);

    /** Обработка отпускания клавиши. */
    void HandleKeyUp(uint32_t key_code, uint16_t scan_code);

    /** Сброс всех удерживаемых нот (отправка Note Off для активных нот). */
    void AllNotesOff();

    /** Возвращает количество удерживаемых клавиш в данный момент. */
    size_t GetActiveNotesCount() const { return active_key_notes_.size(); }

    /** Проверяет, активна ли конкретная MIDI-нота. */
    bool IsNoteActive(uint8_t note) const;

    /**
     * Опрос консольного ввода Windows (вызывается в главном цикле).
     * Обрабатывает события клавиатуры и потерю фокуса окна.
     */
    void Poll();

private:
    struct KeyMapping {
        int relative_semitone{-1}; // -1 если не нота
        bool is_tab{false};
        bool is_oct_down{false};
        bool is_oct_up{false};
    };

    KeyMapping MapKey(uint32_t key_code, uint16_t scan_code) const;
    uint32_t MakeKeyId(uint32_t key_code, uint16_t scan_code) const;
    void DispatchEvent(const MIDI_Event_t& ev);

    int base_octave_{4};
    uint8_t velocity_{100};
    EventCallback event_cb_;

    // Набор всех удерживаемых в данный момент key_id (для фильтрации автоповтора нот и управляющих клавиш)
    std::unordered_set<uint32_t> held_keys_;

    // Отображение key_id -> MIDI note, отправленная при KeyDown
    std::unordered_map<uint32_t, uint8_t> active_key_notes_;

#ifdef _WIN32
    void* console_handle_{nullptr};
    unsigned long original_console_mode_{0};
    bool console_mode_saved_{false};
    bool focus_state_{true};
#endif
};

} // namespace purrmidi

#endif // VIRTUAL_KEYBOARD_H
