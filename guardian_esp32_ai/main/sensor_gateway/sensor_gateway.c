/**
 * Sensor Gateway — UART(F407) → JSON → MQTT
 *
 * 移植自 guardian_esp32/main/main.c
 * 去掉了 WiFi 初始化（由 guardian_esp32_ai 的 WifiBoard 负责），
 * 其余逻辑完全保留：UART 接收、JSON 解析、MQTT 发布、NTP 时间同步。
 *
 * 后台任务：
 *   gateway_start_task  —— 初始化 MQTT，启动 uart_rx_task，自删除
 *   uart_rx_task        —— 持续接收 F407 UART 数据，解析并发布 MQTT
 *   ntp_sync_task       —— NTP 时间同步，完成后发时间给 F407，自删除
 */

#include "sensor_gateway.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

/* ──────────────────────────────────────────
 * Kconfig 默认值（在 guardian_esp32_ai/main/Kconfig.projbuild 中定义）
 * ────────────────────────────────────────── */
#define MQTT_BROKER_URI     CONFIG_GW_MQTT_BROKER_URI
#define MQTT_USERNAME       CONFIG_GW_MQTT_USERNAME
#define MQTT_PASSWORD       CONFIG_GW_MQTT_PASSWORD

/* UART */
#define UART_PORT_NUM       UART_NUM_1
#define UART_BAUD_RATE      115200
#define UART_TX_PIN         GPIO_NUM_17
#define UART_RX_PIN         GPIO_NUM_16
#define UART_BUF_SIZE       512

/* 日志 TAG */
static const char *TAG = "sensor_gw";

/* ──────────────────────────────────────────
 * 运行时配置（从 NVS 读取）
 * ────────────────────────────────────────── */
static char s_device_id[32];   /* MQTT topic 中的设备 ID */

/* ──────────────────────────────────────────
 * MQTT 状态
 * ────────────────────────────────────────── */
#define MQTT_CONNECTED_BIT  BIT0
static EventGroupHandle_t s_mqtt_event_group = NULL;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ──────────────────────────────────────────
 * UART 接收缓冲
 * ────────────────────────────────────────── */
#define RX_LINE_BUF_SIZE    512
static char s_rx_line[RX_LINE_BUF_SIZE];
static int  s_rx_line_pos = 0;

/* ──────────────────────────────────────────
 * 前向声明
 * ────────────────────────────────────────── */
static void mqtt_publish_qos(const char *topic_suffix, const char *payload, int qos);
static void send_time_to_f407(void);

/* ═══════════════════════════════════════════
 * NVS 配置读取
 * ═══════════════════════════════════════════ */
static void load_device_id_from_nvs(void)
{
    /* 先填充 Kconfig 默认值 */
    strncpy(s_device_id, CONFIG_GW_MQTT_DEVICE_ID_DEFAULT, sizeof(s_device_id) - 1);

    nvs_handle_t h;
    if (nvs_open("mqtt", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_device_id);
        if (nvs_get_str(h, "device_id", s_device_id, &len) == ESP_OK && len > 1) {
            ESP_LOGI(TAG, "device_id from NVS: %s", s_device_id);
        } else {
            ESP_LOGI(TAG, "device_id from Kconfig default: %s", s_device_id);
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "mqtt NVS namespace not found, using Kconfig default: %s", s_device_id);
    }
}

/* ═══════════════════════════════════════════
 * MQTT 事件处理
 * ═══════════════════════════════════════════ */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected!");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected, will auto-reconnect");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  TCP err=0x%x  TLS err=0x%x",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err);
        }
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════
 * MQTT 初始化
 * ═══════════════════════════════════════════ */
