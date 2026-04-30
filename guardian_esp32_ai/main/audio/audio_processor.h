#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

#ifdef CONFIG_USE_AUDIO_PROCESSOR
#include <esp_afe_sr_models.h>
#else
// When AFE is disabled, provide a dummy typedef so the interface compiles
// without pulling in esp-sr headers.
typedef struct srmodel_list_s srmodel_list_t;
#endif

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    virtual void Initialize(AudioCodec* codec, int frame_duration_ms,
                            srmodel_list_t* models_list = nullptr) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
};

#endif
