/*
 * MPU6050 6-Axis IMU Driver
 * Target: STM32F407 + RT-Thread standard edition
 */

#include "sensor_mpu6050.h"
#include <math.h>
#include <string.h>

#define DBG_TAG "mpu6050"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * MPU6050 Register Addresses
 *---------------------------------------------------------------------------*/
#define MPU6050_REG_SMPLRT_DIV      0x19    /* Sample rate divider */
#define MPU6050_REG_CONFIG          0x1A    /* Configuration (DLPF) */
#define MPU6050_REG_GYRO_CONFIG     0x1B    /* Gyroscope configuration */
#define MPU6050_REG_ACCEL_CONFIG    0x1C    /* Accelerometer configuration */
#define MPU6050_REG_FIFO_EN         0x23    /* FIFO enable */
#define MPU6050_REG_INT_PIN_CFG     0x37    /* INT pin configuration */
#define MPU6050_REG_INT_ENABLE      0x38    /* Interrupt enable */
#define MPU6050_REG_ACCEL_XOUT_H    0x3B    /* Accelerometer data (6 bytes) */
#define MPU6050_REG_TEMP_OUT_H      0x41    /* Temperature data (2 bytes) */
#define MPU6050_REG_GYRO_XOUT_H     0x43    /* Gyroscope data (6 bytes) */
#define MPU6050_REG_PWR_MGMT_1      0x6B    /* Power management 1 */
#define MPU6050_REG_PWR_MGMT_2      0x6C    /* Power management 2 */
#define MPU6050_REG_WHO_AM_I        0x75    /* Device ID (should be 0x68) */

/*---------------------------------------------------------------------------
 * Configuration values
 *---------------------------------------------------------------------------*/
/* WHO_AM_I values for different chip variants */
#define MPU6050_WHO_AM_I_VAL        0x68
#define MPU6500_WHO_AM_I_VAL        0x70    /* MPU6500/MPU9250 compatible */

/* Accelerometer full scale range */
#define ACCEL_FS_2G                 0x00    /* ±2g,  sensitivity 16384 LSB/g */
#define ACCEL_FS_4G                 0x08    /* ±4g,  sensitivity 8192 LSB/g */
#define ACCEL_FS_8G                 0x10    /* ±8g,  sensitivity 4096 LSB/g */
#define ACCEL_FS_16G                0x18    /* ±16g, sensitivity 2048 LSB/g */

/* Gyroscope full scale range */
#define GYRO_FS_250DPS              0x00    /* ±250°/s,  sensitivity 131 LSB/°/s */
#define GYRO_FS_500DPS              0x08    /* ±500°/s,  sensitivity 65.5 LSB/°/s */
#define GYRO_FS_1000DPS             0x10    /* ±1000°/s, sensitivity 32.8 LSB/°/s */
#define GYRO_FS_2000DPS             0x18    /* ±2000°/s, sensitivity 16.4 LSB/°/s */

/* Digital Low Pass Filter (DLPF) */
#define DLPF_BW_256HZ               0x00    /* Gyro: 256Hz, Accel: 260Hz */
#define DLPF_BW_188HZ               0x01    /* Gyro: 188Hz, Accel: 184Hz */
#define DLPF_BW_98HZ                0x02    /* Gyro: 98Hz,  Accel: 94Hz */
#define DLPF_BW_42HZ                0x03    /* Gyro: 42Hz,  Accel: 44Hz */
#define DLPF_BW_20HZ                0x04    /* Gyro: 20Hz,  Accel: 21Hz */
#define DLPF_BW_10HZ                0x05    /* Gyro: 10Hz,  Accel: 10Hz */
#define DLPF_BW_5HZ                 0x06    /* Gyro: 5Hz,   Accel: 5Hz */

/* Selected configuration */
#define SELECTED_ACCEL_FS           ACCEL_FS_2G
#define SELECTED_GYRO_FS            GYRO_FS_250DPS
#define SELECTED_DLPF               DLPF_BW_20HZ