static esp_err_t mqtt_init(void)
{
    s_mqtt_event_group = xEventGroupCreate();

    char client_id[48];
    snprintf(client_id, sizeof(client_id), "esp32-%s-%lu",
             s_device_id, (unsigned long)(esp_random() & 0xFFFF));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                     = MQTT_BROKER_URI,
        .credentials.username                   = MQTT_USERNAME,
        .credentials.authentication.password    = MQTT_PASSWORD,
        .credentials.client_id                  = client_id,
        .broker.verification.crt_bundle_attach  = esp_crt_bundle_attach,
        .network.timeout_ms                     = 10000,
        .session.keepalive                      = 60,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));

    /* 等待首次连接（最多 15 秒，失败不阻断，后台自动重连） */
    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group, MQTT_CONNECTED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & MQTT_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "MQTT initial connect timeout, will keep retrying in background");
    return ESP_ERR_TIMEOUT;  /* 非致命，自动重连 */
}

/* ═══════════════════════════════════════════
 * UART 初始化
 * ═══════════════════════════════════════════ */
static esp_err_t uart_init_gw(void)
{
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d ready: RX=GPIO%d TX=GPIO%d %d baud",
             UART_PORT_NUM, UART_RX_PIN, UART_TX_PIN, UART_BAUD_RATE);
    return ESP_OK;
}

/* ═══════════════════════════════════════════
 * MQTT 发布
 * ═══════════════════════════════════════════ */
static void mqtt_publish_qos(const char *topic_suffix, const char *payload, int qos)
{
    if (!s_mqtt_event_group) return;
    if (!(xEventGroupGetBits(s_mqtt_event_group) & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT not connected, skip publish");
        return;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "guardian/%s/%s", s_device_id, topic_suffix);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, 0);
    if (qos > 0) {
        ESP_LOGI(TAG, "-> %s (QoS%d, id=%d)", topic, qos, msg_id);
    } else {
        ESP_LOGI(TAG, "-> %s %s", topic, payload);
    }
}

static void mqtt_publish(const char *topic_suffix, const char *payload)
{
    mqtt_publish_qos(topic_suffix, payload, 0);
}

/* ═══════════════════════════════════════════
 * 发送时间到 F407
 * ═══════════════════════════════════════════ */
static void send_time_to_f407(void)
{
    time_t now;
    struct tm t;
    char buf[96];

    time(&now);
    localtime_r(&now, &t);

    if (t.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "Time not valid yet, skip sending to F407");
        return;
    }
    int len = snprintf(buf, sizeof(buf),
        "{\"t\":\"time\",\"year\":%d,\"month\":%d,\"day\":%d,"
        "\"hour\":%d,\"min\":%d,\"sec\":%d}\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    uart_write_bytes(UART_PORT_NUM, buf, len);
    ESP_LOGI(TAG, "Time sent to F407: %s", buf);
}

/* ═══════════════════════════════════════════
 * NTP 同步任务（失败后每 60 秒重试，直到成功）
 * ═══════════════════════════════════════════ */
