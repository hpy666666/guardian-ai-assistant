/*
 * BME280 Temperature / Humidity / Pressure Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   BME280 SCL -> PB8  (soft I2C1, bus name "i2c1", shared with MAX30102)
 *   BME280 SDA -> PB9
 *   BME280 SDO -> GND  (I2C address = 0x76)
 *   BME280 CSB -> VCC  (force I2C mode)
 *   BME280 VCC -> 3.3V
 *   BME280 GND -> GND
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-17   first version for Guardian project
 */

#ifndef __SENSOR_BME280_H__
#define __SENSOR_BME280_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware definitions
 *---------------------------------------------------------------------------*/
#define BME280_I2C_BUS_NAME     "i2c1"      /* shared with MAX30102 */
#define BME280_I2C_ADDR         0x76        /* SDO = GND */

/*---------------------------------------------------------------------------
 * Register map (Bosch BME280 datasheet, section 5.4)
 *---------------------------------------------------------------------------*/
/* Calibration data block 1: 0x88 – 0xA1 */
#define BME280_REG_CALIB00      0x88
#define BME280_REG_CALIB25      0x9F        /* end of first calib block */
#define BME280_REG_CALIB26_H1   0xA1        /* dig_H1 is a single byte here */

/* Calibration data block 2: 0xE1 – 0xF0 */
#define BME280_REG_CALIB26      0xE1

/* Data registers */
#define BME280_REG_CHIP_ID      0xD0        /* should read 0x60 */
#define BME280_REG_RESET        0xE0        /* write 0xB6 to soft-reset */
#define BME280_REG_CTRL_HUM     0xF2        /* must be set BEFORE ctrl_meas */
#define BME280_REG_STATUS       0xF3
#define BME280_REG_CTRL_MEAS    0xF4
#define BME280_REG_CONFIG       0xF5
#define BME280_REG_PRESS_MSB    0xF7
#define BME280_REG_PRESS_LSB    0xF8
#define BME280_REG_PRESS_XLSB   0xF9
#define BME280_REG_TEMP_MSB     0xFA
#define BME280_REG_TEMP_LSB     0xFB
#define BME280_REG_TEMP_XLSB    0xFC
#define BME280_REG_HUM_MSB      0xFD
#define BME280_REG_HUM_LSB      0xFE

/* ctrl_meas bit fields */
#define BME280_OSRS_T_X2        (0x02 << 5)   /* temp oversampling x2 */
#define BME280_OSRS_P_X16       (0x05 << 2)   /* pressure oversampling x16 */
#define BME280_MODE_NORMAL      0x03          /* Normal mode */
#define BME280_MODE_FORCED      0x01          /* one-shot */
#define BME280_MODE_SLEEP       0x00

/* ctrl_hum */
#define BME280_OSRS_H_X1        0x01          /* humidity oversampling x1 */

/* config register */
/* t_sb=1000ms (0x05), filter=4 (0x02), spi3w_en=0 */
#define BME280_CONFIG_VAL       ((0x05 << 5) | (0x02 << 2) | 0x00)

/*---------------------------------------------------------------------------
 * Calibration coefficient storage
 *---------------------------------------------------------------------------*/
typedef struct
{
    /* Temperature */
    rt_uint16_t dig_T1;
    rt_int16_t  dig_T2;
    rt_int16_t  dig_T3;

    /* Pressure */
    rt_uint16_t dig_P1;
    rt_int16_t  dig_P2;
    rt_int16_t  dig_P3;
    rt_int16_t  dig_P4;
    rt_int16_t  dig_P5;
    rt_int16_t  dig_P6;
    rt_int16_t  dig_P7;
    rt_int16_t  dig_P8;
    rt_int16_t  dig_P9;

    /* Humidity */
    rt_uint8_t  dig_H1;
    rt_int16_t  dig_H2;
    rt_uint8_t  dig_H3;
    rt_int16_t  dig_H4;
    rt_int16_t  dig_H5;
    rt_int8_t   dig_H6;
} bme280_calib_t;

/*---------------------------------------------------------------------------
 * Public result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    rt_int32_t  temperature;    /* 0.01 °C units, e.g. 2512 = 25.12 °C */
    rt_uint32_t pressure;       /* Pa in Q8.8 fixed point / 256 = Pa     */
                                /* divide by 100 for hPa                  */
    rt_uint32_t humidity;       /* 0.001 %RH units / 1024 = %RH           */
                                /* divide by 1024 for %RH                 */
    rt_uint8_t  valid;          /* 1 = valid data */
    rt_uint32_t timestamp;      /* rt_tick_get() */
} bme280_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
/**
 * @brief  Initialise BME280 and start background 1 Hz sampling thread.
 *         Call once from main() or INIT_APP_EXPORT.
 * @return RT_EOK on success, negative on error.
 */
rt_err_t bme280_init(void);

/**
 * @brief  Get the latest BME280 measurement. Thread-safe.
 * @param  result  Output buffer.
 * @return RT_EOK always; check result->valid.
 */
rt_err_t bme280_get_result(bme280_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_BME280_H__ */
