/*
 * BME280 Temperature / Humidity / Pressure Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Notes:
 *  - Shares soft I2C bus "i2c1" (PB8/PB9) with MAX30102.
 *    RT-Thread I2C bus driver is thread-safe; no extra locking needed.
 *  - Compensation formulas are the Bosch-provided 32-bit integer version
 *    from BME280 datasheet Rev 1.6, Appendix (section 4.2.3).
 *  - t_fine is the shared temperature fine value required by the pressure
 *    and humidity compensation formulas.
 *  - Output units:
 *      temperature : 0.01 °C   (divide by 100 for °C)
 *      pressure    : Pa * 256  (divide by 256 for Pa, then /100 for hPa)
 *      humidity    : %RH * 1024 (divide by 1024 for %RH)
 */

#define LOG_TAG     "bme280"
#define LOG_LVL     DBG_INFO
#include <rtdbg.h>

#include "sensor_bme280.h"
#include <board.h>      /* GET_PIN() */
#include <string.h>

/* SCL/SDA pin numbers — must match board.h BSP_I2C1 config */
#define BME280_SCL_PIN      GET_PIN(B, 8)
#define BME280_SDA_PIN      GET_PIN(B, 9)

/*---------------------------------------------------------------------------
 * Internal state
 *---------------------------------------------------------------------------*/
static struct rt_i2c_bus_device *s_i2c_bus = RT_NULL;
static bme280_calib_t            s_calib;
static bme280_result_t           s_result;
static struct rt_mutex           s_mutex;
static rt_bool_t                 s_initialized = RT_FALSE;

/*---------------------------------------------------------------------------
 * Low-level I2C helpers
 *---------------------------------------------------------------------------*/

/**
 * Write one byte to a BME280 register.
 */
static rt_err_t _write_reg(rt_uint8_t reg, rt_uint8_t val)
{
    rt_uint8_t buf[2] = { reg, val };
    struct rt_i2c_msg msg;

    msg.addr  = BME280_I2C_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = buf;
    msg.len   = 2;

    if (rt_i2c_transfer(s_i2c_bus, &msg, 1) != 1)
    {
        LOG_E("write reg 0x%02X failed", reg);
        return -RT_EIO;
    }
    return RT_EOK;
}

/**
 * Read `len` bytes starting at register `reg` into `buf`.
 * Uses a combined write-then-read transfer.
 */
static rt_err_t _read_regs(rt_uint8_t reg, rt_uint8_t *buf, rt_uint16_t len)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = BME280_I2C_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    msgs[1].addr  = BME280_I2C_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    if (rt_i2c_transfer(s_i2c_bus, msgs, 2) != 2)
    {
        LOG_E("read regs from 0x%02X len=%d failed", reg, len);
        return -RT_EIO;
    }
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Calibration coefficient loading
 *---------------------------------------------------------------------------*/

