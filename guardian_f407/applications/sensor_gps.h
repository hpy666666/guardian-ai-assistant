/*
 * GPS ATGM336H Driver — NMEA-0183 parser
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   GPS TXD -> PB11  (UART3 RX, GPS sends data to STM32)
 *   GPS RXD -> not connected (we only receive)
 *   GPS VCC -> 3.3V
 *   GPS GND -> GND
 *   GPS PPS -> not connected
 *
 * Parses:
 *   $GPRMC — recommended minimum (lat, lon, speed, date, valid flag)
 *   $GPGGA — fix data (lat, lon, altitude, satellite count, HDOP)
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-17   first version for Guardian project
 */

#ifndef __SENSOR_GPS_H__
#define __SENSOR_GPS_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware definitions
 *---------------------------------------------------------------------------*/
#define GPS_UART_NAME       "uart3"     /* UART3: PB10=TX, PB11=RX */
#define GPS_BAUD_RATE       9600        /* ATGM336H default baud rate */
#define GPS_RX_BUF_SIZE     256         /* ring buffer size */
#define GPS_LINE_BUF_SIZE   128         /* max NMEA sentence length */

/*---------------------------------------------------------------------------
 * Result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    /* Position */
    float       latitude;       /* degrees, positive = North */
    float       longitude;      /* degrees, positive = East  */
    float       altitude;       /* metres above sea level    */

    /* Motion */
    float       speed_knots;    /* speed over ground, knots  */

    /* Quality */
    rt_uint8_t  satellites;     /* number of satellites in use */
    float       hdop;           /* horizontal dilution of precision */

    /* Status */
    rt_uint8_t  valid;          /* 1 = position fix valid */
    rt_uint8_t  fix_type;       /* 0=no fix, 1=GPS, 2=DGPS */

    /* Time (UTC from GPS, not synced to RTC) */
    rt_uint8_t  hour;
    rt_uint8_t  minute;
    rt_uint8_t  second;
    rt_uint8_t  day;
    rt_uint8_t  month;
    rt_uint16_t year;

    rt_uint32_t timestamp;      /* rt_tick_get() when last updated */
} gps_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
/**
 * @brief  Initialise UART3 and start GPS receive/parse thread.
 * @return RT_EOK on success, negative on error.
 */
rt_err_t gps_init(void);

/**
 * @brief  Get the latest GPS fix. Thread-safe.
 * @param  result  Output buffer.
 * @return RT_EOK always; check result->valid.
 */
rt_err_t gps_get_result(gps_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_GPS_H__ */
