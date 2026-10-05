#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_out.h"
#include "pluck_synth.h"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace purrmidi {

struct AudioCallbackData {
    float gain{1.0f};
    WavRecorder* recorder{nullptr};
};

struct AudioOut::Impl {
    ma_context context;
    ma_device device;
    bool context_initialized{false};
    bool device_initialized{false};
    AudioCallbackData cb_data;
};

static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    AudioCallbackData* cb_data = static_cast<AudioCallbackData*>(pDevice->pUserData);
    int16_t* out = static_cast<int16_t*>(pOutput);

    // Render directly using PluckSynth_FillStereoBuffer
    PluckSynth_FillStereoBuffer(out, frameCount);

    // Apply gain if not 1.0f
    float gain = cb_data ? cb_data->gain : 1.0f;
    if (gain != 1.0f) {
        uint32_t total_samples = frameCount * 2;
        for (uint32_t i = 0; i < total_samples; ++i) {
            float s = (float)out[i] * gain;
            if (s > 32767.0f) s = 32767.0f;
            else if (s < -32768.0f) s = -32768.0f;
            out[i] = (int16_t)s;
        }
    }

    // Record to WAV buffer if recorder provided
    if (cb_data && cb_data->recorder && cb_data->recorder->IsEnabled()) {
        cb_data->recorder->WriteSamples(out, frameCount);
    }
}

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}

AudioOut::~AudioOut() {
    Stop();
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
    }
    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
    }
}

std::vector<AudioDeviceInfo> AudioOut::ListDevices() {
    std::vector<AudioDeviceInfo> list;
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
        return list;
    }

    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    if (ma_context_get_devices(&ctx, &pPlaybackInfos, &playbackCount, NULL, NULL) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            list.push_back({i, pPlaybackInfos[i].name, pPlaybackInfos[i].isDefault != 0});
        }
    }

    ma_context_uninit(&ctx);
    return list;
}

bool AudioOut::Init(uint32_t buffer_frames, float gain, WavRecorder* recorder) {
    impl_->cb_data.gain = gain;
    impl_->cb_data.recorder = recorder;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate        = 48000;
    config.periodSizeInFrames = buffer_frames;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback      = data_callback;
    config.pUserData         = &impl_->cb_data;

    ma_result result = ma_device_init(NULL, &config, &impl_->device);
    if (result != MA_SUCCESS) {
        std::cerr << "miniaudio error: failed to initialize playback device (code " << result << ")" << std::endl;
        return false;
    }
    impl_->device_initialized = true;

    device_name_ = impl_->device.playback.name;
    actual_period_frames_ = impl_->device.playback.internalPeriodSizeInFrames;
    if (actual_period_frames_ == 0) {
        actual_period_frames_ = buffer_frames;
    }
    estimated_latency_ms_ = (double)actual_period_frames_ / 48000.0 * 1000.0;

    return true;
}

bool AudioOut::Start() {
    if (!impl_->device_initialized) return false;
    ma_result result = ma_device_start(&impl_->device);
    if (result == MA_SUCCESS) {
        is_started_ = true;
        return true;
    }
    std::cerr << "miniaudio error: failed to start playback device (code " << result << ")" << std::endl;
    return false;
}

void AudioOut::Stop() {
    if (impl_->device_initialized && is_started_) {
        ma_device_stop(&impl_->device);
        is_started_ = false;
    }
}

} // namespace purrmidi
