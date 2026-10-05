#ifndef MIDI_INPUT_H
#define MIDI_INPUT_H

#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <RtMidi.h>
#include "midi_event.h"

namespace purrmidi {

struct MidiPortInfo {
    unsigned int index;
    std::string name;
};

using MidiMonitorCallback = std::function<void(double timeStamp, const std::vector<uint8_t>& message)>;

class MidiInput {
public:
    MidiInput();
    ~MidiInput();

    static std::vector<MidiPortInfo> ListPorts();

    bool OpenPort(unsigned int port_index, bool ignore_sysex = true, bool ignore_timing = false, bool ignore_sens = true);
    void ClosePort();

    void SetMonitorCallback(MidiMonitorCallback cb) { monitor_cb_ = cb; }

private:
    static void RtMidiCallback(double timeStamp, std::vector<unsigned char>* message, void* userData);

    std::unique_ptr<RtMidiIn> rt_midi_;
    MidiMonitorCallback monitor_cb_;
    bool is_open_{false};
};

} // namespace purrmidi

#endif // MIDI_INPUT_H