/* Sensitivity based on selected range */
#define ACCEL_SENSITIVITY           16384.0f    /* LSB/g for ±2g */
#define GYRO_SENSITIVITY            131.0f      /* LSB/°/s for ±250°/s */

/*---------------------------------------------------------------------------
 * Fall detection parameters
 *---------------------------------------------------------------------------*/
#define FALL_FREEFALL_THRESHOLD     0.3f    /* g, below this = free fall */
#define FALL_IMPACT_THRESHOLD       2.5f    /* g, above this = impact */
#define FALL_ANGLE_THRESHOLD        45.0f   /* degrees, posture change */
#define FALL_STILL_THRESHOLD        0.2f    /* g deviation from 1g */
#define FALL_FREEFALL_DURATION_MS   80      /* Minimum free-fall time */
#define FALL_STILL_DURATION_MS      2000    /* Time to confirm lying still */

/*---------------------------------------------------------------------------
 * Complementary filter parameter
 *---------------------------------------------------------------------------*/
#define COMP_FILTER_ALPHA           0.98f   /* Gyro weight (0.98 typical) */

/*---------------------------------------------------------------------------
 * Private variables
 *---------------------------------------------------------------------------*/
static struct rt_i2c_bus_device *s_i2c_bus = RT_NULL;
static struct rt_mutex s_mutex;
static rt_bool_t s_initialized = RT_FALSE;

static mpu6050_result_t s_result;

/* Gyroscope calibration offsets */
static float s_gyro_offset_x = 0.0f;
static float s_gyro_offset_y = 0.0f;
static float s_gyro_offset_z = 0.0f;

/* Complementary filter state */
static float s_roll = 0.0f;
static float s_pitch = 0.0f;
static rt_tick_t s_last_tick = 0;

/* Fall detection state machine */
static rt_uint8_t s_fall_state = FALL_STATE_NORMAL;
static rt_tick_t s_freefall_start = 0;
static rt_tick_t s_impact_start = 0;    /* Time when IMPACT state started */
static rt_tick_t s_still_start = 0;
static float s_pre_fall_roll = 0.0f;
static float s_pre_fall_pitch = 0.0f;

/*---------------------------------------------------------------------------
 * I2C helper functions
 *---------------------------------------------------------------------------*/
static rt_err_t _write_reg(rt_uint8_t reg, rt_uint8_t value)
{
    rt_uint8_t buf[2] = {reg, value};
    struct rt_i2c_msg msg = {
        .addr  = MPU6050_I2C_ADDR,
        .flags = RT_I2C_WR,
        .buf   = buf,
        .len   = 2
    };

    if (rt_i2c_transfer(s_i2c_bus, &msg, 1) != 1)
        return -RT_ERROR;

    return RT_EOK;
}

static rt_err_t _read_reg(rt_uint8_t reg, rt_uint8_t *value)
{
    struct rt_i2c_msg msgs[2] = {
        { .addr = MPU6050_I2C_ADDR, .flags = RT_I2C_WR, .buf = &reg, .len = 1 },
        { .addr = MPU6050_I2C_ADDR, .flags = RT_I2C_RD, .buf = value, .len = 1 }
    };

    if (rt_i2c_transfer(s_i2c_bus, msgs, 2) != 2)
        return -RT_ERROR;

    return RT_EOK;
}

