/**
 * WifiBoard implementation
 * Uses ESP-IDF native WiFi APIs (no 78/esp-wifi-connect WifiStation dependency).
 * WiFi credentials stored in NVS under namespace "wifi", keys "ssid"/"password".
 */
#include "wifi_board.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <cJSON.h>
#include <lwip/ip4_addr.h>
#include <string.h>

static const char* TAG = "WifiBoard";

// ──────────────────────────────────────────────────────────────────────────
// Internal state for native WiFi connection
// ──────────────────────────────────────────────────────────────────────────

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5

static EventGroupHandle_t s_wifi_event_group = nullptr;
static int                s_retry_count      = 0;
static std::string        s_ip_address;
static std::string        s_connected_ssid;
static int                s_rssi             = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_count < WIFI_MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "Retrying WiFi... (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
            } else {
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_ip_address = ip_str;
        s_retry_count = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        // Read RSSI after connect
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
            s_connected_ssid = (const char*)ap_info.ssid;
        }
        ESP_LOGI(TAG, "Got IP: %s, SSID: %s, RSSI: %d",
                 ip_str, s_connected_ssid.c_str(), s_rssi);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// WifiBoard constructor / board type
// ──────────────────────────────────────────────────────────────────────────

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set, resetting to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

// ──────────────────────────────────────────────────────────────────────────
// Config-AP mode (fallback when no credentials stored)
// We start a simple SoftAP so the user can connect and enter WiFi credentials
// via the web config page served by the application.
// ──────────────────────────────────────────────────────────────────────────

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    // Build AP SSID: "Guardian-XXXX" from last 4 bytes of MAC
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Guardian-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len       = strlen(ap_ssid);
    ap_config.ap.channel        = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    std::string hint = "Connect to hotspot: ";
    hint += ap_ssid;
    hint += ", then open 192.168.4.1";
    ESP_LOGI(TAG, "%s", hint.c_str());
    application.Alert("WiFi Config", hint.c_str(), "gear", "");

    // Stay in config mode forever (restart required to leave)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ──────────────────────────────────────────────────────────────────────────
// StartNetwork — connect to stored WiFi, fall back to config-AP mode
// ──────────────────────────────────────────────────────────────────────────

void WifiBoard::StartNetwork() {
    if (wifi_config_mode_) {
        // Initialize WiFi driver first
        ESP_ERROR_CHECK(esp_netif_init());
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        EnterWifiConfigMode();
        return;
    }

    // Read credentials from NVS
    Settings settings("wifi", false);
    std::string ssid     = settings.GetString("ssid");
    std::string password = settings.GetString("password");

    if (ssid.empty()) {
        ESP_LOGW(TAG, "No WiFi credentials found, entering config mode");
        wifi_config_mode_ = true;
        // Initialize WiFi driver first
        ESP_ERROR_CHECK(esp_netif_init());
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        EnterWifiConfigMode();
        return;
    }

    // ── Initialize TCP/IP stack & WiFi driver ──────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid,     ssid.c_str(),     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable    = true;
    wifi_config.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s ...", ssid.c_str());

    // Wait up to 60 s for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(60 * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return;
    }

    // Connection failed → config mode
    ESP_LOGE(TAG, "WiFi connect failed, entering config mode");
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    wifi_config_mode_ = true;
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg2 = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg2));
    EnterWifiConfigMode();
}

// ──────────────────────────────────────────────────────────────────────────
// Misc helpers
// ──────────────────────────────────────────────────────────────────────────

const char* WifiBoard::GetNetworkStateIcon() {
    return "";
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void WifiBoard::ResetWifiConfiguration() {
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ──────────────────────────────────────────────────────────────────────────
// JSON helpers used by the Protocol hello handshake
// ──────────────────────────────────────────────────────────────────────────

std::string WifiBoard::GetBoardJson() {
    std::string board_json = "{";
    board_json += "\"type\":\"wifi\",";
    board_json += "\"name\":\"Guardian-ESP32-S3\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\""  + s_connected_ssid + "\",";
        board_json += "\"rssi\":"    + std::to_string(s_rssi)  + ",";
        board_json += "\"ip\":\""    + s_ip_address      + "\",";
    }
    board_json += "\"mac\":\""   + SystemInfo::GetMacAddress() + "\"";
    board_json += "}";
    return board_json;
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board      = Board::GetInstance();
    auto  root       = cJSON_CreateObject();

    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec   = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type",  "wifi");
    cJSON_AddStringToObject(network, "ssid",  s_connected_ssid.c_str());
    cJSON_AddNumberToObject(network, "rssi",  s_rssi);
    cJSON_AddStringToObject(network, "ip",    s_ip_address.c_str());
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
