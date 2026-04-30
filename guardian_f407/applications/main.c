/*
 * Guardian - Elderly Safety Monitoring System
 * Main Application Entry
 *
 * Features:
 *   - Heart rate & SpO2 monitoring (MAX30102)
 *   - Environmental sensing (BME280: temp/hum/press)
 *   - Ambient light sensing (BH1750)
 *   - GPS positioning (ATGM336H)
 *   - Fall detection (MPU6050/6500)
 *   - Gas leak detection (MQ-4 methane, MQ-7 CO)
 *   - OLED status display (SSD1306)
 *   - Watchdog protection for system reliability
 *   - SD card data logging (JSON format)
 *   - ESP32-S3 communication for cloud upload (WiFi)
 *   - Air780E 4G backup communication (DISABLED: AT firmware lacks MQTT TLS support)
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-21   initial sensor integration
 * 2026-03-22   add OLED normal mode display and watchdog
 * 2026-03-23   add SD card data logging
 * 2026-03-24   add ESP32 UART communication
 * 2026-03-24   add Air780E 4G backup communication
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include "sensor_max30102.h"
#include "sensor_bme280.h"
#include "sensor_bh1750.h"
#include "sensor_gps.h"
#include "sensor_mpu6050.h"
#include "sensor_mq_gas.h"
#include "oled_ssd1306.h"
#include "display_status.h"
#include "data_logger.h"
#include "esp_comm.h"
/* #include "lte_driver.h" */  /* DISABLED: Air780E AT firmware lacks MQTT TLS support */
#include "ws2812b.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Configuration
 *---------------------------------------------------------------------------*/
/* Watchdog timeout in milliseconds (must feed within this time) */
#define WDT_TIMEOUT_MS          5000

/* Main loop interval (must be less than WDT_TIMEOUT_MS) */
#define MAIN_LOOP_INTERVAL_MS   2000

/* Enable/disable watchdog (set to 0 for debugging) */
#define ENABLE_WATCHDOG         1

/* Enable/disable data logging to SD card */
#define ENABLE_DATA_LOGGING     1

/* Enable/disable ESP32 communication */
#define ENABLE_ESP32_COMM       1

/* Force WiFi channel check (set to 0 if ESP32 is not physically connected) */
#define ESP32_HARDWARE_CONNECTED  1  /* ESP32 is connected on UART6 (PC6=TX, PC7=RX) */

/* DISABLED: 4G LTE backup communication - Air780E AT firmware lacks MQTT TLS support */
/* #define ENABLE_LTE_COMM         1 */
#define ENABLE_LTE_COMM         0

/* ESP32 send interval (in loops, 1 loop = 2s, so 5 = 10s) */
#define ESP32_SEND_INTERVAL     5

/* LTE connection check interval (in loops) */
/* #define LTE_CHECK_INTERVAL      30 */  /* DISABLED with LTE */

/* Communication channel state */
typedef enum {
    COMM_CHANNEL_NONE = 0,
    COMM_CHANNEL_WIFI,      /* Using ESP32 WiFi */
    /* COMM_CHANNEL_LTE, */  /* DISABLED: 4G LTE not available */
} comm_channel_t;

static comm_channel_t s_comm_channel = COMM_CHANNEL_NONE;
static rt_uint32_t s_esp32_fail_count = 0;
/* static rt_uint32_t s_lte_fail_count = 0; */    /* DISABLED with LTE */
/* static rt_bool_t s_lte_connecting = RT_FALSE; */ /* DISABLED with LTE */

#define MAX_FAIL_COUNT  3   /* Switch channel after 3 consecutive failures */

/*---------------------------------------------------------------------------
 * LTE Connection Thread - DISABLED
 * Reason: Air780E AT firmware does not support MQTT over TLS (no CMQTT command set)
 *         EMQX Cloud free tier only exposes port 8883 (TLS), so plain TCP MQTT
 *         commands (MCONFIG/MIPSTART/MCONNECT) are rejected with CME ERROR: 767.
 *---------------------------------------------------------------------------*/