static rt_err_t _load_calibration(void)
{
    rt_uint8_t raw[26];
    rt_err_t   ret;

    /* --- Block 1: 0x88 – 0x9F (24 bytes) and 0xA1 (1 byte) --- */
    ret = _read_regs(BME280_REG_CALIB00, raw, 24);
    if (ret != RT_EOK) return ret;

    s_calib.dig_T1 = (rt_uint16_t)(raw[1]  << 8 | raw[0]);
    s_calib.dig_T2 = (rt_int16_t) (raw[3]  << 8 | raw[2]);
    s_calib.dig_T3 = (rt_int16_t) (raw[5]  << 8 | raw[4]);

    s_calib.dig_P1 = (rt_uint16_t)(raw[7]  << 8 | raw[6]);
    s_calib.dig_P2 = (rt_int16_t) (raw[9]  << 8 | raw[8]);
    s_calib.dig_P3 = (rt_int16_t) (raw[11] << 8 | raw[10]);
    s_calib.dig_P4 = (rt_int16_t) (raw[13] << 8 | raw[12]);
    s_calib.dig_P5 = (rt_int16_t) (raw[15] << 8 | raw[14]);
    s_calib.dig_P6 = (rt_int16_t) (raw[17] << 8 | raw[16]);
    s_calib.dig_P7 = (rt_int16_t) (raw[19] << 8 | raw[18]);
    s_calib.dig_P8 = (rt_int16_t) (raw[21] << 8 | raw[20]);
    s_calib.dig_P9 = (rt_int16_t) (raw[23] << 8 | raw[22]);

    /* dig_H1 lives at 0xA1 */
    ret = _read_regs(BME280_REG_CALIB26_H1, raw, 1);
    if (ret != RT_EOK) return ret;
    s_calib.dig_H1 = raw[0];

    /* --- Block 2: 0xE1 – 0xE7 (7 bytes) --- */
    ret = _read_regs(BME280_REG_CALIB26, raw, 7);
    if (ret != RT_EOK) return ret;

    s_calib.dig_H2 = (rt_int16_t)(raw[1] << 8 | raw[0]);
    s_calib.dig_H3 = raw[2];
    /* H4: 0xE4[7:4] << 4 | 0xE5[3:0] */
    s_calib.dig_H4 = (rt_int16_t)((rt_int16_t)(rt_int8_t)raw[3] << 4 | (raw[4] & 0x0F));
    /* H5: 0xE5[7:4] >> 4 | 0xE6 << 4 */
    s_calib.dig_H5 = (rt_int16_t)((rt_int16_t)(rt_int8_t)raw[5] << 4 | (raw[4] >> 4));
    s_calib.dig_H6 = (rt_int8_t)raw[6];

    LOG_D("calib T1=%u T2=%d T3=%d", s_calib.dig_T1, s_calib.dig_T2, s_calib.dig_T3);
    LOG_D("calib H1=%u H2=%d H3=%u H4=%d H5=%d H6=%d",
          s_calib.dig_H1, s_calib.dig_H2, s_calib.dig_H3,
          s_calib.dig_H4, s_calib.dig_H5, s_calib.dig_H6);

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Bosch 32-bit integer compensation formulas (datasheet section 4.2.3)
 *---------------------------------------------------------------------------*/

/**
 * Compensate raw temperature.
 * @param  adc_T   20-bit raw temperature ADC value
 * @param  t_fine  Output: fine temperature used by P and H formulas
 * @return Temperature in units of 0.01 °C
 */
static rt_int32_t _compensate_T(rt_int32_t adc_T, rt_int32_t *t_fine)
{
    rt_int32_t var1, var2, T;

    var1 = ((((adc_T >> 3) - ((rt_int32_t)s_calib.dig_T1 << 1)))
            * ((rt_int32_t)s_calib.dig_T2)) >> 11;

    var2 = (((((adc_T >> 4) - ((rt_int32_t)s_calib.dig_T1))
              * ((adc_T >> 4) - ((rt_int32_t)s_calib.dig_T1))) >> 12)
             * ((rt_int32_t)s_calib.dig_T3)) >> 14;

    *t_fine = var1 + var2;
    T = (*t_fine * 5 + 128) >> 8;   /* units: 0.01 °C */
    return T;
}

/**
 * Compensate raw pressure.
 * Requires t_fine to be set by _compensate_T() first.
 * @return Pressure in Pa * 256 (Q24.8 fixed point)
 */
static rt_uint32_t _compensate_P(rt_int32_t adc_P, rt_int32_t t_fine)
{
    rt_int64_t var1, var2, p;

    var1 = ((rt_int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (rt_int64_t)s_calib.dig_P6;
    var2 = var2 + ((var1 * (rt_int64_t)s_calib.dig_P5) << 17);
    var2 = var2 + (((rt_int64_t)s_calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (rt_int64_t)s_calib.dig_P3) >> 8)
           + ((var1 * (rt_int64_t)s_calib.dig_P2) << 12);
    var1 = (((((rt_int64_t)1) << 47) + var1)) * ((rt_int64_t)s_calib.dig_P1) >> 33;

    if (var1 == 0)
        return 0;   /* avoid divide-by-zero */

    p    = 1048576 - adc_P;
    p    = (((p << 31) - var2) * 3125) / var1;
    var1 = (((rt_int64_t)s_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((rt_int64_t)s_calib.dig_P8) * p) >> 19;
    p    = ((p + var1 + var2) >> 8) + (((rt_int64_t)s_calib.dig_P7) << 4);

    return (rt_uint32_t)p;   /* Pa * 256 */
}

/**
 * Compensate raw humidity.
 * Requires t_fine to be set by _compensate_T() first.
 * @return Humidity in %RH * 1024 (Q22.10 fixed point)
 */
static rt_uint32_t _compensate_H(rt_int32_t adc_H, rt_int32_t t_fine)
{
    rt_int32_t v_x1_u32r;

    v_x1_u32r = (t_fine - ((rt_int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((rt_int32_t)s_calib.dig_H4) << 20)
                    - (((rt_int32_t)s_calib.dig_H5) * v_x1_u32r))
                   + ((rt_int32_t)16384)) >> 15)
                 * (((((((v_x1_u32r * ((rt_int32_t)s_calib.dig_H6)) >> 10)
                         * (((v_x1_u32r * ((rt_int32_t)s_calib.dig_H3)) >> 11)
                            + ((rt_int32_t)32768))) >> 10)
                       + ((rt_int32_t)2097152))
                      * ((rt_int32_t)s_calib.dig_H2) + 8192) >> 14));

    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7)
                                * ((rt_int32_t)s_calib.dig_H1)) >> 4));

    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);

    return (rt_uint32_t)(v_x1_u32r >> 12);   /* %RH * 1024 */
}

