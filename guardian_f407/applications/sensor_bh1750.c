/*
 * BH1750 Ambient Light Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Protocol notes:
 *   BH1750 has no register address — you write a single command byte,
 *   then read back 2 bytes (big-endian 16-bit raw count).
 *
 *   lux = raw_count / 1.2   (for H-resolution mode, MTreg = 69)
 *
 *   In continuous H-resolution mode the sensor auto-refreshes every ~120 ms.
 *   We poll at 1 Hz which is more than sufficient.
 */

#define LOG_TAG     "bh1750"
#define LOG_LVL     DBG_INFO
#include <rtdbg.h>

#include "sensor_bh1750.h"

/*---------------------------------------------------------------------------
 * Internal state
 *---------------------------------------------------------------------------*/
static struct rt_i2c_bus_device *s_i2c_bus   = RT_NULL;
static bh1750_result_t           s_result;
static struct rt_mutex            s_mutex;
static rt_bool_t                  s_initialized = RT_FALSE;

/*---------------------------------------------------------------------------
 * Low-level I2C helpers
 *---------------------------------------------------------------------------*/

/**
 * Send a single command byte to BH1750 (write-only transaction).
 */
static rt_err_t _send_cmd(rt_uint8_t cmd)
{
    struct rt_i2c_msg msg;

    msg.addr  = BH1750_I2C_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = &cmd;
    msg.len   = 1;

    if (rt_i2c_transfer(s_i2c_bus, &msg, 1) != 1)
    {
        LOG_E("send cmd 0x%02X failed", cmd);
        return -RT_EIO;
    }
    return RT_EOK;
}

/**
 * Read 2 bytes from BH1750 (big-endian raw count).
 */
static rt_err_t _read_raw(rt_uint16_t *raw)
{
    rt_uint8_t buf[2];
    struct rt_i2c_msg msg;

    msg.addr  = BH1750_I2C_ADDR;
    msg.flags = RT_I2C_RD;
    msg.buf   = buf;
    msg.len   = 2;

    if (rt_i2c_transfer(s_i2c_bus, &msg, 1) != 1)
    {
        LOG_E("read raw failed");
        return -RT_EIO;
    }

    *raw = (rt_uint16_t)(buf[0] << 8) | buf[1];
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Read one sample and update result
 *---------------------------------------------------------------------------*/
static rt_err_t _read_sample(void)
{
    rt_uint16_t raw = 0;
    rt_uint32_t lux;
    rt_err_t    ret;

    ret = _read_raw(&raw);
    if (ret != RT_EOK)
        return ret;

    /*
     * lux = raw / 1.2
     * Using integer arithmetic: lux = raw * 10 / 12
     * This avoids float and keeps 1 lux precision (sufficient for this app).
     */
    lux = (rt_uint32_t)raw * 10 / 12;

    LOG_D("raw=%u  lux=%u", raw, lux);

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    s_result.lux       = lux;
    s_result.valid     = 1;
    s_result.timestamp = rt_tick_get();
    rt_mutex_release(&s_mutex);

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Background sampling thread (1 Hz)
 *---------------------------------------------------------------------------*/
static void _bh1750_thread(void *param)
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
            rt_thread_mdelay(1000);
        }
    }
}

/*---------------------------------------------------------------------------
 * MSH command: bh1750
 *---------------------------------------------------------------------------*/
static int _cmd_bh1750(int argc, char **argv)
{
    bh1750_result_t r;
    (void)argc;
    (void)argv;

    bh1750_get_result(&r);

    if (!r.valid)
        rt_kprintf("BH1750: no valid data yet\n");
    else
        rt_kprintf("BH1750: %u lux  (ts=%u)\n", r.lux, r.timestamp);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_bh1750, bh1750, read BH1750 ambient light);

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t bh1750_init(void)
{
    rt_thread_t tid;
    rt_err_t    ret;

    if (s_initialized)
        return RT_EOK;

    /* Find shared I2C bus */
    s_i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(BH1750_I2C_BUS_NAME);
    if (s_i2c_bus == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found", BH1750_I2C_BUS_NAME);
        return -RT_ENOSYS;
    }

    /* Initialise result mutex */
    ret = rt_mutex_init(&s_mutex, "bh1750_mtx", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
    {
        LOG_E("mutex init failed");
        return ret;
    }

    /* Power on */
    ret = _send_cmd(BH1750_CMD_POWER_ON);
    if (ret != RT_EOK)
    {
        LOG_E("power on failed — check wiring (ADDR=GND -> 0x23, ADDR=VCC -> 0x5C)");
        rt_mutex_detach(&s_mutex);
        return ret;
    }

    /* Reset internal data register */
    ret = _send_cmd(BH1750_CMD_RESET);
    if (ret != RT_EOK)
    {
        rt_mutex_detach(&s_mutex);
        return ret;
    }

    /* Start continuous H-resolution mode (1 lx resolution, ~120 ms/sample) */
    ret = _send_cmd(BH1750_CMD_CONT_H_RES);
    if (ret != RT_EOK)
    {
        rt_mutex_detach(&s_mutex);
        return ret;
    }

    /* Wait for the first measurement to complete */
    rt_thread_mdelay(180);

    /* Create sampling thread */
    tid = rt_thread_create("bh1750",
                           _bh1750_thread,
                           RT_NULL,
                           512,
                           RT_THREAD_PRIORITY_MAX / 2 + 3,
                           20);
    if (tid == RT_NULL)
    {
        LOG_E("thread create failed");
        rt_mutex_detach(&s_mutex);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    s_initialized = RT_TRUE;
    LOG_I("init OK — continuous H-res mode");
    return RT_EOK;
}

rt_err_t bh1750_get_result(bh1750_result_t *result)
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
