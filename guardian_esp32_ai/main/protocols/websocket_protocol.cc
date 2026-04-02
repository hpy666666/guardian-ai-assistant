/**
 * WebSocket Protocol implementation using ESP-IDF esp_websocket_client
 * (no dependency on 78/esp-wifi-connect WebSocket class)
 *
 * Binary protocol v3: type(1B) + reserved(1B) + payload_size(2B BE) + payload[]
 */
#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>

#define TAG "WS"
#define OPUS_FRAME_DURATION_MS 60

// ──────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ──────────────────────────────────────────────────────────────────────────

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    CloseAudioChannel();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// esp_websocket_client event handler (static)
// ──────────────────────────────────────────────────────────────────────────

void WebsocketProtocol::WsEventHandler(void* handler_args, esp_event_base_t base,
                                        int32_t event_id, void* event_data)
{
    auto* self  = static_cast<WebsocketProtocol*>(handler_args);
    auto* data  = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        self->connected_ = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        self->connected_ = false;
        if (self->on_audio_channel_closed_ != nullptr) {
            self->on_audio_channel_closed_();
        }
        break;

    case WEBSOCKET_EVENT_DATA: {
        bool is_binary = (data->op_code == 0x02);  // 0x02 = binary opcode
        if (is_binary) {
            const char* raw = data->data_ptr;
            size_t      len = data->data_len;

            if (self->version_ == 3 && len >= sizeof(BinaryProtocol3)) {
                auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(raw);
                uint16_t payload_size = ntohs(bp3->payload_size);
                auto payload_ptr = reinterpret_cast<const uint8_t*>(bp3->payload);

                if (bp3->type == 0) {
                    // type=0: Opus 音频帧
                    if (self->on_incoming_audio_ != nullptr) {
                        self->on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                            .sample_rate    = self->server_sample_rate_,
                            .frame_duration = self->server_frame_duration_,
                            .timestamp      = 0,
                            .payload        = std::vector<uint8_t>(payload_ptr, payload_ptr + payload_size)
                        }));
                    }
                } else if (bp3->type == 1) {
                    // type=1: JSON 控制消息（二进制帧包裹的 JSON）
                    if (payload_size > 0 && self->on_incoming_json_ != nullptr) {
                        std::string text(reinterpret_cast<const char*>(payload_ptr), payload_size);
                        auto root = cJSON_Parse(text.c_str());
                        if (root) {
                            auto type_item = cJSON_GetObjectItem(root, "type");
                            if (cJSON_IsString(type_item) &&
                                strcmp(type_item->valuestring, "hello") == 0) {
                                self->ParseServerHello(root);
                            } else {
                                self->on_incoming_json_(root);
                            }
                            cJSON_Delete(root);
                        }
                    }
                }
            } else if (self->version_ == 2 && len >= sizeof(BinaryProtocol2)) {
                auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(raw);
                uint32_t ts   = ntohl(bp2->timestamp);
                uint32_t psz  = ntohl(bp2->payload_size);
                auto payload_ptr = reinterpret_cast<const uint8_t*>(bp2->payload);
                if (self->on_incoming_audio_ != nullptr) {
                    self->on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate    = self->server_sample_rate_,
                        .frame_duration = self->server_frame_duration_,
                        .timestamp      = ts,
                        .payload        = std::vector<uint8_t>(payload_ptr, payload_ptr + psz)
                    }));
                }
            } else {
                // 裸 Opus 数据（无头部）
                if (self->on_incoming_audio_ != nullptr) {
                    self->on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate    = self->server_sample_rate_,
                        .frame_duration = self->server_frame_duration_,
                        .timestamp      = 0,
                        .payload        = std::vector<uint8_t>((uint8_t*)raw, (uint8_t*)raw + len)
                    }));
                }
            }
        } else {
            // Text frame
            // esp_websocket_client may deliver data in multiple chunks;
            // for simplicity we handle single-chunk text messages.
            if (data->data_len > 0) {
                // Make null-terminated copy
                std::string text(data->data_ptr, data->data_len);
                auto root = cJSON_Parse(text.c_str());
                if (root) {
                    auto type_item = cJSON_GetObjectItem(root, "type");
                    if (cJSON_IsString(type_item)) {
                        if (strcmp(type_item->valuestring, "hello") == 0) {
                            self->ParseServerHello(root);
                        } else {
                            if (self->on_incoming_json_ != nullptr) {
                                self->on_incoming_json_(root);
                            }
                        }
                    } else {
                        ESP_LOGE(TAG, "Missing message type, data: %s", text.c_str());
                    }
                    cJSON_Delete(root);
                }
            }
        }
        self->last_incoming_time_ = std::chrono::steady_clock::now();
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        self->SetError("Server error");
        break;

    default:
        break;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// OpenAudioChannel
