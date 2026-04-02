#include "wifi_board.h"
#include "audio/codecs/no_audio_codec.h"
#include "settings.h"
#include "application.h"
#include "config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "GuardianS3Board";

// ── Guardian S3 Board ──────────────────────────────────────
// 独居老人安全监护终端
// INMP441 MIC + MAX98357A SPK，无 I2C codec，用 NoAudioCodecSimplex
class GuardianS3Board : public WifiBoard {
private:
    NoAudioCodecSimplex* audio_codec_ = nullptr;

public:
    GuardianS3Board() {
        ESP_LOGI(TAG, "Guardian S3 Board initializing");

        // 初始化 BOOT 按键（上拉输入，短按切换对话状态）
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        // 启动 BOOT 按键检测任务
        xTaskCreate([](void* arg) {
            bool last_state = true;  // HIGH = not pressed
            while (true) {
                bool current = gpio_get_level(BOOT_BUTTON_GPIO);
                if (last_state && !current) {
                    // 下降沿：按键按下，切换对话状态
                    Application::GetInstance().ToggleChatState();
                }
                last_state = current;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }, "boot_btn", 2048, this, 5, nullptr);
    }

    virtual AudioCodec* GetAudioCodec() override {
        if (audio_codec_ == nullptr) {
            // 使用 NoAudioCodecSimplex:
            //   SPK: I2S0 (BCLK=11, LRCK=12, DOUT=10)   ← 避开 F407 UART RX/TX (GPIO16/17)
            //   MIC: I2S1 (SCK=5,  WS=4,    DIN=6)
            // 注意参数顺序: spk_bclk, spk_ws, spk_dout, mic_sck, mic_ws, mic_din
            audio_codec_ = new NoAudioCodecSimplex(
                16000,                          // input_sample_rate (MIC)
                16000,                          // output_sample_rate (SPK)
                AUDIO_I2S_SPK_GPIO_BCLK,       // spk_bclk  = GPIO11
                AUDIO_I2S_SPK_GPIO_LRCK,       // spk_ws    = GPIO12
                AUDIO_I2S_SPK_GPIO_DOUT,       // spk_dout  = GPIO10
                AUDIO_I2S_MIC_GPIO_SCK,        // mic_sck   = GPIO5
                AUDIO_I2S_MIC_GPIO_WS,         // mic_ws    = GPIO4
                AUDIO_I2S_MIC_GPIO_DIN         // mic_din   = GPIO6
            );
        }
        return audio_codec_;
    }

    virtual std::string GetBoardType() override {
        return "guardian-s3";
    }

    virtual std::string GetBoardJson() override {
        std::string json = "{\"type\":\"guardian-s3\",\"name\":\"Guardian ESP32-S3\"}";
        return json;
    }
};

DECLARE_BOARD(GuardianS3Board)
