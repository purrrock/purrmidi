#include "wav_recorder.h"
#include <fstream>
#include <cstring>

namespace purrmidi {

#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 2;
    uint32_t sample_rate = 48000;
    uint32_t byte_rate = 48000 * 2 * 2;
    uint16_t block_align = 4;
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

WavRecorder::WavRecorder() = default;

void WavRecorder::Init(uint32_t seconds, uint32_t sample_rate, uint16_t num_channels) {
    if (seconds == 0) {
        enabled_ = false;
        return;
    }
    sample_rate_ = sample_rate;
    num_channels_ = num_channels;
    max_samples_ = seconds * sample_rate_ * num_channels_;
    buffer_.resize(max_samples_, 0);
    written_samples_.store(0, std::memory_order_relaxed);
    enabled_ = true;
}

void WavRecorder::WriteSamples(const int16_t* buffer, uint32_t num_frames) {
    if (!enabled_ || !buffer || num_frames == 0) return;

    uint32_t total_samples = num_frames * num_channels_;
    uint32_t current = written_samples_.load(std::memory_order_relaxed);

    if (current >= max_samples_) return;

    uint32_t count = total_samples;
    if (current + count > max_samples_) {
        count = max_samples_ - current;
    }

    uint32_t offset = written_samples_.fetch_add(count, std::memory_order_relaxed);
    if (offset >= max_samples_) return;

    if (offset + count > max_samples_) {
        count = max_samples_ - offset;
    }

    std::memcpy(&buffer_[offset], buffer, count * sizeof(int16_t));
}

bool WavRecorder::SaveToFile(const std::string& filename) {
    if (!enabled_ || filename.empty()) return false;

    uint32_t total_samples = written_samples_.load(std::memory_order_relaxed);
    if (total_samples > max_samples_) total_samples = max_samples_;

    uint32_t data_bytes = total_samples * sizeof(int16_t);

    WavHeader header;
    header.num_channels = num_channels_;
    header.sample_rate = sample_rate_;
    header.bits_per_sample = 16;
    header.block_align = num_channels_ * sizeof(int16_t);
    header.byte_rate = sample_rate_ * header.block_align;
    header.data_size = data_bytes;
    header.file_size = 36 + data_bytes;

    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs.is_open()) return false;

    ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (data_bytes > 0) {
        ofs.write(reinterpret_cast<const char*>(buffer_.data()), data_bytes);
    }
    ofs.close();

    return ofs.good();
}

} // namespace purrmidi
