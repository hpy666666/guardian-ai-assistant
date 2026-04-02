/*
 * ESP32 Communication Module
 *
 * Send sensor data to ESP32-S3 via UART6 (PC6=TX, PC7=RX)
 * Format: JSON line ending with '\n'
 * Supports bidirectional communication:
 *   F407 TX (PC6) → ESP32 RX: sensor data, alerts
 *   ESP32 TX      → F407 RX (PC7): time sync packet {"t":"time",...}
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>
#include "esp_comm.h"
#include "lte_driver.h"
#include "data_logger.h"

#define DBG_TAG "esp_comm"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Private Variables
 *---------------------------------------------------------------------------*/
static rt_device_t s_uart_dev = RT_NULL;
static rt_bool_t s_initialized = RT_FALSE;

/* TX buffer */
static char s_tx_buf[ESP_TX_BUF_SIZE];

/* Statistics */
static rt_uint32_t s_tx_count = 0;
static rt_uint32_t s_ack_count = 0;
static rt_uint32_t s_fail_count = 0;

/* RTC time from ESP32 NTP */
static esp_rtc_time_t s_rtc_time = {
    .year  = 2026,
    .month = 3,
    .day   = 25,
    .hour  = 0,
    .min   = 0,
    .sec   = 0,
    .valid = RT_FALSE   /* becomes RT_TRUE after first NTP sync */
};
static struct rt_mutex s_time_mutex;

/* RX thread for receiving time sync from ESP32 */
static rt_thread_t s_rx_thread = RT_NULL;
static char s_rx_line[ESP_RX_BUF_SIZE];
static int  s_rx_pos = 0;

/*---------------------------------------------------------------------------
 * Private: Parse time packet from ESP32
 * Expected: {"t":"time","year":2026,"month":3,"day":25,"hour":14,"min":30,"sec":0}
 *---------------------------------------------------------------------------*/
static void parse_time_packet(const char *json)
{
    /* Simple hand-rolled parser to avoid pulling in cJSON */
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

    /* Check it's a time packet */
    if (strstr(json, "\"t\":\"time\"") == RT_NULL) {
        return;
    }

    /* Extract each field with sscanf-style search */
    const char *p;

#define EXTRACT_INT(key, var) \
    p = strstr(json, "\"" key "\":"); \
    if (p) { p += strlen("\"" key "\":"); var = atoi(p); }

    EXTRACT_INT("year",  year)
    EXTRACT_INT("month", month)
    EXTRACT_INT("day",   day)
    EXTRACT_INT("hour",  hour)
    EXTRACT_INT("min",   min)
    EXTRACT_INT("sec",   sec)

#undef EXTRACT_INT

    /* Sanity check */
    if (year < 2024 || year > 2099 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        LOG_W("Time packet invalid: %d-%02d-%02d %02d:%02d:%02d",
              year, month, day, hour, min, sec);
        return;
    }

    rt_mutex_take(&s_time_mutex, RT_WAITING_FOREVER);
    s_rtc_time.year  = (rt_uint16_t)year;
    s_rtc_time.month = (rt_uint8_t)month;
    s_rtc_time.day   = (rt_uint8_t)day;
    s_rtc_time.hour  = (rt_uint8_t)hour;
    s_rtc_time.min   = (rt_uint8_t)min;
    s_rtc_time.sec   = (rt_uint8_t)sec;
    s_rtc_time.valid = RT_TRUE;
    rt_mutex_release(&s_time_mutex);

    LOG_I("RTC updated from ESP32: %04d-%02d-%02d %02d:%02d:%02d",
          year, month, day, hour, min, sec);

    /* Notify data_logger to re-open with new filename */
    logger_update_time(year, month, day);
}

/*---------------------------------------------------------------------------
 * RX thread: read bytes from UART6, parse line-based JSON
 *---------------------------------------------------------------------------*/
