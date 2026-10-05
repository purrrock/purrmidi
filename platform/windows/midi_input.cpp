#include "midi_input.h"
extern "C" {
#include "midi_queue.h"
}
#include <iostream>

namespace purrmidi {

MidiInput::MidiInput() {
    try {
        rt_midi_ = std::make_unique<RtMidiIn>();
    } catch (RtMidiError& e) {
        std::cerr << "RtMidi error: " << e.getMessage() << std::endl;
    }
}

MidiInput::~MidiInput() {
    ClosePort();
}

std::vector<MidiPortInfo> MidiInput::ListPorts() {
    std::vector<MidiPortInfo> ports;
    try {
        RtMidiIn rt_in;
        unsigned int count = rt_in.getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            ports.push_back({i, rt_in.getPortName(i)});
        }
    } catch (RtMidiError& e) {
        std::cerr << "RtMidi error listing ports: " << e.getMessage() << std::endl;
    }
    return ports;
}

bool MidiInput::OpenPort(unsigned int port_index, bool ignore_sysex, bool ignore_timing, bool ignore_sens) {
    if (!rt_midi_) return false;

    ClosePort();

    try {
        unsigned int count = rt_midi_->getPortCount();
        if (port_index >= count) {
            std::cerr << "Error: MIDI port index " << port_index << " out of range (total " << count << " ports)." << std::endl;
            return false;
        }

        rt_midi_->ignoreTypes(ignore_sysex, ignore_timing, ignore_sens);
        rt_midi_->setCallback(&MidiInput::RtMidiCallback, this);
        rt_midi_->openPort(port_index);
        is_open_ = true;
        return true;
    } catch (RtMidiError& e) {
        std::cerr << "RtMidi error opening port: " << e.getMessage() << std::endl;
        return false;
    }
}

void MidiInput::ClosePort() {
    if (rt_midi_ && is_open_) {
        try {
            rt_midi_->closePort();
            rt_midi_->cancelCallback();
        } catch (RtMidiError&) {}
        is_open_ = false;
    }
}

void MidiInput::RtMidiCallback(double timeStamp, std::vector<unsigned char>* message, void* userData) {
    if (!message || message->empty() || !userData) return;

    MidiInput* self = static_cast<MidiInput*>(userData);

    if (self->monitor_cb_) {
        self->monitor_cb_(timeStamp, *message);
    }

    uint8_t status = (*message)[0];
    if (status < 0xF0) {
        MIDI_Event_t ev;
        ev.status = status;
        ev.data1 = (message->size() > 1) ? (*message)[1] : 0;
        ev.data2 = (message->size() > 2) ? (*message)[2] : 0;
        MIDI_Queue_Push(&ev);
    }
}

} // namespace purrmidi
