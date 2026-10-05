#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include "wav_recorder.h"

namespace purrmidi {

struct AudioDeviceInfo {
    uint32_t index;
    std::string name;
    bool is_default;
};

class AudioOut {
public:
    AudioOut();
    ~AudioOut();

    static std::vector<AudioDeviceInfo> ListDevices();

    bool Init(uint32_t buffer_frames = 256, float gain = 1.0f, WavRecorder* recorder = nullptr);
    bool Start();
    void Stop();

    std::string GetDeviceName() const { return device_name_; }
    uint32_t GetActualPeriodFrames() const { return actual_period_frames_; }
    uint32_t GetSampleRate() const { return 48000; }
    double GetEstimatedLatencyMs() const { return estimated_latency_ms_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string device_name_{"Default"};
    uint32_t actual_period_frames_{256};
    double estimated_latency_ms_{0.0};
    bool is_started_{false};
};

} // namespace purrmidi

#endif // AUDIO_OUT_H