static rt_err_t _read_regs(rt_uint8_t reg, rt_uint8_t *buf, rt_uint8_t len)
{
    struct rt_i2c_msg msgs[2] = {
        { .addr = MPU6050_I2C_ADDR, .flags = RT_I2C_WR, .buf = &reg, .len = 1 },
        { .addr = MPU6050_I2C_ADDR, .flags = RT_I2C_RD, .buf = buf,  .len = len }
    };

    if (rt_i2c_transfer(s_i2c_bus, msgs, 2) != 2)
        return -RT_ERROR;

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Read raw sensor data
 *---------------------------------------------------------------------------*/
static rt_err_t _read_raw_data(rt_int16_t *accel, rt_int16_t *gyro)
{
    rt_uint8_t buf[14];

    /* Read 14 bytes starting from ACCEL_XOUT_H (accel + temp + gyro) */
    if (_read_regs(MPU6050_REG_ACCEL_XOUT_H, buf, 14) != RT_EOK)
        return -RT_ERROR;

    /* Accelerometer (big-endian) */
    accel[0] = (rt_int16_t)((buf[0] << 8) | buf[1]);   /* X */
    accel[1] = (rt_int16_t)((buf[2] << 8) | buf[3]);   /* Y */
    accel[2] = (rt_int16_t)((buf[4] << 8) | buf[5]);   /* Z */

    /* Skip temperature (buf[6], buf[7]) */

    /* Gyroscope (big-endian) */
    gyro[0] = (rt_int16_t)((buf[8]  << 8) | buf[9]);   /* X */
    gyro[1] = (rt_int16_t)((buf[10] << 8) | buf[11]);  /* Y */
    gyro[2] = (rt_int16_t)((buf[12] << 8) | buf[13]);  /* Z */

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Complementary filter for attitude estimation
 *---------------------------------------------------------------------------*/
static void _update_attitude(float ax, float ay, float az,
                             float gx, float gy, float gz,
                             float dt)
{
    /* Calculate angles from accelerometer (only valid when stationary) */
    float accel_roll  = atan2f(ay, az) * 180.0f / M_PI;
    float accel_pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / M_PI;

    /* Complementary filter:
     * - High-pass gyro (good for fast changes, drifts over time)
     * - Low-pass accelerometer (good for steady state, noisy during motion)
     */
    s_roll  = COMP_FILTER_ALPHA * (s_roll  + gx * dt) + (1.0f - COMP_FILTER_ALPHA) * accel_roll;
    s_pitch = COMP_FILTER_ALPHA * (s_pitch + gy * dt) + (1.0f - COMP_FILTER_ALPHA) * accel_pitch;
}

/*---------------------------------------------------------------------------
 * Fall detection state machine
 *---------------------------------------------------------------------------*/
static void _update_fall_detection(float accel_mag, float roll, float pitch)
{
    rt_tick_t now = rt_tick_get();

    switch (s_fall_state)
    {
    case FALL_STATE_NORMAL:
        /* Stage 1: Detect free-fall (weightlessness) */
        if (accel_mag < FALL_FREEFALL_THRESHOLD)
        {
            s_freefall_start = now;
            s_pre_fall_roll = roll;
            s_pre_fall_pitch = pitch;
            s_fall_state = FALL_STATE_FREEFALL;
            LOG_D("Fall: free-fall detected, mag=%d.%02d",
                  (int)accel_mag, (int)(accel_mag * 100) % 100);
        }
        break;

    case FALL_STATE_FREEFALL:
        /* Check if free-fall continues or impact occurs */
        if (accel_mag >= FALL_IMPACT_THRESHOLD)
        {
            /* Stage 2: Impact detected */
            rt_tick_t freefall_duration = now - s_freefall_start;
            if (freefall_duration >= rt_tick_from_millisecond(FALL_FREEFALL_DURATION_MS))
            {
                s_fall_state = FALL_STATE_IMPACT;
                s_impact_start = now;   /* Record when IMPACT state started */
                s_still_start = now;
                LOG_D("Fall: impact detected, mag=%d.%02d",
                      (int)accel_mag, (int)(accel_mag * 100) % 100);
            }
            else
            {
                /* Too short, probably just a bump */
                s_fall_state = FALL_STATE_NORMAL;
            }
        }
        else if (accel_mag > FALL_FREEFALL_THRESHOLD + 0.3f)
        {
            /* No longer in free-fall, no impact, reset */
            s_fall_state = FALL_STATE_NORMAL;
        }
        /* Stay in free-fall state if still weightless */
        break;

    case FALL_STATE_IMPACT:
        /* Stage 3: Check for posture change and stillness */
        {
            float roll_change = fabsf(roll - s_pre_fall_roll);
            float pitch_change = fabsf(pitch - s_pre_fall_pitch);
            float accel_deviation = fabsf(accel_mag - 1.0f);

            /* Check if lying still (acceleration close to 1g) */
            if (accel_deviation < FALL_STILL_THRESHOLD)
            {
                /* Check posture change */
                if (roll_change > FALL_ANGLE_THRESHOLD || pitch_change > FALL_ANGLE_THRESHOLD)
                {
                    rt_tick_t still_duration = now - s_still_start;
                    if (still_duration >= rt_tick_from_millisecond(FALL_STILL_DURATION_MS))
                    {
                        /* Confirmed fall! */
                        s_fall_state = FALL_STATE_FALLEN;
                        s_result.fall_timestamp = now;
                        LOG_W("Fall: CONFIRMED! roll_chg=%d pitch_chg=%d",
                              (int)roll_change, (int)pitch_change);
                    }
                }
                else
                {
                    /* Posture didn't change much, probably not a fall */
                    if ((now - s_still_start) > rt_tick_from_millisecond(3000))
                    {
                        s_fall_state = FALL_STATE_NORMAL;
                        LOG_D("Fall: posture unchanged, resetting");
                    }
                }
            }
            else
            {
                /* Still moving, reset stillness timer */
                s_still_start = now;
            }

            /* Timeout: if IMPACT state lasts more than 5 seconds without confirmation, reset */
            if ((now - s_impact_start) > rt_tick_from_millisecond(5000))
            {
                s_fall_state = FALL_STATE_NORMAL;
                LOG_D("Fall: IMPACT state timeout, resetting");
            }
        }
        break;

    case FALL_STATE_FALLEN:
        /* Stay in fallen state until manually reset */
        break;
    }
}

/*---------------------------------------------------------------------------
 * Sensor sampling thread
 *---------------------------------------------------------------------------*/
static void _mpu6050_thread(void *param)
{
    rt_int16_t accel_raw[3], gyro_raw[3];
    float ax, ay, az, gx, gy, gz;
    float dt;
    rt_tick_t now;

    (void)param;

    LOG_I("sampling thread started");

    while (1)
    {
        /* Read raw data */
        if (_read_raw_data(accel_raw, gyro_raw) != RT_EOK)
        {
            rt_thread_mdelay(100);
            continue;
        }

        /* Convert to physical units */
        ax = (float)accel_raw[0] / ACCEL_SENSITIVITY;
        ay = (float)accel_raw[1] / ACCEL_SENSITIVITY;
        az = (float)accel_raw[2] / ACCEL_SENSITIVITY;

        gx = (float)gyro_raw[0] / GYRO_SENSITIVITY - s_gyro_offset_x;
        gy = (float)gyro_raw[1] / GYRO_SENSITIVITY - s_gyro_offset_y;
        gz = (float)gyro_raw[2] / GYRO_SENSITIVITY - s_gyro_offset_z;

        /* Calculate time delta */
        now = rt_tick_get();
        if (s_last_tick == 0)
            dt = 0.01f;  /* First iteration, assume 10ms */
        else
            dt = (float)(now - s_last_tick) / RT_TICK_PER_SECOND;
        s_last_tick = now;

        /* Update attitude with complementary filter */
        _update_attitude(ax, ay, az, gx, gy, gz, dt);

        /* Calculate acceleration magnitude */
        float accel_mag = sqrtf(ax * ax + ay * ay + az * az);

        /* Update fall detection */
        _update_fall_detection(accel_mag, s_roll, s_pitch);

        /* Update result structure (thread-safe) */
        rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);

        s_result.accel_x = ax;
        s_result.accel_y = ay;
        s_result.accel_z = az;
        s_result.gyro_x = gx;
        s_result.gyro_y = gy;
        s_result.gyro_z = gz;
        s_result.roll = s_roll;
        s_result.pitch = s_pitch;
        s_result.accel_mag = accel_mag;
        s_result.fall_state = s_fall_state;
        s_result.valid = RT_TRUE;
        s_result.timestamp = now;

        rt_mutex_release(&s_mutex);

        /* Sample at ~100Hz */
        rt_thread_mdelay(10);
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t mpu6050_init(void)
{
    rt_uint8_t id;
    rt_err_t ret;
    rt_thread_t tid;

    if (s_initialized)
        return RT_EOK;

    /* Find I2C bus */
    s_i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(MPU6050_I2C_BUS_NAME);
    if (s_i2c_bus == RT_NULL)
    {
        LOG_E("I2C bus %s not found", MPU6050_I2C_BUS_NAME);
        return -RT_ERROR;
    }

    /* Check WHO_AM_I register */
    if (_read_reg(MPU6050_REG_WHO_AM_I, &id) != RT_EOK)
    {
        LOG_E("Failed to read WHO_AM_I register");
        return -RT_ERROR;
    }

    if (id != MPU6050_WHO_AM_I_VAL && id != MPU6500_WHO_AM_I_VAL)
    {
        LOG_E("WHO_AM_I mismatch: expected 0x68 or 0x70, got 0x%02X", id);
        return -RT_ERROR;
    }

    LOG_D("WHO_AM_I = 0x%02X (%s)", id,
          id == MPU6050_WHO_AM_I_VAL ? "MPU6050" : "MPU6500");

    /* Wake up the device (clear sleep bit) */
    ret = _write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to wake up device");
        return ret;
    }
    rt_thread_mdelay(100);  /* Wait for device to stabilize */

    /* Set clock source to PLL with X-axis gyro reference */
    _write_reg(MPU6050_REG_PWR_MGMT_1, 0x01);

    /* Configure sample rate divider (1kHz / (1 + divider))
     * For 100Hz: divider = 9 */
    _write_reg(MPU6050_REG_SMPLRT_DIV, 9);

    /* Configure DLPF (Digital Low Pass Filter) */
    _write_reg(MPU6050_REG_CONFIG, SELECTED_DLPF);

    /* Configure accelerometer (±2g) */
    _write_reg(MPU6050_REG_ACCEL_CONFIG, SELECTED_ACCEL_FS);

    /* Configure gyroscope (±250°/s) */
    _write_reg(MPU6050_REG_GYRO_CONFIG, SELECTED_GYRO_FS);

    /* Disable FIFO and interrupts (we'll use polling) */
    _write_reg(MPU6050_REG_FIFO_EN, 0x00);
    _write_reg(MPU6050_REG_INT_ENABLE, 0x00);

    /* Initialize mutex */
    ret = rt_mutex_init(&s_mutex, "mpu_mtx", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to create mutex");
        return ret;
    }

    /* Initialize result structure */
    rt_memset(&s_result, 0, sizeof(s_result));

    /* Create sampling thread
     * Stack size increased to 2048 for float-point math (sqrtf, atan2f, fabsf) */
    tid = rt_thread_create("mpu6050",
                           _mpu6050_thread,
                           RT_NULL,
                           2048,
                           RT_THREAD_PRIORITY_MAX / 2 - 1,  /* Higher priority for IMU */
                           20);
    if (tid == RT_NULL)
    {
        LOG_E("Failed to create thread");
        rt_mutex_detach(&s_mutex);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    s_initialized = RT_TRUE;
    LOG_I("init OK — I2C addr 0x%02X, accel ±2g, gyro ±250°/s", MPU6050_I2C_ADDR);

    return RT_EOK;
}

rt_err_t mpu6050_get_result(mpu6050_result_t *result)
{
    RT_ASSERT(result != RT_NULL);

    if (!s_initialized)
    {
        rt_memset(result, 0, sizeof(*result));
        return RT_EOK;
    }

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    *result = s_result;
    rt_mutex_release(&s_mutex);

    return RT_EOK;
}

void mpu6050_reset_fall_state(void)
{
    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    s_fall_state = FALL_STATE_NORMAL;
    s_result.fall_state = FALL_STATE_NORMAL;
    rt_mutex_release(&s_mutex);

    LOG_I("Fall state reset to normal");
}

rt_err_t mpu6050_calibrate_gyro(void)
{
    rt_int16_t accel_raw[3], gyro_raw[3];
    float sum_x = 0, sum_y = 0, sum_z = 0;
    int samples = 100;
    int i;

    LOG_I("Calibrating gyroscope... keep device still!");

    for (i = 0; i < samples; i++)
    {
        if (_read_raw_data(accel_raw, gyro_raw) != RT_EOK)
        {
            LOG_E("Calibration failed: read error");
            return -RT_ERROR;
        }

        sum_x += (float)gyro_raw[0] / GYRO_SENSITIVITY;
        sum_y += (float)gyro_raw[1] / GYRO_SENSITIVITY;
        sum_z += (float)gyro_raw[2] / GYRO_SENSITIVITY;

        rt_thread_mdelay(10);
    }

    s_gyro_offset_x = sum_x / samples;
    s_gyro_offset_y = sum_y / samples;
    s_gyro_offset_z = sum_z / samples;

    LOG_I("Gyro calibration done: offset X=%d.%02d Y=%d.%02d Z=%d.%02d",
          (int)s_gyro_offset_x, (int)(fabsf(s_gyro_offset_x) * 100) % 100,
          (int)s_gyro_offset_y, (int)(fabsf(s_gyro_offset_y) * 100) % 100,
          (int)s_gyro_offset_z, (int)(fabsf(s_gyro_offset_z) * 100) % 100);

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * MSH Commands
 *---------------------------------------------------------------------------*/
static int _cmd_mpu6050(int argc, char **argv)
{
    mpu6050_result_t r;
    (void)argc;
    (void)argv;

    if (!s_initialized)
    {
        rt_kprintf("MPU6050 not initialized\n");
        return -1;
    }

    mpu6050_get_result(&r);

    rt_kprintf("MPU6050 Data:\n");
    rt_kprintf("  Accel: X=%d.%02d  Y=%d.%02d  Z=%d.%02d g\n",
               (int)r.accel_x, (int)(fabsf(r.accel_x) * 100) % 100,
               (int)r.accel_y, (int)(fabsf(r.accel_y) * 100) % 100,
               (int)r.accel_z, (int)(fabsf(r.accel_z) * 100) % 100);
    rt_kprintf("  Gyro:  X=%d.%01d  Y=%d.%01d  Z=%d.%01d dps\n",
               (int)r.gyro_x, (int)(fabsf(r.gyro_x) * 10) % 10,
               (int)r.gyro_y, (int)(fabsf(r.gyro_y) * 10) % 10,
               (int)r.gyro_z, (int)(fabsf(r.gyro_z) * 10) % 10);
    rt_kprintf("  Attitude: Roll=%d.%01d  Pitch=%d.%01d deg\n",
               (int)r.roll, (int)(fabsf(r.roll) * 10) % 10,
               (int)r.pitch, (int)(fabsf(r.pitch) * 10) % 10);
    rt_kprintf("  Accel magnitude: %d.%02d g\n",
               (int)r.accel_mag, (int)(r.accel_mag * 100) % 100);
    rt_kprintf("  Fall state: %u (%s)\n", r.fall_state,
               r.fall_state == 0 ? "NORMAL" :
               r.fall_state == 1 ? "FREEFALL" :
               r.fall_state == 2 ? "IMPACT" : "FALLEN!");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_mpu6050, mpu6050, read MPU6050 IMU data);

static int _cmd_mpu6050_cal(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!s_initialized)
    {
        rt_kprintf("MPU6050 not initialized\n");
        return -1;
    }

    return mpu6050_calibrate_gyro() == RT_EOK ? 0 : -1;
}
MSH_CMD_EXPORT_ALIAS(_cmd_mpu6050_cal, mpu6050_cal, calibrate MPU6050 gyroscope);

static int _cmd_mpu6050_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    mpu6050_reset_fall_state();
    rt_kprintf("Fall state reset\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_mpu6050_reset, mpu6050_reset, reset fall detection state);
