/*
 * Data Logger Module Implementation
 * Logs sensor data to SD card in JSON Lines format
 */

#include "data_logger.h"
#include <rtthread.h>
#include <dfs_posix.h>
#include <stdio.h>
#include <string.h>

#define DBG_TAG "logger"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Private variables
 *---------------------------------------------------------------------------*/
static int s_log_fd = -1;                       /* Log file descriptor */
static char s_log_filename[32];                 /* Current log filename */
static rt_uint32_t s_bytes_written = 0;         /* Total bytes written */
static rt_bool_t s_initialized = RT_FALSE;

/* RTC date (updated by ESP32 NTP sync via logger_update_time) */
static int s_year  = 2026;
static int s_month = 3;
static int s_day   = 25;   /* Default: boot date; updated when NTP arrives */
static rt_bool_t s_date_from_ntp = RT_FALSE;

/*---------------------------------------------------------------------------
 * Private functions
 *---------------------------------------------------------------------------*/

/**
 * @brief  Generate log filename using current RTC date
 */
static void generate_filename(char *buf, rt_size_t size)
{
    snprintf(buf, size, "%slog_%04d%02d%02d.json",
             LOG_DIR, s_year, s_month, s_day);
}

/**
 * @brief  Open or create log file for current day
 */
static rt_err_t open_log_file(void)
{
    /* Close existing file if open */
    if (s_log_fd >= 0)
    {
        close(s_log_fd);
        s_log_fd = -1;
    }

    /* Generate filename */
    generate_filename(s_log_filename, sizeof(s_log_filename));

    /* Open file in append mode */
    s_log_fd = open(s_log_filename, O_WRONLY | O_CREAT | O_APPEND);
    if (s_log_fd < 0)
    {
        LOG_E("Failed to open log file: %s (err=%d)", s_log_filename, s_log_fd);
        return -RT_ERROR;
    }

    s_date_from_ntp = RT_FALSE;  /* track whether date was set by NTP */
    LOG_I("Log file opened: %s", s_log_filename);

    return RT_EOK;
}

/**
 * @brief  Check if we need to switch to a new day's file
 *         Only meaningful once NTP date is available
 */
static void check_day_rollover(void)
{
    /* Without NTP, s_day stays at default - no rollover check needed */
    (void)0;
}

/**
 * @brief  Write a line to log file
 */
