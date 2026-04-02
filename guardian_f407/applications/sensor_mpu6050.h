/*
 * MPU6050 6-Axis IMU Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   MPU6050 VCC  -> 3.3V
 *   MPU6050 GND  -> GND
 *   MPU6050 SCL  -> PB8 (I2C1_SCL)
 *   MPU6050 SDA  -> PB9 (I2C1_SDA)
 *   MPU6050 AD0  -> GND (I2C address = 0x68)
 *   MPU6050 INT  -> Not connected (polling mode)
 *
 * Features:
 *   - 3-axis accelerometer (±2g default)
 *   - 3-axis gyroscope (±250°/s default)
 *   - Complementary filter for attitude (Roll/Pitch)
 *   - Fall detection algorithm (3-stage state machine)
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-21   first version for Guardian project
 */

#ifndef __SENSOR_MPU6050_H__
#define __SENSOR_MPU6050_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware definitions
 *---------------------------------------------------------------------------*/
#define MPU6050_I2C_BUS_NAME    "i2c1"
#define MPU6050_I2C_ADDR        0x68    /* AD0 = GND */

/*---------------------------------------------------------------------------
 * Fall detection states
 *---------------------------------------------------------------------------*/
#define FALL_STATE_NORMAL       0   /* Normal state */
#define FALL_STATE_FREEFALL     1   /* Free-fall detected (weightlessness) */
#define FALL_STATE_IMPACT       2   /* Impact detected */
#define FALL_STATE_FALLEN       3   /* Confirmed fall (lying still) */

/*---------------------------------------------------------------------------
 * Result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    /* Raw acceleration in g (1g = 9.8 m/s²) */
    float accel_x;
    float accel_y;
    float accel_z;

    /* Raw angular velocity in °/s */
    float gyro_x;
    float gyro_y;
    float gyro_z;

    /* Attitude angles from complementary filter (degrees) */
    float roll;         /* Rotation around X axis */
    float pitch;        /* Rotation around Y axis */

    /* Derived values */
    float accel_mag;    /* Acceleration vector magnitude: sqrt(x²+y²+z²) */

    /* Fall detection */
    rt_uint8_t fall_state;      /* FALL_STATE_xxx */
    rt_uint32_t fall_timestamp; /* Tick when fall was detected */

    /* Status */
    rt_bool_t valid;
    rt_uint32_t timestamp;
} mpu6050_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize MPU6050 sensor
 * @return RT_EOK on success, negative error code on failure
 */
rt_err_t mpu6050_init(void);

/**
 * @brief  Get latest sensor data (thread-safe)
 * @param  result  Output buffer
 * @return RT_EOK on success
 */
rt_err_t mpu6050_get_result(mpu6050_result_t *result);

/**
 * @brief  Reset fall detection state to normal
 *         Call this after fall alert has been acknowledged
 */
void mpu6050_reset_fall_state(void);

/**
 * @brief  Calibrate gyroscope zero offset
 *         Device must be stationary during calibration
 * @return RT_EOK on success
 */
rt_err_t mpu6050_calibrate_gyro(void);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_MPU6050_H__ */
