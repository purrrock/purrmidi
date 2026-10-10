#include <iostream>
#include <cassert>
#include "midi_decode.h"

using namespace purrmidi;

void test_note_number_to_name() {
    assert(NoteNumberToName(60) == "C4(60)");
    assert(NoteNumberToName(0) == "C-1(0)");
    assert(NoteNumberToName(127) == "G9(127)");
    assert(NoteNumberToName(69) == "A4(69)");
}

void test_note_on_off() {
    std::vector<uint8_t> note_on = {0x90, 60, 100};
    std::string s1 = DecodeMidiMessage(note_on);
    assert(s1.find("NoteOn") != std::string::npos);
    assert(s1.find("ch=1") != std::string::npos);
    assert(s1.find("C4(60)") != std::string::npos);
    assert(s1.find("vel=100") != std::string::npos);

    std::vector<uint8_t> note_on_vel0 = {0x90, 60, 0};
    std::string s2 = DecodeMidiMessage(note_on_vel0);
    assert(s2.find("NoteOn vel=0 -> трактуется как NoteOff") != std::string::npos);

    std::vector<uint8_t> note_off = {0x80, 60, 64};
    std::string s3 = DecodeMidiMessage(note_off);
    assert(s3.find("NoteOff") != std::string::npos);
    assert(s3.find("vel=64") != std::string::npos);
}

void test_cc() {
    std::vector<uint8_t> cc_damp = {0xB0, 1, 127};
    std::string s1 = DecodeMidiMessage(cc_damp);
    assert(s1.find("CC ch=1 #1 Damp val=127") != std::string::npos);

    std::vector<uint8_t> cc_decay = {0xB0, 72, 100};
    std::string s2 = DecodeMidiMessage(cc_decay);
    assert(s2.find("CC ch=1 #72 Decay val=100") != std::string::npos);

    std::vector<uint8_t> cc_sustain = {0xB0, 64, 127};
    std::string s3 = DecodeMidiMessage(cc_sustain);
    assert(s3.find("CC ch=1 #64 Sustain val=127") != std::string::npos);

    std::vector<uint8_t> cc_other = {0xB0, 7, 100};
    std::string s4 = DecodeMidiMessage(cc_other);
    assert(s4.find("CC ch=1 #7 val=100 (Not used)") != std::string::npos);
}

void test_other_channel_voice() {
    std::vector<uint8_t> poly_at = {0xA0, 60, 80};
    std::string s1 = DecodeMidiMessage(poly_at);
    assert(s1.find("PolyAT ch=1 C4(60) val=80") != std::string::npos);

    std::vector<uint8_t> pc = {0xC0, 5};
    std::string s2 = DecodeMidiMessage(pc);
    assert(s2.find("ProgramChange ch=1 prog=5") != std::string::npos);

    std::vector<uint8_t> chan_at = {0xD0, 90};
    std::string s3 = DecodeMidiMessage(chan_at);
    assert(s3.find("ChannelAT ch=1 val=90") != std::string::npos);
}

void test_pitch_bend() {
    std::vector<uint8_t> pb_center = {0xE0, 0x00, 0x40};
    std::string s1 = DecodeMidiMessage(pb_center);
    assert(s1.find("PitchBend") != std::string::npos);
    assert(s1.find("val=8192") != std::string::npos);
    assert(s1.find("(center)") != std::string::npos);

    std::vector<uint8_t> pb_max = {0xE0, 0x7F, 0x7F};
    std::string s2 = DecodeMidiMessage(pb_max);
    assert(s2.find("PitchBend ch=1 val=16383") != std::string::npos);
    assert(s2.find("(center)") == std::string::npos);
}

void test_sysex_and_realtime() {
    std::vector<uint8_t> empty;
    assert(DecodeMidiMessage(empty) == "");

    std::vector<uint8_t> sysex = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    std::string s1 = DecodeMidiMessage(sysex);
    assert(s1.find("SysEx len=6") != std::string::npos);

    assert(DecodeMidiMessage({0xF8}).find("Realtime Clock") != std::string::npos);
    assert(DecodeMidiMessage({0xFA}).find("Realtime Start") != std::string::npos);
    assert(DecodeMidiMessage({0xFB}).find("Realtime Continue") != std::string::npos);
    assert(DecodeMidiMessage({0xFC}).find("Realtime Stop") != std::string::npos);
    assert(DecodeMidiMessage({0xFE}).find("Realtime ActiveSensing") != std::string::npos);
    assert(DecodeMidiMessage({0xFF}).find("Realtime Reset") != std::string::npos);
    assert(DecodeMidiMessage({0xF2}).find("System 0xF2") != std::string::npos);
}

void test_format_line() {
    std::vector<uint8_t> note_on = {0x90, 0x3C, 0x64};
    std::string line = FormatMidiLine(12.345, note_on);
    assert(line.find("[  12.345]") != std::string::npos);
    assert(line.find("90 3C 64") != std::string::npos);
    assert(line.find("NoteOn") != std::string::npos);

    std::vector<uint8_t> long_msg = {0xF0, 0x01, 0x02, 0x03, 0xF7};
    std::string line2 = FormatMidiLine(0.1, long_msg);
    assert(line2.find("F0 01 02") != std::string::npos);
    assert(line2.find("SysEx len=5") != std::string::npos);
}

int main() {
    test_note_number_to_name();
    test_note_on_off();
    test_cc();
    test_other_channel_voice();
    test_pitch_bend();
    test_sysex_and_realtime();
    test_format_line();
    std::cout << "test_midi_decode passed successfully." << std::endl;
    return 0;
}