static void ntp_sync_task(void *arg)
{
    ESP_LOGI(TAG, "NTP sync task started");

    /* 先等 MQTT 稳定（MQTT 本身要 1~2 秒连上，此时 DNS 也肯定可用了） */
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (1) {
        ESP_LOGI(TAG, "NTP sync attempt...");
        esp_sntp_stop();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp1.aliyun.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");
        esp_sntp_init();

        setenv("TZ", "CST-8", 1);
        tzset();

        /* 等待最多 30 秒（60 × 500ms） */
        int retry = 0;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retry < 60) {
            vTaskDelay(pdMS_TO_TICKS(500));
            retry++;
        }

        if (sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGI(TAG, "NTP sync OK (retry=%d)", retry);
            send_time_to_f407();
            esp_sntp_stop();
            vTaskDelete(NULL);  /* 成功，退出 */
            return;
        }

        /* 本轮失败，停止 SNTP，等 60 秒后重试 */
        esp_sntp_stop();
        ESP_LOGW(TAG, "NTP sync failed, retry in 60s");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* ═══════════════════════════════════════════
 * 解析 F407 JSON 并发布 MQTT
 * ═══════════════════════════════════════════ */
static void process_f407_json(const char *json_str)
{
    ESP_LOGI(TAG, "RX: %s", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse error");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "t");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    char payload[256];

    if (strcmp(type->valuestring, "data") == 0) {

        /* MAX30102: heartrate + spo2 */
        cJSON *hr   = cJSON_GetObjectItem(root, "hr");
        cJSON *spo2 = cJSON_GetObjectItem(root, "spo2");
        if (hr && spo2 && cJSON_IsNumber(hr) && cJSON_IsNumber(spo2)) {
            snprintf(payload, sizeof(payload),
                     "{\"heartrate\": %d, \"spo2\": %d}", hr->valueint, spo2->valueint);
            mqtt_publish("heartrate", payload);
        }

        /* BME280: temperature + humidity */
        cJSON *temp = cJSON_GetObjectItem(root, "temp");
        cJSON *hum  = cJSON_GetObjectItem(root, "hum");
        if (temp && hum && cJSON_IsNumber(temp) && cJSON_IsNumber(hum)) {
            float temp_f = temp->valueint / 10.0f;
            snprintf(payload, sizeof(payload),
                     "{\"temperature\": %.1f, \"humidity\": %d}", temp_f, hum->valueint);
            mqtt_publish("env", payload);
        }

        /* BH1750: light */
        cJSON *lux = cJSON_GetObjectItem(root, "lux");
        if (lux && cJSON_IsNumber(lux)) {
            snprintf(payload, sizeof(payload), "{\"lux\": %d}", lux->valueint);
            mqtt_publish("light", payload);
        }

        /* MQ-4 + MQ-7: gas */
        cJSON *mq4 = cJSON_GetObjectItem(root, "mq4");
        cJSON *mq7 = cJSON_GetObjectItem(root, "mq7");
        if (mq4 && mq7 && cJSON_IsNumber(mq4) && cJSON_IsNumber(mq7)) {
            snprintf(payload, sizeof(payload),
                     "{\"mq4_mv\": %d, \"mq7_mv\": %d}", mq4->valueint, mq7->valueint);
            mqtt_publish("gas", payload);
        }

        /* GPS: location */
        cJSON *lat = cJSON_GetObjectItem(root, "lat");
        cJSON *lon = cJSON_GetObjectItem(root, "lon");
        if (lat && lon && cJSON_IsNumber(lat) && cJSON_IsNumber(lon) &&
            lat->valuedouble != 0.0 && lon->valuedouble != 0.0) {
            double lat_f = lat->valuedouble / 100000.0;
            double lon_f = lon->valuedouble / 100000.0;
            snprintf(payload, sizeof(payload),
                     "{\"lat\": %.6f, \"lon\": %.6f}", lat_f, lon_f);
            mqtt_publish("location", payload);
        }

        /* MPU6050: IMU */
        cJSON *roll   = cJSON_GetObjectItem(root, "roll");
        cJSON *pitch  = cJSON_GetObjectItem(root, "pitch");
        cJSON *accmag = cJSON_GetObjectItem(root, "accmag");
        cJSON *fall   = cJSON_GetObjectItem(root, "fall");
        if (roll && pitch && accmag) {
            float mag_f = accmag->valueint / 100.0f;
            snprintf(payload, sizeof(payload),
                     "{\"roll\": %d, \"pitch\": %d, \"accel_mag\": %.2f, \"fall_state\": %d}",
                     roll->valueint, pitch->valueint, mag_f,
                     fall ? fall->valueint : 0);
            mqtt_publish("imu", payload);
        }

        /* Device status */
        cJSON *sd  = cJSON_GetObjectItem(root, "sd");
        cJSON *lte = cJSON_GetObjectItem(root, "lte");
        snprintf(payload, sizeof(payload),
                 "{\"status\": \"online\", \"battery\": 100, \"sd\": %d, \"lte\": %d}",
                 sd ? sd->valueint : 0, lte ? lte->valueint : 0);
        mqtt_publish("status", payload);

    } else if (strcmp(type->valuestring, "alert") == 0) {

        cJSON *alert_type = cJSON_GetObjectItem(root, "type");
        if (alert_type && cJSON_IsString(alert_type)) {
            char topic[32];
            if (strcmp(alert_type->valuestring, "fall") == 0) {
                snprintf(topic, sizeof(topic), "alert/fall");
                snprintf(payload, sizeof(payload),
                         "{\"type\": \"fall\", \"confidence\": 0.95, \"uptime_s\": %lu}",
                         (unsigned long)(esp_timer_get_time() / 1000000));
                mqtt_publish_qos(topic, payload, 1);
                ESP_LOGW(TAG, "!!! FALL ALERT (QoS1) !!!");
            } else if (strcmp(alert_type->valuestring, "gas") == 0) {
                snprintf(topic, sizeof(topic), "alert/gas");
                snprintf(payload, sizeof(payload),
                         "{\"type\": \"gas\", \"uptime_s\": %lu}",
                         (unsigned long)(esp_timer_get_time() / 1000000));
                mqtt_publish_qos(topic, payload, 1);
                ESP_LOGW(TAG, "!!! GAS ALERT (QoS1) !!!");
            }
        }
    }

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════
 * UART 接收任务
 * ═══════════════════════════════════════════ */
static void uart_rx_task(void *arg)
{
    uint8_t byte;
    uint32_t idle_counter = 0;
    ESP_LOGI(TAG, "uart_rx_task started, waiting for F407 data on GPIO%d...", UART_RX_PIN);

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            idle_counter = 0;
            if (byte == '\n') {
                if (s_rx_line_pos > 0) {
                    s_rx_line[s_rx_line_pos] = '\0';
                    process_f407_json(s_rx_line);
                    s_rx_line_pos = 0;
                }
            } else if (byte != '\r') {
                if (s_rx_line_pos < RX_LINE_BUF_SIZE - 1) {
                    s_rx_line[s_rx_line_pos++] = (char)byte;
                } else {
                    /* 缓冲区溢出：丢弃最旧的 64 字节 */
                    memmove(s_rx_line, s_rx_line + 64, RX_LINE_BUF_SIZE - 64);
                    s_rx_line_pos = RX_LINE_BUF_SIZE - 64;
                    s_rx_line[s_rx_line_pos++] = (char)byte;
                }
            }
        } else {
            /* 每 30 秒没收到任何字节，打印一次等待提示，帮助排查 F407 未发送问题 */
            idle_counter++;
            if (idle_counter >= 300) {   /* 300 × 100ms = 30s */
                ESP_LOGW(TAG, "No data from F407 for 30s. Check: 1) F407 running? "
                              "2) UART wiring PC6->GPIO16, GND->GND? "
                              "3) Baud rate 115200?");
                idle_counter = 0;
            }
        }
    }
}

/* ═══════════════════════════════════════════
 * 网关启动任务（一次性，执行完自删除）
 * ═══════════════════════════════════════════ */
static void gateway_start_task(void *arg)
{
    ESP_LOGI(TAG, "Sensor Gateway starting...");

    /* 读取 NVS device_id */
    load_device_id_from_nvs();

    /* UART 初始化 */
    uart_init_gw();

    /* MQTT 初始化（不成功也继续，后台自动重连） */
    mqtt_init();

    /* NTP 时间同步（后台，完成后自删除） */
    xTaskCreate(ntp_sync_task, "ntp_sync", 3072, NULL, 5, NULL);

    /* 启动 UART 接收循环 */
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Sensor Gateway started: device_id=%s", s_device_id);
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════
 * 公共接口
 * ═══════════════════════════════════════════ */
void sensor_gateway_start(void)
{
    /* 用独立任务启动，不阻塞 WiFi 回调线程 */
    xTaskCreate(gateway_start_task, "gw_start", 4096, NULL, 5, NULL);
}