static rt_err_t write_log_line(const char *line, rt_size_t len)
{
    if (s_log_fd < 0)
        return -RT_ERROR;

    int written = write(s_log_fd, line, len);
    if (written != (int)len)
    {
        LOG_E("Write failed: %d/%d bytes", written, len);
        return -RT_ERROR;
    }

    s_bytes_written += written;
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Public API Implementation
 *---------------------------------------------------------------------------*/

rt_err_t logger_init(void)
{
    rt_err_t ret;

    if (s_initialized)
        return RT_EOK;

    /* Open log file */
    ret = open_log_file();
    if (ret != RT_EOK)
        return ret;

    s_initialized = RT_TRUE;

    /* Log system start event */
    logger_log_event(LOG_EVENT_SYSTEM_START, rt_tick_get() / RT_TICK_PER_SECOND);

    LOG_I("Data logger initialized");
    return RT_EOK;
}

rt_err_t logger_log_data(const display_data_t *data, rt_uint32_t timestamp)
{
    char buf[LOG_BUFFER_SIZE];
    int len;

    if (!s_initialized || s_log_fd < 0)
        return -RT_ERROR;

    /* Check for day rollover */
    check_day_rollover();

    /* Build JSON line
     * Note: Using integer values where possible to save space
     * temp is in 0.01C units, hum in %, press in hPa
     */
    len = snprintf(buf, sizeof(buf),
        "{\"ts\":%u"
        ",\"hr\":%d,\"hr_v\":%d"
        ",\"spo2\":%d,\"spo2_v\":%d"
        ",\"temp\":%d,\"hum\":%u,\"press\":%u,\"env_v\":%d"
        /* temp unit: 0.1C (same as esp_comm: temperature/10) */
        ",\"lux\":%u,\"lux_v\":%d"
        ",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"sat\":%u,\"gps_v\":%d"
        ",\"roll\":%.1f,\"pitch\":%.1f,\"mag\":%.2f,\"fall\":%d,\"imu_v\":%d"
        ",\"ch4\":%u,\"ch4_lv\":%d,\"co\":%u,\"co_lv\":%d,\"gas_v\":%d"
        "}\n",
        timestamp,
        /* Heart rate */
        data->hr.hr_valid ? data->hr.heart_rate : 0,
        data->hr.hr_valid ? 1 : 0,
        /* SpO2 */
        data->hr.spo2_valid ? data->hr.spo2 : 0,
        data->hr.spo2_valid ? 1 : 0,
        /* Environment */
        data->env.temperature / 10,  /* 0.01C -> 0.1C, consistent with esp_comm */
        data->env.humidity >> 10,
        (data->env.pressure >> 8) / 100,
        data->env.valid ? 1 : 0,
        /* Light */
        data->light.lux,
        data->light.valid ? 1 : 0,
        /* GPS */
        data->gps.latitude,
        data->gps.longitude,
        data->gps.altitude,
        data->gps.satellites,
        data->gps.valid ? 1 : 0,
        /* IMU */
        data->imu.roll,
        data->imu.pitch,
        data->imu.accel_mag,
        data->imu.fall_state,
        data->imu.valid ? 1 : 0,
        /* Gas */
        data->gas.mq4_mv,
        data->gas.mq4_level,
        data->gas.mq7_mv,
        data->gas.mq7_level,
        data->gas.valid ? 1 : 0
    );

    if (len >= (int)sizeof(buf))
    {
        LOG_W("JSON line truncated");
        len = sizeof(buf) - 1;
    }

    return write_log_line(buf, len);
}

rt_err_t logger_log_event(log_event_t event, rt_uint32_t timestamp)
{
    char buf[128];
    int len;
    const char *event_name;

    if (!s_initialized || s_log_fd < 0)
        return -RT_ERROR;

    switch (event)
    {
    case LOG_EVENT_FALL_DETECTED:
        event_name = "FALL_DETECTED";
        break;
    case LOG_EVENT_GAS_ALARM:
        event_name = "GAS_ALARM";
        break;
    case LOG_EVENT_SYSTEM_START:
        event_name = "SYSTEM_START";
        break;
    case LOG_EVENT_SYSTEM_STOP:
        event_name = "SYSTEM_STOP";
        break;
    default:
        return RT_EOK;
    }

    len = snprintf(buf, sizeof(buf),
        "{\"ts\":%u,\"event\":\"%s\"}\n",
        timestamp, event_name);

    return write_log_line(buf, len);
}

void logger_close(void)
{
    if (s_log_fd >= 0)
    {
        /* Log system stop event */
        logger_log_event(LOG_EVENT_SYSTEM_STOP, rt_tick_get() / RT_TICK_PER_SECOND);

        close(s_log_fd);
        s_log_fd = -1;
        LOG_I("Log file closed, total bytes: %u", s_bytes_written);
    }
    s_initialized = RT_FALSE;
}

const char* logger_get_filename(void)
{
    return s_log_filename;
}

rt_uint32_t logger_get_bytes_written(void)
{
    return s_bytes_written;
}

rt_bool_t logger_is_ready(void)
{
    return s_initialized && (s_log_fd >= 0);
}

void logger_update_time(int year, int month, int day)
{
    /* Check if date actually changed (avoid unnecessary file reopen) */
    if (s_year == year && s_month == month && s_day == day && s_date_from_ntp) {
        return;
    }

    s_year  = year;
    s_month = month;
    s_day   = day;
    s_date_from_ntp = RT_TRUE;

    LOG_I("Date updated from NTP: %04d-%02d-%02d, reopening log file", year, month, day);

    /* Re-open log file with correct date in filename */
    if (s_initialized) {
        open_log_file();
    }
}

/*---------------------------------------------------------------------------
 * MSH Commands for testing
 *---------------------------------------------------------------------------*/

/**
 * @brief  Show logger status
 */
static int cmd_log_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("\n===== Logger Status =====\n");
    rt_kprintf("Initialized: %s\n", s_initialized ? "YES" : "NO");
    rt_kprintf("File: %s\n", s_log_filename[0] ? s_log_filename : "(none)");
    rt_kprintf("File open: %s\n", s_log_fd >= 0 ? "YES" : "NO");
    rt_kprintf("Bytes written: %u\n", s_bytes_written);
    rt_kprintf("=========================\n\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log_status, log_status, show data logger status);

/**
 * @brief  Manually start logger
 */
static int cmd_log_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_initialized)
    {
        rt_kprintf("Logger already running\n");
        return 0;
    }

    if (logger_init() == RT_EOK)
        rt_kprintf("Logger started: %s\n", s_log_filename);
    else
        rt_kprintf("Failed to start logger\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log_start, log_start, start data logger);

/**
 * @brief  Manually stop logger
 */
static int cmd_log_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!s_initialized)
    {
        rt_kprintf("Logger not running\n");
        return 0;
    }

    logger_close();
    rt_kprintf("Logger stopped\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log_stop, log_stop, stop data logger);

/**
 * @brief  Test logging with dummy data
 */
static int cmd_log_test(int argc, char **argv)
{
    display_data_t test_data;
    rt_uint32_t ts;

    (void)argc;
    (void)argv;

    /* Initialize logger if needed */
    if (!s_initialized)
    {
        if (logger_init() != RT_EOK)
        {
            rt_kprintf("Failed to init logger\n");
            return -1;
        }
    }

    /* Create test data */
    memset(&test_data, 0, sizeof(test_data));

    test_data.hr.hr_valid = 1;
    test_data.hr.heart_rate = 75;
    test_data.hr.spo2_valid = 1;
    test_data.hr.spo2 = 98;

    test_data.env.valid = 1;
    test_data.env.temperature = 2350;  /* 23.50 C */
    test_data.env.humidity = 65 << 10; /* 65% */
    test_data.env.pressure = 101325 << 8; /* 1013.25 hPa */

    test_data.light.valid = 1;
    test_data.light.lux = 500;

    test_data.gps.valid = 0;
    test_data.gps.satellites = 3;

    test_data.imu.valid = 1;
    test_data.imu.roll = 5.0f;
    test_data.imu.pitch = -3.0f;
    test_data.imu.accel_mag = 1.02f;
    test_data.imu.fall_state = 0;

    test_data.gas.valid = 1;
    test_data.gas.mq4_mv = 800;
    test_data.gas.mq4_level = 0;
    test_data.gas.mq7_mv = 600;
    test_data.gas.mq7_level = 0;

    ts = rt_tick_get() / RT_TICK_PER_SECOND;

    rt_kprintf("Writing test log entry...\n");
    if (logger_log_data(&test_data, ts) == RT_EOK)
    {
        rt_kprintf("OK! Bytes written: %u\n", s_bytes_written);
        rt_kprintf("File: %s\n", s_log_filename);
        rt_kprintf("\nUse 'cat %s' to view log content\n", s_log_filename);
    }
    else
    {
        rt_kprintf("FAILED to write log\n");
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log_test, log_test, test logger with dummy data);
