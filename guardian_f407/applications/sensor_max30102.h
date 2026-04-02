/*
 * MAX30102 Heart Rate & SpO2 Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   MAX30102 SCL -> PB8  (soft I2C1, bus name "i2c1")
 *   MAX30102 SDA -> PB9
 *   MAX30102 INT -> PB0  (active low, triggers on A_FULL)
 *   MAX30102 VCC -> 3.3V
 *   MAX30102 GND -> GND
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-17   first version for Guardian project
 */

#ifndef __SENSOR_MAX30102_H__
#define __SENSOR_MAX30102_H__

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>      /* GET_PIN() macro and BSP pin definitions */

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware pin definitions
 *---------------------------------------------------------------------------*/
#define MAX30102_I2C_BUS_NAME   "i2c1"      /* soft I2C bus configured in board.h */
#define MAX30102_I2C_ADDR       0x57        /* 7-bit address (write=0xAE>>1) */
#define MAX30102_INT_PIN        GET_PIN(B, 0)   /* active-low interrupt output */

/*---------------------------------------------------------------------------
 * Register map (from MAX30102 datasheet Table 3)
 *---------------------------------------------------------------------------*/
#define MAX30102_REG_INT_STATUS1    0x00
#define MAX30102_REG_INT_STATUS2    0x01
#define MAX30102_REG_INT_ENABLE1    0x02
#define MAX30102_REG_INT_ENABLE2    0x03
#define MAX30102_REG_FIFO_WR_PTR    0x04
#define MAX30102_REG_OVF_COUNTER    0x05
#define MAX30102_REG_FIFO_RD_PTR    0x06
#define MAX30102_REG_FIFO_DATA      0x07
#define MAX30102_REG_FIFO_CONFIG    0x08
#define MAX30102_REG_MODE_CONFIG    0x09
#define MAX30102_REG_SPO2_CONFIG    0x0A
#define MAX30102_REG_LED1_PA        0x0C    /* Red LED */
#define MAX30102_REG_LED2_PA        0x0D    /* IR  LED */
#define MAX30102_REG_PILOT_PA       0x10
#define MAX30102_REG_MULTI_LED1     0x11
#define MAX30102_REG_MULTI_LED2     0x12
#define MAX30102_REG_TEMP_INT       0x1F
#define MAX30102_REG_TEMP_FRAC      0x20
#define MAX30102_REG_TEMP_CONFIG    0x21
#define MAX30102_REG_REV_ID         0xFE
#define MAX30102_REG_PART_ID        0xFF

/* REG_INT_ENABLE1 bits */
#define MAX30102_INT_A_FULL_EN      (1 << 7)
#define MAX30102_INT_PPG_RDY_EN     (1 << 6)
#define MAX30102_INT_ALC_OVF_EN     (1 << 5)

/* REG_MODE_CONFIG values */
#define MAX30102_MODE_HR_ONLY       0x02
#define MAX30102_MODE_SPO2          0x03
#define MAX30102_MODE_MULTI_LED     0x07
#define MAX30102_MODE_RESET         0x40
#define MAX30102_MODE_SHDN          0x80

/* REG_SPO2_CONFIG bit fields */
/* ADC range: bits [6:5] */
#define MAX30102_ADC_RGE_2048       (0x00 << 5)
#define MAX30102_ADC_RGE_4096       (0x01 << 5)   /* recommended */
#define MAX30102_ADC_RGE_8192       (0x02 << 5)
#define MAX30102_ADC_RGE_16384      (0x03 << 5)
/* Sample rate: bits [4:2] */
#define MAX30102_SR_50              (0x00 << 2)
#define MAX30102_SR_100             (0x01 << 2)   /* 100 sps, recommended */
#define MAX30102_SR_200             (0x02 << 2)
#define MAX30102_SR_400             (0x03 << 2)
/* LED pulse width (also sets ADC resolution): bits [1:0] */
#define MAX30102_PW_69              0x00  /* 15-bit */
#define MAX30102_PW_118             0x01  /* 16-bit */
#define MAX30102_PW_215             0x02  /* 17-bit */
#define MAX30102_PW_411             0x03  /* 18-bit, recommended */

/*---------------------------------------------------------------------------
 * Sampling / algorithm constants
 *---------------------------------------------------------------------------*/
#define MAX30102_SAMPLE_RATE        100     /* sps, must match SPO2_SR setting */
#define MAX30102_BUFFER_SIZE        500     /* 5 seconds of data at 100 sps */
#define MAX30102_FIFO_ALMOST_FULL   17      /* samples remaining when INT fires */

/* 18-bit raw data mask (LED_PW = 0x03 → 18-bit resolution) */
#define MAX30102_DATA_MASK          0x0003FFFFUL

/*---------------------------------------------------------------------------
 * Public result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    rt_int32_t  heart_rate;         /* beats per minute, -999 = invalid */
    rt_int32_t  spo2;               /* %, -999 = invalid */
    rt_uint8_t  hr_valid;           /* 1 = valid */
    rt_uint8_t  spo2_valid;         /* 1 = valid */
    rt_uint32_t timestamp;          /* rt_tick_get() at measurement */
} max30102_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
/**
 * @brief  Initialise the MAX30102 and start the background measurement thread.
 *         Call once from main() or an INIT_APP_EXPORT entry.
 * @return RT_EOK on success, negative on error.
 */
rt_err_t max30102_init(void);

/**
 * @brief  Read the latest valid heart rate and SpO2 result.
 *         Thread-safe (uses a mutex).
 * @param  result  Output buffer.
 * @return RT_EOK always; check result->hr_valid / spo2_valid.
 */
rt_err_t max30102_get_result(max30102_result_t *result);

/**
 * @brief  Write a single MAX30102 register via I2C.
 *         Exposed for optional MSH debug commands.
 */
rt_err_t max30102_write_reg(rt_uint8_t reg, rt_uint8_t data);

/**
 * @brief  Read a single MAX30102 register via I2C.
 */
rt_err_t max30102_read_reg(rt_uint8_t reg, rt_uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_MAX30102_H__ */
