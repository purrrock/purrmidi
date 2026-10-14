#include "virtual_keyboard.h"
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace purrmidi {

VirtualKeyboard::VirtualKeyboard() {
#ifdef _WIN32
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    if (hInput != INVALID_HANDLE_VALUE && hInput != NULL) {
        if (GetFileType(hInput) == FILE_TYPE_CHAR) {
            DWORD mode = 0;
            if (GetConsoleMode(hInput, &mode)) {
                original_console_mode_ = mode;
                console_mode_saved_ = true;
                DWORD new_mode = (mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
                if (SetConsoleMode(hInput, new_mode)) {
                    console_handle_ = hInput;
                }
            }
        }
    }
#endif
}

VirtualKeyboard::~VirtualKeyboard() {
    AllNotesOff();
#ifdef _WIN32
    if (console_mode_saved_ && console_handle_) {
        SetConsoleMode(console_handle_, original_console_mode_);
    }
#endif
}

void VirtualKeyboard::SetBaseOctave(int octave) {
    if (octave < 0) octave = 0;
    if (octave > 8) octave = 8;
    base_octave_ = octave;
}

void VirtualKeyboard::SetVelocity(uint8_t velocity) {
    if (velocity < 1) velocity = 1;
    if (velocity > 127) velocity = 127;
    velocity_ = velocity;
}

uint32_t VirtualKeyboard::MakeKeyId(uint32_t key_code, uint16_t scan_code) const {
    if (scan_code != 0) {
        return static_cast<uint32_t>(scan_code);
    }
    return 0x10000U | (key_code & 0xFFFFU);
}

VirtualKeyboard::KeyMapping VirtualKeyboard::MapKey(uint32_t key_code, uint16_t scan_code) const {
    KeyMapping mapping;

    // 1. Проверка физического сканкода (для сохранения раскладки независимо от языка ОС)
    if (scan_code != 0) {
        switch (scan_code) {
            // Белые клавиши (нижний ряд)
            case 0x2C: mapping.relative_semitone = 0;  return mapping; // Z -> C
            case 0x2D: mapping.relative_semitone = 2;  return mapping; // X -> D
            case 0x2E: mapping.relative_semitone = 4;  return mapping; // C -> E
            case 0x2F: mapping.relative_semitone = 5;  return mapping; // V -> F
            case 0x30: mapping.relative_semitone = 7;  return mapping; // B -> G
            case 0x31: mapping.relative_semitone = 9;  return mapping; // N -> A
            case 0x32: mapping.relative_semitone = 11; return mapping; // M -> B
            case 0x33: mapping.relative_semitone = 12; return mapping; // , -> C (+1 oct)
            case 0x34: mapping.relative_semitone = 14; return mapping; // . -> D (+1 oct)
            case 0x35: mapping.relative_semitone = 16; return mapping; // / -> E (+1 oct)

            // Чёрные клавиши (средний ряд)
            case 0x1F: mapping.relative_semitone = 1;  return mapping; // S -> C#
            case 0x20: mapping.relative_semitone = 3;  return mapping; // D -> D#
            case 0x22: mapping.relative_semitone = 6;  return mapping; // G -> F#
            case 0x23: mapping.relative_semitone = 8;  return mapping; // H -> G#
            case 0x24: mapping.relative_semitone = 10; return mapping; // J -> A#
            case 0x26: mapping.relative_semitone = 13; return mapping; // L -> C# (+1 oct)
            case 0x27: mapping.relative_semitone = 15; return mapping; // ; -> D# (+1 oct)

            // Управление
            case 0x0F: mapping.is_tab = true;      return mapping; // TAB
            case 0x1A: mapping.is_oct_down = true; return mapping; // [
            case 0x1B: mapping.is_oct_up = true;   return mapping; // ]
            default: break;
        }
    }

    // 2. Резервная проверка Virtual Key Code
    switch (key_code) {
        // Белые клавиши
        case 0x5A: mapping.relative_semitone = 0;  return mapping; // VK_Z
        case 0x58: mapping.relative_semitone = 2;  return mapping; // VK_X
        case 0x43: mapping.relative_semitone = 4;  return mapping; // VK_C
        case 0x56: mapping.relative_semitone = 5;  return mapping; // VK_V
        case 0x42: mapping.relative_semitone = 7;  return mapping; // VK_B
        case 0x4E: mapping.relative_semitone = 9;  return mapping; // VK_N
        case 0x4D: mapping.relative_semitone = 11; return mapping; // VK_M
        case 0xBC: mapping.relative_semitone = 12; return mapping; // VK_OEM_COMMA
        case 0xBE: mapping.relative_semitone = 14; return mapping; // VK_OEM_PERIOD
        case 0xBF: mapping.relative_semitone = 16; return mapping; // VK_OEM_2 (slash)

        // Чёрные клавиши
        case 0x53: mapping.relative_semitone = 1;  return mapping; // VK_S
        case 0x44: mapping.relative_semitone = 3;  return mapping; // VK_D
        case 0x47: mapping.relative_semitone = 6;  return mapping; // VK_G
        case 0x48: mapping.relative_semitone = 8;  return mapping; // VK_H
        case 0x4A: mapping.relative_semitone = 10; return mapping; // VK_J
        case 0x4C: mapping.relative_semitone = 13; return mapping; // VK_L
        case 0xBA: mapping.relative_semitone = 15; return mapping; // VK_OEM_1 (semicolon)

        // Управление
        case 0x09: mapping.is_tab = true;      return mapping; // VK_TAB
        case 0xDB: mapping.is_oct_down = true; return mapping; // VK_OEM_4 ([)
        case 0xDD: mapping.is_oct_up = true;   return mapping; // VK_OEM_6 (])
        default: break;
    }

    return mapping;
}

void VirtualKeyboard::DispatchEvent(const MIDI_Event_t& ev) {
    if (event_cb_) {
        event_cb_(ev);
    }
}

void VirtualKeyboard::HandleKeyDown(uint32_t key_code, uint16_t scan_code) {
    uint32_t key_id = MakeKeyId(key_code, scan_code);

    // Игнорируем повторные автонажатия удержанной клавиши ОС Windows (для всех клавиш, включая TAB и октавы)
    if (!held_keys_.insert(key_id).second) {
        return;
    }

    KeyMapping mapping = MapKey(key_code, scan_code);

    if (mapping.is_tab) {
        MIDI_Event_t ev;
        ev.status = MIDI_STATUS_PROGRAM_CHANGE;
        ev.data1 = 127;
        ev.data2 = 0;
        DispatchEvent(ev);
        return;
    }

    if (mapping.is_oct_down) {
        if (base_octave_ > 0) base_octave_--;
        return;
    }

    if (mapping.is_oct_up) {
        if (base_octave_ < 8) base_octave_++;
        return;
    }

    if (mapping.relative_semitone >= 0) {
        int note_val = (base_octave_ + 1) * 12 + mapping.relative_semitone;
        if (note_val >= 0 && note_val <= 127) {
            uint8_t note = static_cast<uint8_t>(note_val);
            active_key_notes_[key_id] = note;

            MIDI_Event_t ev;
            ev.status = MIDI_STATUS_NOTE_ON;
            ev.data1 = note;
            ev.data2 = velocity_;
            DispatchEvent(ev);
        }
    }
}

void VirtualKeyboard::HandleKeyUp(uint32_t key_code, uint16_t scan_code) {
    uint32_t key_id = MakeKeyId(key_code, scan_code);
    held_keys_.erase(key_id);

    auto it = active_key_notes_.find(key_id);
    if (it == active_key_notes_.end()) {
        return; // Повторное или неизвестное отпускание игнорируется
    }

    uint8_t note = it->second;
    active_key_notes_.erase(it);

    MIDI_Event_t ev;
    ev.status = MIDI_STATUS_NOTE_OFF;
    ev.data1 = note;
    ev.data2 = 0;
    DispatchEvent(ev);
}

void VirtualKeyboard::AllNotesOff() {
    for (const auto& pair : active_key_notes_) {
        MIDI_Event_t ev;
        ev.status = MIDI_STATUS_NOTE_OFF;
        ev.data1 = pair.second;
        ev.data2 = 0;
        DispatchEvent(ev);
    }
    active_key_notes_.clear();
    held_keys_.clear();
}

bool VirtualKeyboard::IsNoteActive(uint8_t note) const {
    for (const auto& pair : active_key_notes_) {
        if (pair.second == note) {
            return true;
        }
    }
    return false;
}

void VirtualKeyboard::Poll() {
#ifdef _WIN32
    if (!console_handle_) return;
/*
    HWND console_hwnd = GetConsoleWindow();
    if (console_hwnd && IsWindowVisible(console_hwnd)) {
        HWND fg_hwnd = GetForegroundWindow();
        if (fg_hwnd && fg_hwnd != console_hwnd) {
            if (focus_state_) {
                focus_state_ = false;
                AllNotesOff();
            }
        } else if (fg_hwnd == console_hwnd) {
            focus_state_ = true;
        }
    }
*/
    DWORD num_events = 0;
    if (!GetNumberOfConsoleInputEvents(console_handle_, &num_events) || num_events == 0) {
        return;
    }

    std::vector<INPUT_RECORD> records(num_events);
    DWORD read_events = 0;
    if (!ReadConsoleInputW(console_handle_, records.data(), num_events, &read_events) || read_events == 0) {
        return;
    }

    for (DWORD i = 0; i < read_events; ++i) {
        const auto& rec = records[i];
        if (rec.EventType == FOCUS_EVENT) {
            if (!rec.Event.FocusEvent.bSetFocus) {
                focus_state_ = false;
                AllNotesOff();
            } else {
                focus_state_ = true;
            }
        } else if (rec.EventType == KEY_EVENT) {

            const auto& ke = rec.Event.KeyEvent;
            uint32_t vk = ke.wVirtualKeyCode;
            uint16_t sc = ke.wVirtualScanCode;
            if (ke.bKeyDown) {
                HandleKeyDown(vk, sc);
            } else {
                HandleKeyUp(vk, sc);
            }
        }
    }
#endif
}

} // namespace purrmidi
