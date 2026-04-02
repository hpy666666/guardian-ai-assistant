/*
 * ESP32 Communication Module
 *
 * Send sensor data to ESP32-S3 via UART6 (PC6=TX, PC7=RX)
 * Format: JSON line ending with '\n'
 * Supports bidirectional communication:
 *   F407 TX (PC6) → ESP32 RX: sensor data, alerts
 *   ESP32 TX      → F407 RX (PC7): time sync packet from NTP
 */

#ifndef ESP_COMM_H
#define ESP_COMM_H

#include <rtthread.h>
#include "display_status.h"  /* For display_data_t */

/*---------------------------------------------------------------------------
 * Configuration
 *---------------------------------------------------------------------------*/
#define ESP_UART_NAME       "uart6"
#define ESP_UART_BAUDRATE   115200

/* Buffer sizes */
#define ESP_TX_BUF_SIZE     512     /* Increased from 256 for full JSON */
#define ESP_RX_BUF_SIZE     128     /* For receiving time sync / commands */

/* Timeouts */
#define ESP_ACK_TIMEOUT_MS  500     /* Wait 500ms for ACK */
#define ESP_RETRY_COUNT     2       /* Retry twice on failure */

/*---------------------------------------------------------------------------
 * RTC Time Structure (from NTP via ESP32)
 *---------------------------------------------------------------------------*/
typedef struct {
    rt_uint16_t year;    /* e.g. 2026 */
    rt_uint8_t  month;   /* 1-12 */
    rt_uint8_t  day;     /* 1-31 */
    rt_uint8_t  hour;    /* 0-23 */
    rt_uint8_t  min;     /* 0-59 */
    rt_uint8_t  sec;     /* 0-59 */
    rt_bool_t   valid;   /* RT_TRUE once synced from ESP32 */
} esp_rtc_time_t;

/*---------------------------------------------------------------------------
 * API Functions
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize ESP32 communication module
 *         Also starts RX thread for receiving time sync packets
 * @return RT_EOK on success, error code on failure
 */
rt_err_t esp_comm_init(void);

/**
 * @brief  Send sensor data to ESP32
 *         Data will be formatted as JSON and sent via UART
 * @param  data  Pointer to sensor data structure
 * @return RT_EOK on success, error code on failure
 */
rt_err_t esp_comm_send_data(const display_data_t *data);

/**
 * @brief  Send alert event to ESP32
 * @param  alert_type  "fall" or "gas"
 * @return RT_EOK on success
 */
rt_err_t esp_comm_send_alert(const char *alert_type);

/**
 * @brief  Check if ESP32 communication is ready
 * @return RT_TRUE if ready, RT_FALSE otherwise
 */
rt_bool_t esp_comm_is_ready(void);

/**
 * @brief  Get the latest RTC time received from ESP32 NTP
 * @param  out  Pointer to fill with current time
 * @return RT_TRUE if time has been synced, RT_FALSE if still using default
 */
rt_bool_t esp_comm_get_time(esp_rtc_time_t *out);

/**
 * @brief  Get transmission statistics
 */
void esp_comm_get_stats(rt_uint32_t *tx_count, rt_uint32_t *ack_count, rt_uint32_t *fail_count);

#endif /* ESP_COMM_H */
