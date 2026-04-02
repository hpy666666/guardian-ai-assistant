/*
 * BH1750 Ambient Light Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   BH1750 SCL  -> PB8  (soft I2C1, bus name "i2c1", shared with MAX30102/BME280)
 *   BH1750 SDA  -> PB9
 *   BH1750 ADDR -> GND  (I2C address = 0x23)
 *   BH1750 VCC  -> 3.3V
 *   BH1750 GND  -> GND
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-17   first version for Guardian project
 */

#ifndef __SENSOR_BH1750_H__
#define __SENSOR_BH1750_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware definitions
 *---------------------------------------------------------------------------*/
#define BH1750_I2C_BUS_NAME     "i2c1"      /* shared with MAX30102 / BME280 */
#define BH1750_I2C_ADDR         0x23        /* ADDR pin = GND; 0x5C if ADDR = VCC */

/*---------------------------------------------------------------------------
 * BH1750 command bytes (no register map — uses single-byte opcodes)
 *---------------------------------------------------------------------------*/
#define BH1750_CMD_POWER_DOWN   0x00    /* power off */
#define BH1750_CMD_POWER_ON     0x01    /* power on, wait for measurement cmd */
#define BH1750_CMD_RESET        0x07    /* reset data register (must be powered on) */

/* Continuous measurement modes */
#define BH1750_CMD_CONT_H_RES   0x10    /* 1 lx resolution, 120 ms typ */
#define BH1750_CMD_CONT_H_RES2  0x11    /* 0.5 lx resolution, 120 ms typ */
#define BH1750_CMD_CONT_L_RES   0x13    /* 4 lx resolution,  16 ms typ  */

/* One-time measurement modes (device powers down after each measurement) */
#define BH1750_CMD_ONE_H_RES    0x20
#define BH1750_CMD_ONE_H_RES2   0x21
#define BH1750_CMD_ONE_L_RES    0x23

/* Measurement time registers (MTreg, default 69) */
/* High bits: 0x40 | (MTreg >> 5)   Low bits: 0x60 | (MTreg & 0x1F) */
#define BH1750_MTREG_DEFAULT    69

/*---------------------------------------------------------------------------
 * Result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    rt_uint32_t lux;        /* illuminance in lux (integer, 0–65535) */
    rt_uint8_t  valid;      /* 1 = valid */
    rt_uint32_t timestamp;  /* rt_tick_get() */
} bh1750_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
/**
 * @brief  Initialise BH1750 and start background 1 Hz sampling thread.
 * @return RT_EOK on success, negative on error.
 */
rt_err_t bh1750_init(void);

/**
 * @brief  Get the latest illuminance reading. Thread-safe.
 * @param  result  Output buffer.
 * @return RT_EOK always; check result->valid.
 */
rt_err_t bh1750_get_result(bh1750_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_BH1750_H__ */
