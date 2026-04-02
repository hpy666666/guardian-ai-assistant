/*
 * Data Logger Module
 * Logs sensor data to SD card in JSON format
 *
 * Features:
 *   - JSON format for easy cloud upload and analysis
 *   - Auto file split by date (log_YYYYMMDD.json)
 *   - Configurable logging interval
 *   - Event logging for alerts (fall, gas alarm)
 *
 * File format: JSON Lines (one JSON object per line)
 *   {"ts":1234567890,"hr":75,"spo2":98,"temp":2350,"hum":65,...}
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-23   first version
 */

#ifndef __DATA_LOGGER_H__
#define __DATA_LOGGER_H__

#include <rtthread.h>
#include "display_status.h"  /* For display_data_t */

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Configuration
 *---------------------------------------------------------------------------*/
#define LOG_DIR             "/"                 /* Log directory */
#define LOG_INTERVAL_SEC    10                  /* Log interval in seconds */
#define LOG_BUFFER_SIZE     512                 /* JSON line buffer size */

/*---------------------------------------------------------------------------
 * Event types for alert logging
 *---------------------------------------------------------------------------*/
typedef enum {
    LOG_EVENT_NONE = 0,
    LOG_EVENT_FALL_DETECTED,
    LOG_EVENT_GAS_ALARM,
    LOG_EVENT_SYSTEM_START,
    LOG_EVENT_SYSTEM_STOP
} log_event_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize data logger
 *         Creates log directory if needed, opens today's log file
 * @return RT_EOK on success, error code on failure
 */
rt_err_t logger_init(void);

/**
 * @brief  Log sensor data to file
 *         Call this periodically from main loop
 * @param  data      Pointer to aggregated sensor data
 * @param  timestamp System uptime in seconds (or RTC time if available)
 * @return RT_EOK on success, error code on failure
 */
rt_err_t logger_log_data(const display_data_t *data, rt_uint32_t timestamp);

/**
 * @brief  Log an event (alert, system event)
 * @param  event     Event type
 * @param  timestamp System uptime in seconds
 * @return RT_EOK on success
 */
rt_err_t logger_log_event(log_event_t event, rt_uint32_t timestamp);

/**
 * @brief  Flush and close log file
 *         Call before system shutdown
 */
void logger_close(void);

/**
 * @brief  Get current log file name
 * @return Pointer to current log file path string
 */
const char* logger_get_filename(void);

/**
 * @brief  Get total bytes written to current log file
 * @return Bytes written
 */
rt_uint32_t logger_get_bytes_written(void);

/**
 * @brief  Check if logger is ready (SD card mounted and file open)
 * @return RT_TRUE if ready, RT_FALSE otherwise
 */
rt_bool_t logger_is_ready(void);

/**
 * @brief  Update RTC date from ESP32 NTP sync
 *         Re-opens log file with correct date-based filename.
 *         Called by esp_comm when a time packet is received.
 * @param  year   Full year (e.g. 2026)
 * @param  month  Month 1-12
 * @param  day    Day 1-31
 */
void logger_update_time(int year, int month, int day);

#ifdef __cplusplus
}
#endif

#endif /* __DATA_LOGGER_H__ */