static void esp_rx_thread_entry(void *param)
{
    (void)param;
    char byte;

    while (1) {
        rt_size_t n = rt_device_read(s_uart_dev, 0, &byte, 1);
        if (n != 1) {
            rt_thread_mdelay(5);
            continue;
        }

        if (byte == '\n') {
            if (s_rx_pos > 0) {
                s_rx_line[s_rx_pos] = '\0';
                parse_time_packet(s_rx_line);
                s_rx_pos = 0;
            }
        } else if (byte == '\r') {
            /* ignore CR */
        } else {
            if (s_rx_pos < (int)sizeof(s_rx_line) - 1) {
                s_rx_line[s_rx_pos++] = byte;
            } else {
                /* overflow, discard */
                s_rx_pos = 0;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------------------------*/

rt_err_t esp_comm_init(void)
{
    if (s_initialized) {
        return RT_EOK;
    }

    /* Init time mutex */
    rt_mutex_init(&s_time_mutex, "esp_time", RT_IPC_FLAG_PRIO);

    /* Find UART device */
    s_uart_dev = rt_device_find(ESP_UART_NAME);
    if (s_uart_dev == RT_NULL) {
        LOG_E("UART '%s' not found - check BSP_USING_UART6 in board.h", ESP_UART_NAME);
        return -RT_ENOSYS;
    }

    /* Configure UART: 115200 8N1 */
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    config.baud_rate = ESP_UART_BAUDRATE;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
    config.parity    = PARITY_NONE;

    rt_err_t ret = rt_device_control(s_uart_dev, RT_DEVICE_CTRL_CONFIG, &config);
    if (ret != RT_EOK) {
        LOG_W("UART config failed, using default");
    }

    /* Open UART in bidirectional mode */
    ret = rt_device_open(s_uart_dev, RT_DEVICE_OFLAG_RDWR);
    if (ret != RT_EOK) {
        LOG_E("Failed to open UART '%s'", ESP_UART_NAME);
        s_uart_dev = RT_NULL;
        return ret;
    }

    s_initialized = RT_TRUE;

    /* Start RX thread to receive time sync packets from ESP32 */
    s_rx_thread = rt_thread_create("esp_rx",
                                   esp_rx_thread_entry, RT_NULL,
                                   512, 18, 10);
    if (s_rx_thread != RT_NULL) {
        rt_thread_startup(s_rx_thread);
        LOG_I("ESP32 RX thread started (time sync listener)");
    } else {
        LOG_W("Failed to create ESP32 RX thread");
    }

    LOG_I("ESP32 comm ready on %s @ %d baud (bidirectional)", ESP_UART_NAME, ESP_UART_BAUDRATE);

    return RT_EOK;
}

rt_bool_t esp_comm_is_ready(void)
{
    return s_initialized;
}

rt_err_t esp_comm_send_data(const display_data_t *data)
{
    if (!s_initialized || data == RT_NULL) {
        return -RT_ERROR;
    }

    /*
     * Build compact JSON:
     *   t: type ("data")
     *   hr: heart rate
     *   spo2: blood oxygen
     *   temp: temperature * 10 (integer, 0.1C resolution)
     *   hum: humidity
     *   mq4/mq7: gas sensor mV
     *   lat/lon: GPS * 100000 (integer)
     *   lux: light intensity
     *   fall: fall state (0=ok, 1=impact, 2=check, 3=fallen)
     *   roll/pitch: IMU angles (integer)
     *   accmag: acceleration magnitude * 100
     *   sd: SD card status (0=fail, 1=ok)
     *   lte: 4G module status (0=disconnected, 1=connected)
     */
    int len = snprintf(s_tx_buf, ESP_TX_BUF_SIZE,
        "{\"t\":\"data\""
        ",\"hr\":%d"
        ",\"spo2\":%d"
        ",\"temp\":%d"
        ",\"hum\":%u"
        ",\"lux\":%u"
        ",\"lat\":%d"
        ",\"lon\":%d"
        ",\"mq4\":%u"
        ",\"mq7\":%u"
        ",\"roll\":%d"
        ",\"pitch\":%d"
        ",\"accmag\":%d"
        ",\"fall\":%u"
        ",\"sd\":%u"
        ",\"lte\":%u"
        "}\n",
        data->hr.hr_valid ? data->hr.heart_rate : 0,
        data->hr.spo2_valid ? data->hr.spo2 : 0,
        data->env.valid ? data->env.temperature / 10 : 0,  /* 0.01C -> 0.1C */
        data->env.valid ? (data->env.humidity >> 10) : 0,  /* Q22.10 -> int */
        data->light.valid ? data->light.lux : 0,
        data->gps.valid ? (int)(data->gps.latitude * 100000) : 0,
        data->gps.valid ? (int)(data->gps.longitude * 100000) : 0,
        data->gas.valid ? data->gas.mq4_mv : 0,
        data->gas.valid ? data->gas.mq7_mv : 0,
        data->imu.valid ? (int)data->imu.roll : 0,
        data->imu.valid ? (int)data->imu.pitch : 0,
        data->imu.valid ? (int)(data->imu.accel_mag * 100) : 0,
        data->imu.fall_state,
        logger_is_ready() ? 1 : 0,   /* SD card status from logger */
        0                            /* 4G LTE status - DISABLED (Air780E firmware incompatible) */
    );

    if (len <= 0 || len >= ESP_TX_BUF_SIZE) {
        LOG_E("JSON format error (len=%d, max=%d)", len, ESP_TX_BUF_SIZE);
        s_fail_count++;
        return -RT_ERROR;
    }

    /* Send via UART with retry */
    for (int retry = 0; retry <= ESP_RETRY_COUNT; retry++) {
        rt_size_t sent = rt_device_write(s_uart_dev, 0, s_tx_buf, len);
        if (sent == len) {
            s_tx_count++;
            s_ack_count++;  /* Assume success for now (no ACK from ESP32 yet) */
            LOG_D("TX[%u]: %s", s_tx_count, s_tx_buf);
            return RT_EOK;
        }

        if (retry < ESP_RETRY_COUNT) {
            LOG_W("UART write incomplete (%d/%d), retry %d", sent, len, retry + 1);
            rt_thread_mdelay(10);
        }
    }

    s_fail_count++;
    LOG_E("UART write failed after %d retries", ESP_RETRY_COUNT);
    return -RT_ERROR;
}

rt_err_t esp_comm_send_alert(const char *alert_type)
{
    if (!s_initialized || alert_type == RT_NULL) {
        return -RT_ERROR;
    }

    int len = snprintf(s_tx_buf, ESP_TX_BUF_SIZE,
        "{\"t\":\"alert\",\"type\":\"%s\"}\n",
        alert_type);

    if (len <= 0 || len >= ESP_TX_BUF_SIZE) {
        s_fail_count++;
        return -RT_ERROR;
    }

    /* Alerts are important, retry more aggressively */
    for (int retry = 0; retry <= ESP_RETRY_COUNT + 1; retry++) {
        rt_size_t sent = rt_device_write(s_uart_dev, 0, s_tx_buf, len);
        if (sent == len) {
            s_tx_count++;
            s_ack_count++;
            LOG_I("Alert sent: %s", alert_type);
            return RT_EOK;
        }

        if (retry < ESP_RETRY_COUNT + 1) {
            LOG_W("Alert send incomplete, retry %d", retry + 1);
            rt_thread_mdelay(20);
        }
    }

    s_fail_count++;
    LOG_E("Alert send failed: %s", alert_type);
    return -RT_ERROR;
}

/*---------------------------------------------------------------------------
 * Statistics Functions
 *---------------------------------------------------------------------------*/
void esp_comm_get_stats(rt_uint32_t *tx_count, rt_uint32_t *ack_count, rt_uint32_t *fail_count)
{
    if (tx_count) *tx_count = s_tx_count;
    if (ack_count) *ack_count = s_ack_count;
    if (fail_count) *fail_count = s_fail_count;
}

/*---------------------------------------------------------------------------
 * RTC Time Access
 *---------------------------------------------------------------------------*/
rt_bool_t esp_comm_get_time(esp_rtc_time_t *out)
{
    if (out == RT_NULL) return RT_FALSE;
    rt_mutex_take(&s_time_mutex, RT_WAITING_FOREVER);
    *out = s_rtc_time;
    rt_mutex_release(&s_time_mutex);
    return s_rtc_time.valid;
}

/*---------------------------------------------------------------------------
 * MSH Debug Commands
 *---------------------------------------------------------------------------*/
#ifdef RT_USING_FINSH
#include <finsh.h>

/**
 * Send test data to ESP32
 * Usage: esp_test
 */
static void esp_test(void)
{
    if (!s_initialized) {
        rt_kprintf("ESP comm not initialized\n");
        return;
    }

    /* Create dummy test data */
    display_data_t test_data = {0};
    test_data.hr.hr_valid = 1;
    test_data.hr.heart_rate = 75;
    test_data.hr.spo2_valid = 1;
    test_data.hr.spo2 = 98;
    test_data.env.valid = 1;
    test_data.env.temperature = 2350;  /* 23.50 C */
    test_data.env.humidity = 55 << 10; /* 55% in Q22.10 */
    test_data.gas.valid = 1;
    test_data.gas.mq4_mv = 120;
    test_data.gas.mq7_mv = 80;
    test_data.gps.valid = 1;
    test_data.gps.latitude = 39.9042;
    test_data.gps.longitude = 116.4074;
    test_data.light.valid = 1;
    test_data.light.lux = 500;
    test_data.imu.fall_state = 0;

    if (esp_comm_send_data(&test_data) == RT_EOK) {
        rt_kprintf("Test data sent OK\n");
    } else {
        rt_kprintf("Send failed\n");
    }
}
MSH_CMD_EXPORT(esp_test, Send test data to ESP32);

/**
 * Send test alert to ESP32
 * Usage: esp_alert fall
 *        esp_alert gas
 */
static void esp_alert(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("Usage: esp_alert <fall|gas>\n");
        return;
    }

    if (esp_comm_send_alert(argv[1]) == RT_EOK) {
        rt_kprintf("Alert '%s' sent OK\n", argv[1]);
    } else {
        rt_kprintf("Send failed\n");
    }
}
MSH_CMD_EXPORT(esp_alert, Send alert to ESP32: esp_alert <fall|gas>);

#endif /* RT_USING_FINSH */
