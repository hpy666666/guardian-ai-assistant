#include "no_audio_processor.h"
#include <esp_log.h>
#include <cmath>
#include <cstdint>

#define TAG "NoAudioProcessor"

// ── 音量调试：每 N 帧打印一次 RMS，0 关闭，1 每帧都打 ──
#define AUDIO_DEBUG_RMS_INTERVAL 20

static int32_t s_rms_frame_counter = 0;

static int32_t calc_rms(const int16_t* buf, size_t n) {
    if (n == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)buf[i] * buf[i];
    return (int32_t)sqrtf((float)(sum / (int64_t)n));
}

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms,
                                   srmodel_list_t* /* models_list */) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

#if AUDIO_DEBUG_RMS_INTERVAL > 0
    if (++s_rms_frame_counter >= AUDIO_DEBUG_RMS_INTERVAL) {
        s_rms_frame_counter = 0;
        int32_t rms = calc_rms(data.data(), data.size());
        ESP_LOGI(TAG, "MIC RMS=%ld  samples=%d  ch=%d",
                 (long)rms, (int)data.size(), codec_->input_channels());
    }
#endif

    if (codec_->input_channels() == 2) {
        auto mono_data = std::vector<int16_t>(data.size() / 2);
        for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
            mono_data[i] = data[j];
        }
        output_callback_(std::move(mono_data));
    } else {
        output_callback_(std::move(data));
    }
}

void NoAudioProcessor::Start() {
    is_running_ = true;
}

void NoAudioProcessor::Stop() {
    is_running_ = false;
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
