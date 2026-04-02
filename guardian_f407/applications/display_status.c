/*
 * Status Display Module for OLED (Normal Mode)
 * Shows sensor status summary on OLED display
 */

#include "display_status.h"
#include "oled_ssd1306.h"
#include "lte_driver.h"
#include <stdio.h>
#include <string.h>

/*---------------------------------------------------------------------------
 * Public API Implementation
 *---------------------------------------------------------------------------*/

void display_status_update(const display_data_t *data)
{
    char buf[24];  /* Buffer for formatting strings */

    oled_clear();

    /* === Line 0: Heart Rate + SpO2 + Fall Alert (y=0) === */
    {
        char hr_str[8] = "--";
        char spo2_str[8] = "--";

        if (data->hr.hr_valid)
            snprintf(hr_str, sizeof(hr_str), "%d", data->hr.heart_rate);
        if (data->hr.spo2_valid)
            snprintf(spo2_str, sizeof(spo2_str), "%d", data->hr.spo2);

        if (data->imu.fall_state == FALL_STATE_FALLEN)
            snprintf(buf, sizeof(buf), "HR:%s O2:%s%% FALL!", hr_str, spo2_str);
        else
            snprintf(buf, sizeof(buf), "HR:%s bpm O2:%s%%", hr_str, spo2_str);
    }
    oled_draw_string(0, 0, buf, 8);

    /* Separator line */
    oled_draw_hline(0, 9, 128);

    /* === Line 1: Temperature, Humidity, Pressure (y=11) === */
    if (data->env.valid)
    {
        int temp_int = data->env.temperature / 100;
        int temp_frac = (data->env.temperature < 0 ? -data->env.temperature : data->env.temperature) % 100;
        int hum_int = data->env.humidity >> 10;
        int press_int = (data->env.pressure >> 8) / 100;

        snprintf(buf, sizeof(buf), "T:%d.%dC H:%d%% %dhPa",
                 temp_int, temp_frac / 10, hum_int, press_int);
    }
    else
    {
        snprintf(buf, sizeof(buf), "T:-- H:-- P:--");
    }
    oled_draw_string(0, 11, buf, 8);

    /* === Line 2: Light (y=20) === */
    if (data->light.valid)
        snprintf(buf, sizeof(buf), "Light: %u lux", data->light.lux);
    else
        snprintf(buf, sizeof(buf), "Light: --");
    oled_draw_string(0, 20, buf, 8);

    /* === Line 3: Gas Sensors (y=29) === */
    if (data->gas.valid)
    {
        const char *ch4 = mq_gas_level_str(data->gas.mq4_level);
        const char *co  = mq_gas_level_str(data->gas.mq7_level);
        snprintf(buf, sizeof(buf), "Gas CH4:%s CO:%s", ch4, co);
    }
    else
    {
        snprintf(buf, sizeof(buf), "Gas: --");
    }
    oled_draw_string(0, 29, buf, 8);

    /* === Line 4: GPS (y=38) === */
    if (data->gps.valid)
    {
        /* Show abbreviated coordinates to fit on screen */
        int lat_int = (int)data->gps.latitude;
        int lat_frac = (int)((data->gps.latitude - lat_int) * 1000);
        if (lat_frac < 0) lat_frac = -lat_frac;
        int lon_int = (int)data->gps.longitude;
        int lon_frac = (int)((data->gps.longitude - lon_int) * 1000);
        if (lon_frac < 0) lon_frac = -lon_frac;

        snprintf(buf, sizeof(buf), "GPS:%d.%03d,%d.%03d S%u",
                 lat_int, lat_frac, lon_int, lon_frac, data->gps.satellites);
    }
    else
    {
        snprintf(buf, sizeof(buf), "GPS:-- Sat:%u", data->gps.satellites);
    }
    oled_draw_string(0, 38, buf, 8);

    /* === Line 5: IMU (y=47) === */
    if (data->imu.valid)
    {
        int mag_int = (int)(data->imu.accel_mag * 100);
        snprintf(buf, sizeof(buf), "IMU R:%d P:%d %d.%02dg",
                 (int)data->imu.roll, (int)data->imu.pitch,
                 mag_int / 100, mag_int % 100);
    }
    else
    {
        snprintf(buf, sizeof(buf), "IMU: --");
    }
    oled_draw_string(0, 47, buf, 8);

    /* === Line 6: Status bar (y=56) === */
    {
        rt_tick_t uptime_sec = rt_tick_get() / RT_TICK_PER_SECOND;
        int min = uptime_sec / 60;
        int sec = uptime_sec % 60;
        snprintf(buf, sizeof(buf), "Up:%02d:%02d Guardian", min, sec);
    }
    oled_draw_string(0, 56, buf, 8);

    oled_refresh();
}

