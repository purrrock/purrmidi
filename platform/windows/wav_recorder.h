#ifndef WAV_RECORDER_H
#define WAV_RECORDER_H

#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>

namespace purrmidi {

class WavRecorder {
public:
    WavRecorder();
    ~WavRecorder() = default;

    void Init(uint32_t seconds, uint32_t sample_rate = 48000, uint16_t num_channels = 2);
    bool IsEnabled() const { return enabled_; }
    void WriteSamples(const int16_t* buffer, uint32_t num_frames);
    bool SaveToFile(const std::string& filename);

private:
    bool enabled_{false};
    uint32_t sample_rate_{48000};
    uint16_t num_channels_{2};
    uint32_t max_samples_{0};
    std::atomic<uint32_t> written_samples_{0};
    std::vector<int16_t> buffer_;
};

} // namespace purrmidi

#endif // WAV_RECORDER_H