/*---------------------------------------------------------------------------
 * Sensor configuration
 *---------------------------------------------------------------------------*/
static rt_err_t _configure(void)
{
    rt_uint8_t chip_id = 0;
    rt_err_t   ret;

    /*
     * Do NOT send soft-reset before verifying the chip exists —
     * if the device is absent or still powering up, the write NACK
     * returns -RT_EIO and aborts init needlessly.
     * Instead just verify chip ID first; if OK, then reset cleanly.
     */

    /* Verify chip ID first */
    ret = _read_regs(BME280_REG_CHIP_ID, &chip_id, 1);
    if (ret != RT_EOK) return ret;
    if (chip_id != 0x60)
    {
        LOG_E("unexpected chip ID 0x%02X (expected 0x60)", chip_id);
        LOG_E("check: SDO=GND -> addr 0x76, SDO=VCC -> addr 0x77");
        return -RT_ERROR;
    }
    LOG_I("chip ID OK (0x60)");

    /* Now safe to soft-reset — device is confirmed present */
    ret = _write_reg(BME280_REG_RESET, 0xB6);
    if (ret != RT_EOK) return ret;
    rt_thread_mdelay(10);   /* datasheet: startup time ~2ms after reset */

    /* Load factory calibration */
    ret = _load_calibration();
    if (ret != RT_EOK) return ret;

    /*
     * Register write order matters:
     *   1. ctrl_hum  (takes effect only after ctrl_meas is written)
     *   2. config    (t_standby, IIR filter)
     *   3. ctrl_meas (osrs_t, osrs_p, mode) — last, activates everything
     */
    ret = _write_reg(BME280_REG_CTRL_HUM,  BME280_OSRS_H_X1);
    if (ret != RT_EOK) return ret;

    ret = _write_reg(BME280_REG_CONFIG,    BME280_CONFIG_VAL);
    if (ret != RT_EOK) return ret;

    /* osrs_t=x2, osrs_p=x16, mode=Normal */
    ret = _write_reg(BME280_REG_CTRL_MEAS,
                     BME280_OSRS_T_X2 | BME280_OSRS_P_X16 | BME280_MODE_NORMAL);
    if (ret != RT_EOK) return ret;

    LOG_I("configured: Normal mode, t_sb=1000ms, filter=4");
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Read and process one sample
 *---------------------------------------------------------------------------*/
static rt_err_t _read_sample(void)
{
    rt_uint8_t  raw[8];
    rt_int32_t  adc_T, adc_P, adc_H;
    rt_int32_t  t_fine;
    rt_int32_t  temp;
    rt_uint32_t press, hum;
    rt_err_t    ret;

    /* Read all 8 data registers in one burst: 0xF7–0xFE */
    ret = _read_regs(BME280_REG_PRESS_MSB, raw, 8);
    if (ret != RT_EOK) return ret;

    /* 20-bit ADC values (bits [19:4] in MSB/LSB, bits [3:0] in XLSB[7:4]) */
    adc_P = ((rt_int32_t)raw[0] << 12) | ((rt_int32_t)raw[1] << 4) | (raw[2] >> 4);
    adc_T = ((rt_int32_t)raw[3] << 12) | ((rt_int32_t)raw[4] << 4) | (raw[5] >> 4);
    /* 16-bit humidity ADC */
    adc_H = ((rt_int32_t)raw[6] << 8)  |  raw[7];

    /* Bosch compensation */
    temp  = _compensate_T(adc_T, &t_fine);
    press = _compensate_P(adc_P, t_fine);
    hum   = _compensate_H(adc_H, t_fine);

    LOG_D("adc T=%d P=%d H=%d", adc_T, adc_P, adc_H);
    LOG_D("comp T=%d (0.01°C) P=%u (/256 Pa) H=%u (/1024 %%)",
          temp, press, hum);

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    s_result.temperature = temp;
    s_result.pressure    = press;
    s_result.humidity    = hum;
    s_result.valid       = 1;
    s_result.timestamp   = rt_tick_get();
    rt_mutex_release(&s_mutex);

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Background sampling thread (1 Hz)
 *---------------------------------------------------------------------------*/
static void _bme280_thread(void *param)
{
    (void)param;

    LOG_I("sampling thread started");

    while (1)
    {
        if (_read_sample() != RT_EOK)
        {
            LOG_W("sample read failed, retry in 2s");
            rt_thread_mdelay(2000);
        }
        else
        {
            /* Normal mode standby is 1000ms; add a small margin */
            rt_thread_mdelay(1100);
        }
    }
}

/*---------------------------------------------------------------------------
 * MSH command: bme280
 *---------------------------------------------------------------------------*/
static int _cmd_bme280(int argc, char **argv)
{
    bme280_result_t r;
    (void)argc;
    (void)argv;

    bme280_get_result(&r);

    if (!r.valid)
    {
        rt_kprintf("BME280: no valid data yet\n");
        return 0;
    }

    /* temperature: 0.01 °C -> X.XX °C */
    rt_kprintf("BME280:\n");
    rt_kprintf("  Temperature: %d.%02d C\n",
               r.temperature / 100,
               (r.temperature < 0 ? -r.temperature : r.temperature) % 100);

    /* pressure: Pa*256 -> hPa (divide by 256 then by 100) */
    rt_uint32_t pa      = r.pressure >> 8;         /* Pa */
    rt_uint32_t hpa_int = pa / 100;
    rt_uint32_t hpa_frac = (pa % 100);
    rt_kprintf("  Pressure:    %u.%02u hPa\n", hpa_int, hpa_frac);

    /* humidity: %RH*1024 -> X.X %RH */
    rt_uint32_t rh_int  = r.humidity >> 10;
    rt_uint32_t rh_frac = (r.humidity & 0x3FF) * 100 / 1024;
    rt_kprintf("  Humidity:    %u.%02u %%RH\n", rh_int, rh_frac);

    rt_kprintf("  Timestamp:   %u ticks\n", r.timestamp);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_bme280, bme280, read BME280 temp/humidity/pressure);

/*---------------------------------------------------------------------------
 * MSH command: i2c_scan  — scan all 7-bit addresses on i2c1
 *---------------------------------------------------------------------------*/
static int _cmd_i2c_scan(int argc, char **argv)
{
    struct rt_i2c_bus_device *bus;
    rt_uint8_t  dummy;
    int         found = 0;
    rt_uint8_t  addr;
    struct rt_i2c_msg msg;

    bus = (struct rt_i2c_bus_device *)rt_device_find(BME280_I2C_BUS_NAME);
    if (bus == RT_NULL)
    {
        rt_kprintf("I2C bus '%s' not found\n", BME280_I2C_BUS_NAME);
        return -1;
    }

    rt_kprintf("Scanning I2C bus '%s' (7-bit addresses 0x08-0x77)...\n",
               BME280_I2C_BUS_NAME);

    for (addr = 0x08; addr <= 0x77; addr++)
    {
        msg.addr  = addr;
        msg.flags = RT_I2C_RD;
        msg.buf   = &dummy;
        msg.len   = 1;

        if (rt_i2c_transfer(bus, &msg, 1) == 1)
        {
            rt_kprintf("  found device at 0x%02X\n", addr);
            found++;
        }
    }

    if (found == 0)
        rt_kprintf("  no devices found\n");
    else
        rt_kprintf("  total: %d device(s)\n", found);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_i2c_scan, i2c_scan, scan all devices on i2c1);

/*---------------------------------------------------------------------------
 * I2C bus recovery
 * If a slave holds SDA low (bus stuck), toggle SCL up to 9 times to let
 * the slave finish its byte and release SDA, then send a STOP condition.
 *---------------------------------------------------------------------------*/
static void _i2c_bus_recover(void)
{
    int i;

    LOG_W("attempting I2C bus recovery...");

    /* Temporarily take over the pins as GPIO output */
    rt_pin_mode(BME280_SCL_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(BME280_SDA_PIN, PIN_MODE_OUTPUT);

    rt_pin_write(BME280_SDA_PIN, PIN_HIGH);

    for (i = 0; i < 9; i++)
    {
        rt_pin_write(BME280_SCL_PIN, PIN_HIGH);
        rt_hw_us_delay(5);
        rt_pin_write(BME280_SCL_PIN, PIN_LOW);
        rt_hw_us_delay(5);

        /* If SDA is high now, slave released the bus */
        rt_pin_mode(BME280_SDA_PIN, PIN_MODE_INPUT);
        if (rt_pin_read(BME280_SDA_PIN) == PIN_HIGH)
        {
            LOG_W("SDA released after %d clocks", i + 1);
            break;
        }
        rt_pin_mode(BME280_SDA_PIN, PIN_MODE_OUTPUT);
    }

    /* Generate STOP: SDA low → SCL high → SDA high */
    rt_pin_mode(BME280_SDA_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BME280_SDA_PIN, PIN_LOW);
    rt_hw_us_delay(5);
    rt_pin_write(BME280_SCL_PIN, PIN_HIGH);
    rt_hw_us_delay(5);
    rt_pin_write(BME280_SDA_PIN, PIN_HIGH);
    rt_hw_us_delay(5);

    /*
     * Release pins back to open-drain for the soft I2C driver.
     * The drv_soft_i2c driver re-configures them on next transfer.
     */
    rt_pin_mode(BME280_SCL_PIN, PIN_MODE_OUTPUT_OD);
    rt_pin_mode(BME280_SDA_PIN, PIN_MODE_OUTPUT_OD);
    rt_pin_write(BME280_SCL_PIN, PIN_HIGH);
    rt_pin_write(BME280_SDA_PIN, PIN_HIGH);

    rt_thread_mdelay(10);
    LOG_W("bus recovery done");
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t bme280_init(void)
{
    rt_thread_t tid;
    rt_err_t    ret;

    if (s_initialized)
        return RT_EOK;

    /* Small delay so MAX30102 init (called before us) finishes and
     * releases the I2C bus before we start talking to BME280 */
    rt_thread_mdelay(50);

    /* Attempt bus recovery in case BME280 is holding SDA low
     * (e.g. after a previous failed transaction or power glitch) */
    _i2c_bus_recover();

    /* Find I2C bus */
    s_i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(BME280_I2C_BUS_NAME);
    if (s_i2c_bus == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found", BME280_I2C_BUS_NAME);
        return -RT_ENOSYS;
    }

    /* Initialise result mutex */
    ret = rt_mutex_init(&s_mutex, "bme280_mtx", RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK)
    {
        LOG_E("mutex init failed");
        return ret;
    }

    /* Configure sensor */
    ret = _configure();
    if (ret != RT_EOK)
    {
        rt_mutex_detach(&s_mutex);
        return ret;
    }

    /* Allow one standby period before first read */
    rt_thread_mdelay(1100);

    /* Create sampling thread */
    tid = rt_thread_create("bme280",
                           _bme280_thread,
                           RT_NULL,
                           512,           /* stack: smaller than max30102, no big arrays */
                           RT_THREAD_PRIORITY_MAX / 2 + 2,
                           20);
    if (tid == RT_NULL)
    {
        LOG_E("thread create failed");
        rt_mutex_detach(&s_mutex);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    s_initialized = RT_TRUE;
    LOG_I("init OK");
    return RT_EOK;
}

rt_err_t bme280_get_result(bme280_result_t *result)
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