#if 0  /* DISABLED: LTE not functional */
#if ENABLE_LTE_COMM
static void lte_connect_thread_entry(void *parameter)
{
    LOG_I("LTE connection thread started");

    if (lte_connect() == RT_EOK)
    {
        LOG_I("LTE backup connected!");
        s_comm_channel = COMM_CHANNEL_LTE;
    }
    else
    {
        LOG_E("LTE connection failed");
    }

    s_lte_connecting = RT_FALSE;  /* Clear connecting flag */
}
#endif
#endif  /* DISABLED */

/*---------------------------------------------------------------------------
 * Watchdog Functions
 *---------------------------------------------------------------------------*/
#if ENABLE_WATCHDOG
static rt_device_t s_wdt_dev = RT_NULL;
#endif

/**
 * @brief  Initialize and start the watchdog timer
 * @return RT_EOK on success, error code on failure
 */
static rt_err_t watchdog_init(void)
{
#if ENABLE_WATCHDOG
    rt_uint32_t timeout = WDT_TIMEOUT_MS;

    /* Find watchdog device */
    s_wdt_dev = rt_device_find("wdt");
    if (s_wdt_dev == RT_NULL)
    {
        LOG_W("Watchdog device not found, WDT disabled");
        return -RT_ENOSYS;
    }

    /* Open watchdog device */
    if (rt_device_open(s_wdt_dev, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        LOG_E("Failed to open watchdog device");
        s_wdt_dev = RT_NULL;
        return -RT_ERROR;
    }

    /* Set timeout (in milliseconds for some BSPs, seconds for others) */
    /* Note: STM32 HAL WDT driver typically uses seconds */
    rt_uint32_t timeout_sec = (timeout + 999) / 1000;  /* Round up to seconds */
    if (rt_device_control(s_wdt_dev, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout_sec) != RT_EOK)
    {
        LOG_E("Failed to set watchdog timeout");
        rt_device_close(s_wdt_dev);
        s_wdt_dev = RT_NULL;
        return -RT_ERROR;
    }

    /* Start watchdog */
    if (rt_device_control(s_wdt_dev, RT_DEVICE_CTRL_WDT_START, RT_NULL) != RT_EOK)
    {
        LOG_E("Failed to start watchdog");
        rt_device_close(s_wdt_dev);
        s_wdt_dev = RT_NULL;
        return -RT_ERROR;
    }

    LOG_I("Watchdog started, timeout = %u seconds", timeout_sec);
    return RT_EOK;
#else
    LOG_W("Watchdog disabled by configuration");
    return RT_EOK;
#endif
}

/**
 * @brief  Feed the watchdog (must be called periodically)
 *         If this function is not called within the timeout period,
 *         the system will reset automatically.
 */
static void watchdog_feed(void)
{
#if ENABLE_WATCHDOG
    if (s_wdt_dev != RT_NULL)
    {
        rt_device_control(s_wdt_dev, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    }
#endif
}

/*---------------------------------------------------------------------------
 * Main Application
 *---------------------------------------------------------------------------*/
int main(void)
{
    rt_bool_t oled_ok = RT_FALSE;

    /* --- WS2812B LED strip --- */
    if (ws2812b_init() != RT_EOK)
    {
        LOG_E("WS2812B init failed (PB1=TIM3_CH4)");
    }
    else
    {
        /* Run boot animation in a separate thread so main() continues initializing
         * sensors and OLED without waiting for the ~4s chase sequence to finish. */
        rt_thread_t chase_tid = rt_thread_create("led_chase",
                                                 (void (*)(void *))ws2812b_effect_chase_startup,
                                                 (void *)40,        /* speed_ms as parameter */
                                                 512,
                                                 25,                /* low priority: won't starve sensors */
                                                 5);
        if (chase_tid != RT_NULL)
            rt_thread_startup(chase_tid);
        else
            LOG_W("Failed to create led_chase thread, skipping boot animation");
    }

    /* --- OLED SSD1306 display --- */
    if (oled_init() == RT_EOK)
    {
        oled_ok = RT_TRUE;
        /* Show splash screen during initialization */
        display_splash_screen();
    }
    else
    {
        LOG_E("OLED init failed (PB8=SCL, PB9=SDA, addr=0x3C)");
    }

    /* --- MAX30102: heart rate & SpO2 --- */
    if (max30102_init() != RT_EOK)
        LOG_E("MAX30102 init failed (PB8=SCL, PB9=SDA, PB0=INT)");

    /* --- BME280: temperature, humidity, pressure --- */
    if (bme280_init() != RT_EOK)
        LOG_E("BME280 init failed (PB8=SCL, PB9=SDA, SDO=GND, CSB=VCC)");

    /* --- BH1750: ambient light --- */
    if (bh1750_init() != RT_EOK)
        LOG_E("BH1750 init failed (PB8=SCL, PB9=SDA, ADDR=GND)");

    /* --- MPU6050: accelerometer + gyroscope --- */
    if (mpu6050_init() != RT_EOK)
        LOG_E("MPU6050 init failed (PB8=SCL, PB9=SDA, AD0=GND)");
    /* Gyro calibration runs automatically in background thread "mpu_cal" (prio 24) */

    /* --- MQ-4 & MQ-7: gas sensors --- */
    if (mq_gas_init() != RT_EOK)
        LOG_E("MQ gas sensors init failed (MQ4=PA4, MQ7=PA5)");

    /* --- GPS ATGM336H --- */
    if (gps_init() != RT_EOK)
        LOG_E("GPS init failed (PB11=RX from GPS TXD)");

    /* --- Initialize and start watchdog --- */
    watchdog_init();

#if ENABLE_DATA_LOGGING
    /* --- Initialize data logger (after SD card is mounted) --- */
    if (logger_init() == RT_EOK)
    {
        LOG_I("Data logger ready: %s", logger_get_filename());
    }
    else
    {
        LOG_W("Data logger init failed - logging disabled");
    }
#endif

#if ENABLE_ESP32_COMM
    /* --- Initialize ESP32 communication --- */
    if (esp_comm_init() == RT_EOK)
    {
        LOG_I("ESP32 comm ready on UART6 (PC6=TX)");
    }
    else
    {
        LOG_W("ESP32 comm init failed - cloud upload disabled");
    }
#endif

    /* DISABLED: 4G LTE module init - Air780E AT firmware lacks MQTT TLS support
#if ENABLE_LTE_COMM
    if (lte_init() == RT_EOK)
    {
        LOG_I("LTE module initialized on UART4 (PA0=TX, PA1=RX)");
        LOG_I("  Use 'lte_test connect' to connect manually");
        LOG_I("  Or wait for auto-connect if ESP32 fails");
    }
    else
    {
        LOG_W("LTE module init failed - 4G backup disabled");
    }
#endif
    */

    LOG_I("=== Guardian System Initialized ===");

    /*=========================================================================
     * Normal Mode: All sensors + OLED status display + serial logging
     *=========================================================================*/
    display_data_t disp_data;
    rt_uint32_t loop_count = 0;
    static rt_uint8_t last_fall_state = FALL_STATE_NORMAL;
    static rt_bool_t last_gas_alarm = RT_FALSE;

    while (1)
    {
        /* ====== Feed watchdog at the start of each loop ====== */
        watchdog_feed();

        /* ====== Collect all sensor data ====== */
        max30102_get_result(&disp_data.hr);
        bme280_get_result(&disp_data.env);
        bh1750_get_result(&disp_data.light);
        gps_get_result(&disp_data.gps);
        mpu6050_get_result(&disp_data.imu);
        mq_gas_get_result(&disp_data.gas);

        /* ====== Update OLED display ====== */
        if (oled_ok)
        {
            /* Check for critical alerts */
            if (disp_data.imu.fall_state == FALL_STATE_FALLEN)
            {
                display_alert("FALL ALERT!", "Check on user!");
            }
            else if (mq_gas_is_alarm())
            {
                display_alert("GAS ALARM!", "Check for leaks!");
            }
            else
            {
                /* Build communication status for OLED */
                display_comm_status_t comm_st = {0};
                comm_st.channel  = (rt_uint8_t)s_comm_channel;
/* DISABLED LTE status query:
#if ENABLE_LTE_COMM
                {
                    const lte_status_t *ls = lte_get_status();
                    comm_st.lte_state = (rt_uint8_t)ls->state;
                    comm_st.lte_csq   = (rt_int8_t)ls->signal_quality;
                }
#endif
*/
                comm_st.sd_ok    = logger_is_ready();
#if ENABLE_ESP32_COMM
                comm_st.wifi_ok  = (s_comm_channel == COMM_CHANNEL_WIFI);
#endif
                display_status_update_full(&disp_data, &comm_st);
            }
        }

#if ENABLE_DATA_LOGGING
        /* ====== Log events (state changes) ====== */
        {
            rt_uint32_t ts = rt_tick_get() / RT_TICK_PER_SECOND;

            /* Log fall detection event */
            if (disp_data.imu.fall_state == FALL_STATE_FALLEN &&
                last_fall_state != FALL_STATE_FALLEN)
            {
                logger_log_event(LOG_EVENT_FALL_DETECTED, ts);
            }
            last_fall_state = disp_data.imu.fall_state;

            /* Log gas alarm event */
            rt_bool_t gas_alarm = mq_gas_is_alarm();
            if (gas_alarm && !last_gas_alarm)
            {
                logger_log_event(LOG_EVENT_GAS_ALARM, ts);
            }
            last_gas_alarm = gas_alarm;
        }

        /* ====== Data logging (every 5 loops = 10 seconds) ====== */
        if (loop_count % 5 == 0)
        {
            rt_uint32_t ts = rt_tick_get() / RT_TICK_PER_SECOND;
            logger_log_data(&disp_data, ts);
        }
#endif

        /* ====== Smart Communication Channel (WiFi only, LTE disabled) ====== */
#if ENABLE_ESP32_COMM
        if (loop_count % ESP32_SEND_INTERVAL == 0)
        {
            rt_bool_t sent = RT_FALSE;

            /* Try WiFi channel (only channel available) */
            if (s_comm_channel == COMM_CHANNEL_NONE || s_comm_channel == COMM_CHANNEL_WIFI)
            {
#if ESP32_HARDWARE_CONNECTED
                /* ESP32 is physically connected, try sending */
                if (esp_comm_is_ready())
                {
                    if (esp_comm_send_data(&disp_data) == RT_EOK)
                    {
                        sent = RT_TRUE;
                        s_esp32_fail_count = 0;
                        if (s_comm_channel != COMM_CHANNEL_WIFI) {
                            LOG_I("Switched to WiFi channel");
                            s_comm_channel = COMM_CHANNEL_WIFI;
                        }
                    }
                    else
                    {
                        s_esp32_fail_count++;
                        LOG_W("ESP32 send failed (%u/%u)", s_esp32_fail_count, MAX_FAIL_COUNT);
                    }
                }
                else
                {
                    s_esp32_fail_count++;
                    LOG_W("ESP32 not ready (%u/%u)", s_esp32_fail_count, MAX_FAIL_COUNT);
                }
#else
                /* ESP32 not connected yet */
                s_esp32_fail_count = MAX_FAIL_COUNT;
#endif
            }

            /* DISABLED: LTE fallback - Air780E AT firmware lacks MQTT TLS support */

            (void)sent;  /* suppress unused warning when ESP32 not connected */
        }
#endif

        /* Send alerts immediately when detected */
        {
            static rt_uint8_t last_fall_alert = FALL_STATE_NORMAL;
            static rt_bool_t last_gas_alert = RT_FALSE;

            /* Fall alert */
            if (disp_data.imu.fall_state == FALL_STATE_FALLEN &&
                last_fall_alert != FALL_STATE_FALLEN)
            {
#if ENABLE_ESP32_COMM
                if (s_comm_channel == COMM_CHANNEL_WIFI && esp_comm_is_ready()) {
                    esp_comm_send_alert("fall");
                }
#endif
                /* DISABLED: LTE fall alert - Air780E AT firmware lacks MQTT TLS support
#if ENABLE_LTE_COMM
                if (s_comm_channel == COMM_CHANNEL_LTE && lte_is_ready()) {
                    static char alert_buf[128];
                    snprintf(alert_buf, sizeof(alert_buf),
                        "{\"type\":\"fall\",\"confidence\":0.95,\"uptime_s\":%u}",
                        (unsigned)(rt_tick_get() / RT_TICK_PER_SECOND));
                    lte_mqtt_publish("alert/fall", alert_buf);
                }
#endif
                */
            }
            last_fall_alert = disp_data.imu.fall_state;

            /* Gas alert */
            rt_bool_t gas_now = mq_gas_is_alarm();
            if (gas_now && !last_gas_alert)
            {
#if ENABLE_ESP32_COMM
                if (s_comm_channel == COMM_CHANNEL_WIFI && esp_comm_is_ready()) {
                    esp_comm_send_alert("gas");
                }
#endif
                /* DISABLED: LTE gas alert - Air780E AT firmware lacks MQTT TLS support
#if ENABLE_LTE_COMM
                if (s_comm_channel == COMM_CHANNEL_LTE && lte_is_ready()) {
                    static char alert_buf[128];
                    snprintf(alert_buf, sizeof(alert_buf),
                        "{\"type\":\"gas\",\"uptime_s\":%u}",
                        (unsigned)(rt_tick_get() / RT_TICK_PER_SECOND));
                    lte_mqtt_publish("alert/gas", alert_buf);
                }
#endif
                */
            }
            last_gas_alert = gas_now;
        }
        /* (end of alert section) */

        /* ====== Serial logging (every 5 loops = 10 seconds) ====== */
        if (loop_count % 5 == 0)
        {
            LOG_I("--- Sensor Status ---");

            /* Heart Rate & SpO2 */
            if (disp_data.hr.hr_valid)
                LOG_I("HR=%d bpm, SpO2=%d%%",
                      disp_data.hr.heart_rate,
                      disp_data.hr.spo2_valid ? disp_data.hr.spo2 : 0);
            else
                LOG_I("HR=-- (no finger)");

            /* Environment */
            if (disp_data.env.valid)
                LOG_I("Temp=%d.%01dC Hum=%u%% Press=%uhPa",
                      disp_data.env.temperature / 10,
                      (disp_data.env.temperature < 0 ? -disp_data.env.temperature : disp_data.env.temperature) % 10,
                      disp_data.env.humidity >> 10,
                      (disp_data.env.pressure >> 8) / 100);

            /* Light */
            if (disp_data.light.valid)
                LOG_I("Light=%u lux", disp_data.light.lux);

            /* GPS */
            if (disp_data.gps.valid)
                LOG_I("GPS=%.5f,%.5f sats=%u",
                      disp_data.gps.latitude, disp_data.gps.longitude,
                      disp_data.gps.satellites);
            else
                LOG_I("GPS=-- (sats=%u)", disp_data.gps.satellites);

            /* IMU */
            if (disp_data.imu.valid)
            {
                int mag = (int)(disp_data.imu.accel_mag * 100);
                LOG_I("IMU Roll=%d Pitch=%d Mag=%d.%02dg Fall=%s",
                      (int)disp_data.imu.roll, (int)disp_data.imu.pitch,
                      mag / 100, mag % 100,
                      disp_data.imu.fall_state == 0 ? "OK" :
                      disp_data.imu.fall_state == 1 ? "FREEFALL" :
                      disp_data.imu.fall_state == 2 ? "IMPACT" : "FALLEN!");
            }

            /* Gas */
            if (disp_data.gas.valid)
                LOG_I("Gas CH4=%umV(%s) CO=%umV(%s)",
                      disp_data.gas.mq4_mv, mq_gas_level_str(disp_data.gas.mq4_level),
                      disp_data.gas.mq7_mv, mq_gas_level_str(disp_data.gas.mq7_level));

            /* Alerts */
            if (disp_data.imu.fall_state == FALL_STATE_FALLEN)
                LOG_W("!!! FALL DETECTED !!! Use mpu6050_reset to clear");
            if (mq_gas_is_alarm())
                LOG_W("!!! GAS ALARM !!!");
        }  /* end of serial logging */

        loop_count++;
        rt_thread_mdelay(MAIN_LOOP_INTERVAL_MS);
    }

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * MSH Commands for Communication Testing
 *---------------------------------------------------------------------------*/
#ifdef RT_USING_FINSH
#include <finsh.h>

/**
 * Show current communication channel status
 */
static void comm_status(void)
{
    const char *channel_names[] = {"NONE", "WiFi (ESP32)"};

    rt_kprintf("\n===== Communication Status =====\n");
    rt_kprintf("  Active Channel: %s\n", channel_names[s_comm_channel]);
    rt_kprintf("  ESP32 Fails:    %u/%u\n", s_esp32_fail_count, MAX_FAIL_COUNT);
    /* LTE status disabled: Air780E AT firmware lacks MQTT TLS support */
    rt_kprintf("  LTE:            DISABLED (firmware incompatible)\n");
    rt_kprintf("--------------------------------\n");
#if ENABLE_ESP32_COMM
    rt_kprintf("  ESP32 Ready:    %s\n", esp_comm_is_ready() ? "Yes" : "No");
    rt_uint32_t tx, ack, fail;
    esp_comm_get_stats(&tx, &ack, &fail);
    rt_kprintf("  ESP32 TX/ACK/Fail: %u/%u/%u\n", tx, ack, fail);
#endif
    rt_kprintf("================================\n\n");
}
MSH_CMD_EXPORT(comm_status, Show communication channel status);

/**
 * Force switch to LTE channel - DISABLED
 * LTE not available: Air780E AT firmware lacks MQTT TLS support
 */
static void comm_force_lte(void)
{
    rt_kprintf("LTE communication is DISABLED (Air780E AT firmware lacks MQTT TLS support).\n");
}
MSH_CMD_EXPORT(comm_force_lte, [DISABLED] LTE not available - firmware incompatible);

/**
 * Force switch back to WiFi channel
 */
static void comm_force_wifi(void)
{
#if ENABLE_ESP32_COMM
    rt_kprintf("Forcing switch to WiFi channel...\n");
    s_esp32_fail_count = 0;
    s_comm_channel = COMM_CHANNEL_WIFI;
    rt_kprintf("Channel set to WiFi.\n");
#else
    rt_kprintf("ESP32 communication is disabled in config.\n");
#endif
}
MSH_CMD_EXPORT(comm_force_wifi, Force switch to WiFi channel);

/**
 * Reset all communication counters
 */
static void comm_reset(void)
{
    s_esp32_fail_count = 0;
    s_comm_channel = COMM_CHANNEL_NONE;
    rt_kprintf("Communication counters reset.\n");
}
MSH_CMD_EXPORT(comm_reset, Reset communication counters);

#endif /* RT_USING_FINSH */
