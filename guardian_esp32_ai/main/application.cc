/**
 * Guardian ESP32-S3 Application
 * 
 * 简化版应用核心：WiFi 连接 → WebSocket → 音频对话
 * 去掉了 OTA、MCP、显示屏、多语言等功能
 */

#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "protocols/websocket_protocol.h"
#include "sensor_gateway.h"   // Sensor Gateway: UART(F407) -> JSON -> MQTT

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>

#define TAG "Application"

static const char* const STATE_STRINGS[] = {
    "unknown", "starting", "wifi_configuring", "idle", "connecting",
    "listening", "speaking", "upgrading", "activating", "audio_testing", "fatal_error"
};

// ── Constructor ───────────────────────────────────────────────
Application::Application() {
    event_group_ = xEventGroupCreate();
}

Application::~Application() {
    vEventGroupDelete(event_group_);
}

// ── Public helpers ────────────────────────────────────────────
void Application::Schedule(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    main_tasks_.push_back(std::move(callback));
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::SetDeviceState(DeviceState state) {
    device_state_ = state;
    ESP_LOGI(TAG, "State: %s", STATE_STRINGS[state]);
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "[%s] %s", status, message);
}

void Application::DismissAlert() {}

void Application::AbortSpeaking(AbortReason reason) {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->SendAbortSpeaking(reason);
    }
    audio_service_.ResetDecoder();
    SetDeviceState(kDeviceStateIdle);
}

void Application::Reboot() {
    esp_restart();
}

void Application::PlaySound(const std::string_view& sound) {
    // No OGG assets in this stripped build
}

// ── Toggle chat state (BOOT button) ──────────────────────────
void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateIdle) {
        StartListening();
    } else if (device_state_ == kDeviceStateListening) {
        StopListening();
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
}

void Application::StartListening() {
    Schedule([this]() {
        if (device_state_ != kDeviceStateIdle) return;

        // Open audio channel if not yet open
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                SetDeviceState(kDeviceStateIdle);
                return;
            }
        }

        SetDeviceState(kDeviceStateListening);
        audio_service_.EnableVoiceProcessing(true);
        protocol_->SendStartListening(listening_mode_);
        ESP_LOGI(TAG, "Listening started");
    });
}

void Application::StopListening() {
    Schedule([this]() {
        if (device_state_ != kDeviceStateListening) return;
        audio_service_.EnableVoiceProcessing(false);
        protocol_->SendStopListening();
        SetDeviceState(kDeviceStateIdle);
        ESP_LOGI(TAG, "Listening stopped");
    });
}

// ── Main startup ──────────────────────────────────────────────
void Application::Start() {
    SetDeviceState(kDeviceStateStarting);
    ESP_LOGI(TAG, "Guardian AI Assistant starting...");

    auto& board = Board::GetInstance();

    // 1. 连接 WiFi
    board.StartNetwork();
    ESP_LOGI(TAG, "WiFi connected");

    // 2. 启动传感器网关（UART 接收 F407 数据 → MQTT 上传 InfluxDB）
    //    在 WiFi 连通后立即启动，后台运行，不阻塞 AI 语音流程
    sensor_gateway_start();
    ESP_LOGI(TAG, "Sensor gateway started");

    // 3. 从 NVS 读取服务器地址（由 menuconfig / 初次配置写入）
    {
        Settings settings("websocket", true);
        std::string url = settings.GetString("url");
        if (url.empty()) {
            // 写入默认地址 (可通过 menuconfig 或 web 配置覆盖)
            url = CONFIG_GUARDIAN_WS_URL;
            settings.SetString("url", url);
        }
        // 协议版本
        int version = settings.GetInt("version");
        if (version == 0) {
            settings.SetInt("version", CONFIG_GUARDIAN_WS_VERSION);
        }
        // Token (可选)
        std::string token = settings.GetString("token");
        if (token.empty() && strlen(CONFIG_GUARDIAN_WS_TOKEN) > 0) {
            settings.SetString("token", CONFIG_GUARDIAN_WS_TOKEN);
        }
        ESP_LOGI(TAG, "WebSocket server: %s (protocol v%d)", url.c_str(),
                 settings.GetInt("version"));
    }

    // 3. 创建 WebSocket 协议实例
    auto proto = std::make_unique<WebsocketProtocol>();
    protocol_ = std::move(proto);

    // 4. 注册协议回调
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    });

    protocol_->OnIncomingJson([this](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) return;

        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state)) {
                if (strcmp(state->valuestring, "start") == 0) {
                    Schedule([this]() { SetDeviceState(kDeviceStateSpeaking); });
                } else if (strcmp(state->valuestring, "stop") == 0) {
                    Schedule([this]() {
                        // Wait for audio to finish then go idle
                        while (!audio_service_.IsIdle()) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                        }
                        SetDeviceState(kDeviceStateIdle);
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, "STT: %s", text->valuestring);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                ESP_LOGI(TAG, "Emotion: %s", emotion->valuestring);
            }
        }
    });

    protocol_->OnAudioChannelOpened([this]() {
        ESP_LOGI(TAG, "Audio channel opened");
        // Send audio from queue as soon as available
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    });

    protocol_->OnAudioChannelClosed([this]() {
        ESP_LOGI(TAG, "Audio channel closed");
        Schedule([this]() {
            audio_service_.EnableVoiceProcessing(false);
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnNetworkError([this](const std::string& msg) {
        ESP_LOGE(TAG, "Network error: %s", msg.c_str());
        Schedule([this]() { SetDeviceState(kDeviceStateIdle); });
    });

    // 5. 初始化音频
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);

    // 6. 注册音频回调
    AudioServiceCallbacks audio_cbs;
    audio_cbs.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    audio_cbs.on_wake_word_detected = nullptr;  // 无唤醒词
    audio_cbs.on_vad_change = [this](bool speaking) {
        if (speaking && device_state_ == kDeviceStateListening &&
            listening_mode_ == kListeningModeAutoStop) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
        }
    };
    audio_cbs.on_audio_testing_queue_full = nullptr;
    audio_service_.SetCallbacks(audio_cbs);
    audio_service_.Start();

    // 7. 启动协议
    protocol_->Start();

    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "Ready. Press BOOT button to talk.");

    // 8. 主事件循环（在当前任务中运行）
    MainEventLoop();
}

// ── Main event loop ───────────────────────────────────────────
void Application::MainEventLoop() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_,
            MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(100)
        );

        // 执行调度任务
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks = std::move(main_tasks_);
            }
            for (auto& task : tasks) {
                task();
            }
        }

        // 发送音频
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                    protocol_->SendAudio(std::move(packet));
                }
            }
        }
    }
}
