#ifndef MIDI_DECODE_H
#define MIDI_DECODE_H

#include <stdint.h>
#include <string>
#include <vector>

namespace purrmidi {

std::string NoteNumberToName(uint8_t note);
std::string GetCCName(uint8_t cc);
std::string DecodeMidiMessage(const std::vector<uint8_t>& message);
std::string FormatMidiLine(double timestamp_sec, const std::vector<uint8_t>& message);

} // namespace purrmidi

#endif // MIDI_DECODE_H
