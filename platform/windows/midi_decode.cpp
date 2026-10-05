#include "midi_decode.h"
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace purrmidi {

std::string NoteNumberToName(uint8_t note) {
    static const char* const note_names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)(note / 12) - 1;
    int name_idx = note % 12;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d(%u)", note_names[name_idx], octave, (unsigned)note);
    return std::string(buf);
}

std::string GetCCName(uint8_t cc) {
    switch (cc) {
        case 1:
        case 71:
        case 74:
            return "Damp";
        case 72:
            return "Decay";
        case 64:
            return "Sustain";
        default:
            return "не используется pluck_synth";
    }
}

std::string DecodeMidiMessage(const std::vector<uint8_t>& msg) {
    if (msg.empty()) return "";

    uint8_t status = msg[0];
    uint8_t cmd = status & 0xF0;
    uint8_t ch = (status & 0x0F) + 1;

    std::ostringstream ss;

    if (status >= 0xF0) {
        // System / Realtime
        switch (status) {
            case 0xF0: {
                ss << "SysEx len=" << msg.size();
                break;
            }
            case 0xF8: ss << "Realtime Clock"; break;
            case 0xFA: ss << "Realtime Start"; break;
            case 0xFB: ss << "Realtime Continue"; break;
            case 0xFC: ss << "Realtime Stop"; break;
            case 0xFE: ss << "Realtime ActiveSensing"; break;
            case 0xFF: ss << "Realtime Reset"; break;
            default:
                ss << "System 0x" << std::uppercase << std::hex << (int)status;
                break;
        }
        return ss.str();
    }

    uint8_t d1 = (msg.size() > 1) ? msg[1] : 0;
    uint8_t d2 = (msg.size() > 2) ? msg[2] : 0;

    switch (cmd) {
        case 0x90: { // Note On
            if (d2 == 0) {
                ss << "NoteOn vel=0 -> трактуется как NoteOff";
            } else {
                ss << "NoteOn  ch=" << (int)ch << " " << NoteNumberToName(d1) << " vel=" << (int)d2;
            }
            break;
        }
        case 0x80: { // Note Off
            ss << "NoteOff ch=" << (int)ch << " " << NoteNumberToName(d1) << " vel=" << (int)d2;
            break;
        }
        case 0xB0: { // CC
            std::string cc_name = GetCCName(d1);
            ss << "CC ch=" << (int)ch << " #" << (int)d1;
            if (d1 == 1 || d1 == 71 || d1 == 74 || d1 == 72 || d1 == 64) {
                ss << " " << cc_name << " val=" << (int)d2;
            } else {
                ss << " val=" << (int)d2 << " (" << cc_name << ")";
            }
            break;
        }
        case 0xA0: { // PolyAT
            ss << "PolyAT ch=" << (int)ch << " " << NoteNumberToName(d1) << " val=" << (int)d2;
            break;
        }
        case 0xC0: { // Program Change
            ss << "ProgramChange ch=" << (int)ch << " prog=" << (int)d1;
            break;
        }
        case 0xD0: { // Channel AT
            ss << "ChannelAT ch=" << (int)ch << " val=" << (int)d1;
            break;
        }
        case 0xE0: { // Pitch Bend
            uint16_t pb = (uint16_t)d1 | ((uint16_t)d2 << 7);
            ss << "PitchBend ch=" << (int)ch << " val=" << pb;
            if (pb == 8192) ss << " (center)";
            break;
        }
        default:
            ss << "Unknown 0x" << std::uppercase << std::hex << (int)status;
            break;
    }

    return ss.str();
}

std::string FormatMidiLine(double timestamp_sec, const std::vector<uint8_t>& msg) {
    std::ostringstream ss;
    ss << "[" << std::setw(8) << std::fixed << std::setprecision(3) << timestamp_sec << "] ";

    std::ostringstream hex_ss;
    if (msg.size() <= 3) {
        for (size_t i = 0; i < msg.size(); ++i) {
            hex_ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)msg[i] << " ";
        }
        std::string hex_str = hex_ss.str();
        while (hex_str.length() < 9) {
            hex_str += " ";
        }
        ss << hex_str;
    } else {
        for (size_t i = 0; i < 3; ++i) {
            hex_ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)msg[i] << " ";
        }
        ss << hex_ss.str();
    }

    ss << "| " << DecodeMidiMessage(msg);
    return ss.str();
}

} // namespace purrmidi
