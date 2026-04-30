#include "afe_wake_word.h"

#include <esp_log.h>
#include <sstream>
#include <cstring>

// esp-adf opus encoder (same component used in audio_service.cc)
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"

#define DETECTION_RUNNING_EVENT 1
#define TAG "AfeWakeWord"

// Opus encode frame duration for wake-word pre-roll (60 ms matches audio_service)
#define WW_OPUS_FRAME_DURATION_MS 60

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {
    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    models_ = esp_srmodel_init("model");
    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialise wakenet models (srmodel_init returned %p, num=%d)",
                 (void*)models_, models_ ? models_->num : -1);
        return false;
    }

    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            const char* words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            if (words != nullptr) {
                std::stringstream ss(words);
                std::string word;
                while (std::getline(ss, word, ';')) {
                    wake_words_.push_back(word);
                }
            }
        }
    }

    if (wakenet_model_ == nullptr) {
        ESP_LOGE(TAG, "No wakenet model found in model partition");
        return false;
    }
    ESP_LOGI(TAG, "Wake words: %zu", wake_words_.size());

    // Build input format string: one 'M' per mic channel, one 'R' per ref channel
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_,
                                                AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init               = codec_->input_reference();
    afe_config->aec_mode               = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core     = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode      = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_  = afe_iface_->create_from_config(afe_config);

    xTaskCreate([](void* arg) {
        static_cast<AfeWakeWord*>(arg)->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "ww_detect", 4096, this, 3, nullptr);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) return;
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) return 0;
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto feed_size  = afe_iface_->get_feed_chunksize(afe_data_);
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    ESP_LOGI(TAG, "Wake word detection task started, feed=%d fetch=%d", (int)feed_size, (int)fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        // Keep a rolling 2-second window of pre-roll PCM
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            if (!wake_words_.empty() && res->wakenet_model_index >= 1 &&
                res->wakenet_model_index <= (int)wake_words_.size()) {
                last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];
            } else {
                last_detected_wake_word_ = "unknown";
            }
            ESP_LOGI(TAG, "Wake word detected: %s", last_detected_wake_word_.c_str());
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    wake_word_pcm_.emplace_back(data, data + samples);
    // Keep ~2 seconds at 30ms chunks (16 kHz → ~67 chunks)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    // Encode stored pre-roll PCM using esp_opus_enc (same API as audio_service).
    // This runs in a static-stack task allocated in PSRAM to avoid stack overflow.
    const size_t kStackSize = 4096 * 7;
    wake_word_opus_.clear();

    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(kStackSize, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto* self = static_cast<AfeWakeWord*>(arg);

        // Open encoder
        esp_opus_enc_config_t enc_cfg = {
            .sample_rate     = ESP_AUDIO_SAMPLE_RATE_16K,
            .channel         = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .bitrate         = ESP_OPUS_BITRATE_AUTO,
            .frame_duration  = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
            .complexity      = 0, // fastest
            .enable_fec      = false,
            .enable_dtx      = false,
            .enable_vbr      = true,
        };
        void* encoder = nullptr;
        if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder) != ESP_AUDIO_ERR_OK || encoder == nullptr) {
            ESP_LOGE(TAG, "EncodeWakeWordData: failed to open encoder");
            // Push sentinel to unblock GetWakeWordOpus
            std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
            self->wake_word_opus_.push_back(std::vector<uint8_t>());
            self->wake_word_cv_.notify_all();
            vTaskDelete(NULL);
            return;
        }

        int frame_size_bytes = 0;
        int out_buf_size     = 0;
        esp_opus_enc_get_frame_size(encoder, &frame_size_bytes, &out_buf_size);
        int frame_samples = frame_size_bytes / sizeof(int16_t);

        // Flatten all PCM chunks into a single contiguous buffer
        std::vector<int16_t> all_pcm;
        all_pcm.reserve(self->wake_word_pcm_.size() * frame_samples);
        for (auto& chunk : self->wake_word_pcm_) {
            all_pcm.insert(all_pcm.end(), chunk.begin(), chunk.end());
        }
        self->wake_word_pcm_.clear();

        int packets = 0;
        size_t offset = 0;
        std::vector<uint8_t> out_buf(out_buf_size);

        while ((int)(all_pcm.size() - offset) >= frame_samples) {
            esp_audio_enc_in_frame_t in_frame = {
                .buffer = reinterpret_cast<uint8_t*>(all_pcm.data() + offset),
                .len    = (uint32_t)frame_size_bytes,
            };
            esp_audio_enc_out_frame_t out_frame = {
                .buffer        = out_buf.data(),
                .len           = (uint32_t)out_buf_size,
                .encoded_bytes = 0,
            };
            if (esp_opus_enc_process(encoder, &in_frame, &out_frame) == ESP_AUDIO_ERR_OK
                && out_frame.encoded_bytes > 0) {
                std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
                self->wake_word_opus_.emplace_back(out_buf.data(),
                                                   out_buf.data() + out_frame.encoded_bytes);
                self->wake_word_cv_.notify_all();
                packets++;
            }
            offset += frame_samples;
        }

        esp_opus_enc_close(encoder);
        ESP_LOGI(TAG, "Encoded wake word pre-roll: %d packets", packets);

        // Push sentinel empty packet to signal end of stream
        std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
        self->wake_word_opus_.push_back(std::vector<uint8_t>());
        self->wake_word_cv_.notify_all();
        vTaskDelete(NULL);
    }, "ww_encode", kStackSize, this, 2,
       wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty(); // empty means end-of-stream sentinel
}
