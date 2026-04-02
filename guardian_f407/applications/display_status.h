/*
 * Status Display Module for OLED (Normal Mode)
 * Shows sensor status summary on OLED display
 *
 * Display Layout (128x64):
 * ┌────────────────────────────┐
 * │ HR:XXX SpO2:XX%  [FALL!]   │  Line 0: Heart rate + SpO2 + Fall alert
 * ├────────────────────────────┤
 * │ T:XX.XC  H:XX%  P:XXXXhPa  │  Line 1: Temperature, Humidity, Pressure
 * │ Light: XXXXX lux           │  Line 2: Ambient light
 * │ Gas: CH4:OK CO:OK          │  Line 3: Gas sensor status
 * │ GPS: XX.XXX,XXX.XXX SatXX  │  Line 4: GPS coordinates
 * │ IMU: R:XX P:XX Mag:X.XXg   │  Line 5: IMU attitude
 * │ WiFi:-- 4G:CONN CSQ:18 SD  │  Line 6: Comm status bar
 * └────────────────────────────┘
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-22   first version for Guardian project
 */

#ifndef __DISPLAY_STATUS_H__
#define __DISPLAY_STATUS_H__

#include <rtthread.h>
#include "sensor_max30102.h"
#include "sensor_bme280.h"
#include "sensor_bh1750.h"
#include "sensor_gps.h"
#include "sensor_mpu6050.h"
#include "sensor_mq_gas.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Display data structure (aggregates all sensor data)
 *---------------------------------------------------------------------------*/
typedef struct
{
    max30102_result_t hr;
    bme280_result_t   env;
    bh1750_result_t   light;
    gps_result_t      gps;
    mpu6050_result_t  imu;
    mq_gas_result_t   gas;
} display_data_t;

/* Communication status for OLED status bar */
typedef struct
{
    rt_uint8_t channel;         /* 0=NONE, 1=WiFi, 2=LTE */
    rt_uint8_t lte_state;       /* lte_state_t enum value */
    rt_int8_t  lte_csq;         /* Signal quality 0-31, -1=unknown */
    rt_bool_t  sd_ok;           /* SD card logging active */
    rt_bool_t  wifi_ok;         /* ESP32 WiFi connected */
} display_comm_status_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Update OLED display with all sensor data (Normal Mode)
 *         Call this periodically from main loop
 * @param  data  Pointer to aggregated sensor data
 * @param  comm  Pointer to comm status (NULL to use default status bar)
 */
void display_status_update(const display_data_t *data);
void display_status_update_full(const display_data_t *data, const display_comm_status_t *comm);

/**
 * @brief  Display a simple startup splash screen
 */
void display_splash_screen(void);

/**
 * @brief  Display an alert message (e.g., fall detected, gas alarm)
 * @param  title    Alert title (e.g., "FALL ALERT!")
 * @param  message  Alert details
 */
void display_alert(const char *title, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* __DISPLAY_STATUS_H__ */