// ──────────────────────────────────────────────────────────────────────────

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url   = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version       = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    if (url.empty()) {
        url = CONFIG_GUARDIAN_WS_URL;
    }

    error_occurred_ = false;
    connected_      = false;

    // Build extra headers string: "Authorization: Bearer <token>\r\nProtocol-Version: 3\r\n..."
    std::string headers;
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            token = "Bearer " + token;
        }
        headers += "Authorization: " + token + "\r\n";
    }
    headers += "Protocol-Version: " + std::to_string(version_) + "\r\n";
    headers += "Device-Id: " + SystemInfo::GetMacAddress() + "\r\n";
    headers += "Client-Id: " + Board::GetInstance().GetUuid() + "\r\n";

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri            = url.c_str();
    ws_cfg.headers        = headers.c_str();
    ws_cfg.reconnect_timeout_ms  = 0;    // do not auto-reconnect
    ws_cfg.network_timeout_ms    = 10000;
    ws_cfg.task_stack             = 8192;
    ws_cfg.buffer_size            = 4096;

    ws_client_ = esp_websocket_client_init(&ws_cfg);
    if (ws_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return false;
    }

    esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY,
                                  WsEventHandler, this);

    ESP_LOGI(TAG, "Connecting to %s (protocol v%d)", url.c_str(), version_);
    esp_err_t err = esp_websocket_client_start(ws_client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
        return false;
    }

    // Wait for CONNECTED event (up to 10 s)
    int waited = 0;
    while (!connected_ && waited < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited++;
    }
    if (!connected_) {
        ESP_LOGE(TAG, "WebSocket connect timeout");
        SetError("Server not connected");
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
        return false;
    }

    // Send hello
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello (up to 10 s)
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_,
        WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "No server hello received");
        SetError("Server timeout");
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// CloseAudioChannel
// ──────────────────────────────────────────────────────────────────────────

void WebsocketProtocol::CloseAudioChannel() {
    if (ws_client_ != nullptr) {
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_  = nullptr;
        connected_  = false;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// IsAudioChannelOpened
// ──────────────────────────────────────────────────────────────────────────

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return ws_client_ != nullptr && connected_ && !error_occurred_ && !IsTimeout();
}

// ──────────────────────────────────────────────────────────────────────────
// SendAudio
// ──────────────────────────────────────────────────────────────────────────

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        return false;
    }

    if (version_ == 3) {
        std::vector<uint8_t> buf(sizeof(BinaryProtocol3) + packet->payload.size());
        auto* bp3 = reinterpret_cast<BinaryProtocol3*>(buf.data());
        bp3->type         = 0;
        bp3->reserved     = 0;
        bp3->payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());
        int sent = esp_websocket_client_send_bin(ws_client_,
            reinterpret_cast<const char*>(buf.data()), buf.size(),
            pdMS_TO_TICKS(1000));
        return sent >= 0;
    } else if (version_ == 2) {
        std::vector<uint8_t> buf(sizeof(BinaryProtocol2) + packet->payload.size());
        auto* bp2 = reinterpret_cast<BinaryProtocol2*>(buf.data());
        bp2->version      = htons(static_cast<uint16_t>(version_));
        bp2->type         = 0;
        bp2->reserved     = 0;
        bp2->timestamp    = htonl(packet->timestamp);
        bp2->payload_size = htonl(static_cast<uint32_t>(packet->payload.size()));
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());
        int sent = esp_websocket_client_send_bin(ws_client_,
            reinterpret_cast<const char*>(buf.data()), buf.size(),
            pdMS_TO_TICKS(1000));
        return sent >= 0;
    } else {
        int sent = esp_websocket_client_send_bin(ws_client_,
            reinterpret_cast<const char*>(packet->payload.data()),
            packet->payload.size(),
            pdMS_TO_TICKS(1000));
        return sent >= 0;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// SendText
// ──────────────────────────────────────────────────────────────────────────

bool WebsocketProtocol::SendText(const std::string& text) {
    if (!IsAudioChannelOpened()) {
        return false;
    }
    int sent = esp_websocket_client_send_text(ws_client_,
        text.c_str(), text.size(), pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError("Server error");
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// GetHelloMessage
// ──────────────────────────────────────────────────────────────────────────

std::string WebsocketProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", false);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

// ──────────────────────────────────────────────────────────────────────────
// ParseServerHello
// ──────────────────────────────────────────────────────────────────────────

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || !cJSON_IsString(transport) ||
        strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport in server hello");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