void display_status_update_full(const display_data_t *data, const display_comm_status_t *comm)
{
    char buf[24];

    oled_clear();

    /* === Line 0: Heart Rate + SpO2 + Fall Alert (y=0) === */
    {
        char hr_str[8] = "--";
        char spo2_str[8] = "--";

        if (data->hr.hr_valid)
            snprintf(hr_str, sizeof(hr_str), "%d", data->hr.heart_rate);
        if (data->hr.spo2_valid)
            snprintf(spo2_str, sizeof(spo2_str), "%d", data->hr.spo2);

        if (data->imu.fall_state == FALL_STATE_FALLEN)
            snprintf(buf, sizeof(buf), "HR:%s O2:%s%% FALL!", hr_str, spo2_str);
        else
            snprintf(buf, sizeof(buf), "HR:%s bpm O2:%s%%", hr_str, spo2_str);
    }
    oled_draw_string(0, 0, buf, 8);

    /* Separator line */
    oled_draw_hline(0, 9, 128);

    /* === Line 1: Temperature, Humidity, Pressure (y=11) === */
    if (data->env.valid)
    {
        int temp_int = data->env.temperature / 100;
        int temp_frac = (data->env.temperature < 0 ? -data->env.temperature : data->env.temperature) % 100;
        int hum_int = data->env.humidity >> 10;
        int press_int = (data->env.pressure >> 8) / 100;

        snprintf(buf, sizeof(buf), "T:%d.%dC H:%d%% %dhPa",
                 temp_int, temp_frac / 10, hum_int, press_int);
    }
    else
    {
        snprintf(buf, sizeof(buf), "T:-- H:-- P:--");
    }
    oled_draw_string(0, 11, buf, 8);

    /* === Line 2: Light (y=20) === */
    if (data->light.valid)
        snprintf(buf, sizeof(buf), "Light: %u lux", data->light.lux);
    else
        snprintf(buf, sizeof(buf), "Light: --");
    oled_draw_string(0, 20, buf, 8);

    /* === Line 3: Gas Sensors (y=29) === */
    if (data->gas.valid)
    {
        const char *ch4 = mq_gas_level_str(data->gas.mq4_level);
        const char *co  = mq_gas_level_str(data->gas.mq7_level);
        snprintf(buf, sizeof(buf), "Gas CH4:%s CO:%s", ch4, co);
    }
    else
    {
        snprintf(buf, sizeof(buf), "Gas: --");
    }
    oled_draw_string(0, 29, buf, 8);

    /* === Line 4: GPS (y=38) === */
    if (data->gps.valid)
    {
        int lat_int = (int)data->gps.latitude;
        int lat_frac = (int)((data->gps.latitude - lat_int) * 1000);
        if (lat_frac < 0) lat_frac = -lat_frac;
        int lon_int = (int)data->gps.longitude;
        int lon_frac = (int)((data->gps.longitude - lon_int) * 1000);
        if (lon_frac < 0) lon_frac = -lon_frac;

        snprintf(buf, sizeof(buf), "GPS:%d.%03d,%d.%03d S%u",
                 lat_int, lat_frac, lon_int, lon_frac, data->gps.satellites);
    }
    else
    {
        snprintf(buf, sizeof(buf), "GPS:-- Sat:%u", data->gps.satellites);
    }
    oled_draw_string(0, 38, buf, 8);

    /* === Line 5: IMU (y=47) === */
    if (data->imu.valid)
    {
        int mag_int = (int)(data->imu.accel_mag * 100);
        snprintf(buf, sizeof(buf), "IMU R:%d P:%d %d.%02dg",
                 (int)data->imu.roll, (int)data->imu.pitch,
                 mag_int / 100, mag_int % 100);
    }
    else
    {
        snprintf(buf, sizeof(buf), "IMU: --");
    }
    oled_draw_string(0, 47, buf, 8);

    /* === Line 6: Communication status bar (y=56) === */
    if (comm)
    {
        /* LTE state short names for display */
        static const char * const lte_short[] = {
            "--",       /* IDLE */
            "INIT",     /* INIT */
            "SIM",      /* SIM_READY */
            "NET",      /* NETWORK_REG */
            "GPRS",     /* GPRS_ATTACHED */
            "MQTT..",   /* MQTT_CONNECTING */
            "ON",       /* MQTT_CONNECTED */
            "ERR",      /* ERROR */
        };

        const char *lte_str = lte_short[comm->lte_state < 8 ? comm->lte_state : 7];

        if (comm->channel == 2 && comm->lte_state == 6) {
            /* LTE connected and active - show signal strength */
            snprintf(buf, sizeof(buf), "4G:%s CSQ:%d %s",
                     lte_str,
                     comm->lte_csq >= 0 ? comm->lte_csq : 0,
                     comm->sd_ok ? "SD" : "");
        } else if (comm->channel == 1) {
            /* WiFi active */
            snprintf(buf, sizeof(buf), "WiFi:ON 4G:%s %s",
                     lte_str,
                     comm->sd_ok ? "SD" : "");
        } else {
            /* LTE connecting or no connection */
            snprintf(buf, sizeof(buf), "4G:%s CSQ:%d %s",
                     lte_str,
                     comm->lte_csq >= 0 ? comm->lte_csq : 0,
                     comm->sd_ok ? "SD" : "");
        }
    }
    else
    {
        rt_tick_t uptime_sec = rt_tick_get() / RT_TICK_PER_SECOND;
        int min = uptime_sec / 60;
        int sec = uptime_sec % 60;
        snprintf(buf, sizeof(buf), "Up:%02d:%02d Guardian", min, sec);
    }
    oled_draw_string(0, 56, buf, 8);

    oled_refresh();
}

void display_splash_screen(void)
{
    oled_clear();

    oled_draw_string(20, 8, "Guardian", 16);
    oled_draw_string(4, 28, "Elderly Safety System", 8);
    oled_draw_hline(0, 40, 128);
    oled_draw_string(16, 48, "Initializing...", 8);

    oled_refresh();
}

void display_alert(const char *title, const char *message)
{
    oled_clear();

    /* Draw alert box */
    oled_draw_rect(2, 2, 125, 61);
    oled_draw_rect(4, 4, 123, 59);

    /* Title centered (approximately) */
    oled_draw_string(10, 12, title, 16);

    /* Separator */
    oled_draw_hline(10, 32, 108);

    /* Message */
    oled_draw_string(10, 40, message, 8);

    oled_refresh();
}
