/*
 * MQ Gas Sensor Driver (MQ-4 Methane, MQ-7 Carbon Monoxide)
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Uses RT-Thread ADC framework
 * Requires: BSP_USING_ADC1 enabled in board config
 */

#include "sensor_mq_gas.h"
#include <rtdevice.h>

#define DBG_TAG "mq_gas"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * ADC device name (channels defined in header file)
 *---------------------------------------------------------------------------*/
#define ADC_DEV_NAME        "adc1"      /* RT-Thread ADC device name */

/* Note: Thresholds are now defined in sensor_mq_gas.h for centralized configuration */

/*---------------------------------------------------------------------------
 * Private variables
 *---------------------------------------------------------------------------*/
static rt_adc_device_t s_adc_dev = RT_NULL;
static struct rt_mutex s_mutex;
static rt_bool_t s_initialized = RT_FALSE;
static mq_gas_result_t s_result;

/*---------------------------------------------------------------------------
 * GPIO initialization for DO pins (optional digital alarm output)
 *---------------------------------------------------------------------------*/
static void _gpio_init(void)
{
    /* Configure DO pins as input with pull-up
     * DO is active low (low = alarm) */
    rt_pin_mode(MQ4_DO_PIN, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(MQ7_DO_PIN, PIN_MODE_INPUT_PULLUP);
}

/*---------------------------------------------------------------------------
 * Read ADC channel using RT-Thread ADC framework
 *---------------------------------------------------------------------------*/
static rt_uint32_t _adc_read_channel(rt_uint32_t channel)
{
    rt_uint32_t value = 0;

    if (s_adc_dev == RT_NULL)
        return 0;

    /* Enable ADC channel */
    rt_adc_enable(s_adc_dev, channel);

    /* Read ADC value */
    value = rt_adc_read(s_adc_dev, channel);

    /* Disable ADC channel */
    rt_adc_disable(s_adc_dev, channel);

    return value;
}

/*---------------------------------------------------------------------------
 * Determine gas level from ADC value
 *---------------------------------------------------------------------------*/
static gas_level_t _get_mq4_level(rt_uint16_t raw)
{
    if (raw >= MQ4_DANGER_THRESHOLD)
        return GAS_LEVEL_DANGER;
    else if (raw >= MQ4_WARNING_THRESHOLD)
        return GAS_LEVEL_WARNING;
    else
        return GAS_LEVEL_NORMAL;
}

static gas_level_t _get_mq7_level(rt_uint16_t raw)
{
    if (raw >= MQ7_DANGER_THRESHOLD)
        return GAS_LEVEL_DANGER;
    else if (raw >= MQ7_WARNING_THRESHOLD)
        return GAS_LEVEL_WARNING;
    else
        return GAS_LEVEL_NORMAL;
}

/*---------------------------------------------------------------------------
 * Sensor sampling thread
 *---------------------------------------------------------------------------*/
static void _mq_gas_thread(void *param)
{
    rt_uint32_t mq4_raw, mq7_raw;
    rt_uint16_t mq4_mv, mq7_mv;
    rt_bool_t mq4_do, mq7_do;

    (void)param;

    LOG_I("sampling thread started");

    /* Allow sensors to warm up */
    rt_thread_mdelay(1000);

    while (1)
    {
        /* Read ADC values */
        mq4_raw = _adc_read_channel(MQ4_ADC_CHANNEL);
        mq7_raw = _adc_read_channel(MQ7_ADC_CHANNEL);

        /* Convert to millivolts (3300mV / 4095) */
        mq4_mv = (rt_uint16_t)((rt_uint32_t)mq4_raw * 3300 / 4095);
        mq7_mv = (rt_uint16_t)((rt_uint32_t)mq7_raw * 3300 / 4095);

        /* Read DO pins (active low) */
        mq4_do = (rt_pin_read(MQ4_DO_PIN) == PIN_LOW) ? RT_TRUE : RT_FALSE;
        mq7_do = (rt_pin_read(MQ7_DO_PIN) == PIN_LOW) ? RT_TRUE : RT_FALSE;

        /* Update result (thread-safe) */
        rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);

        s_result.mq4_raw = (rt_uint16_t)mq4_raw;
        s_result.mq4_mv = mq4_mv;
        s_result.mq4_level = _get_mq4_level((rt_uint16_t)mq4_raw);
        s_result.mq4_do_alarm = mq4_do;

        s_result.mq7_raw = (rt_uint16_t)mq7_raw;
        s_result.mq7_mv = mq7_mv;
        s_result.mq7_level = _get_mq7_level((rt_uint16_t)mq7_raw);
        s_result.mq7_do_alarm = mq7_do;

        s_result.valid = RT_TRUE;
        s_result.timestamp = rt_tick_get();

        rt_mutex_release(&s_mutex);

        /* Sample every 500ms (gas changes slowly) */
        rt_thread_mdelay(500);
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t mq_gas_init(void)
{
    rt_err_t ret;
    rt_thread_t tid;

    if (s_initialized)
        return RT_EOK;

    /* Find ADC device */
    s_adc_dev = (rt_adc_device_t)rt_device_find(ADC_DEV_NAME);
    if (s_adc_dev == RT_NULL)
    {
        LOG_E("ADC device %s not found! Enable BSP_USING_ADC1 in menuconfig", ADC_DEV_NAME);
        return -RT_ERROR;
    }

    /* Initialize GPIO for DO pins */
    _gpio_init();

    /* Initialize mutex */
    ret = rt_mutex_init(&s_mutex, "mq_mtx", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to create mutex");
        return ret;
    }

    /* Initialize result structure */
    rt_memset(&s_result, 0, sizeof(s_result));

    /* Create sampling thread */
    tid = rt_thread_create("mq_gas",
                           _mq_gas_thread,
                           RT_NULL,
                           1024,
                           RT_THREAD_PRIORITY_MAX / 2 + 2,
                           20);
    if (tid == RT_NULL)
    {
        LOG_E("Failed to create thread");
        rt_mutex_detach(&s_mutex);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    s_initialized = RT_TRUE;
    LOG_I("init OK — MQ-4(PA4/CH4), MQ-7(PA5/CH5)");

    return RT_EOK;
}

rt_err_t mq_gas_get_result(mq_gas_result_t *result)
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

rt_bool_t mq_gas_is_alarm(void)
{
    rt_bool_t alarm = RT_FALSE;

    if (!s_initialized)
        return RT_FALSE;

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    alarm = (s_result.mq4_level >= GAS_LEVEL_WARNING) ||
            (s_result.mq7_level >= GAS_LEVEL_WARNING) ||
            s_result.mq4_do_alarm ||
            s_result.mq7_do_alarm;
    rt_mutex_release(&s_mutex);

    return alarm;
}

const char *mq_gas_level_str(gas_level_t level)
{
    switch (level)
    {
    case GAS_LEVEL_NORMAL:  return "OK";
    case GAS_LEVEL_WARNING: return "WARN";
    case GAS_LEVEL_DANGER:  return "DANGER";
    default:                return "?";
    }
}

/*---------------------------------------------------------------------------
 * MSH Commands
 *---------------------------------------------------------------------------*/
static int _cmd_mq_gas(int argc, char **argv)
{
    mq_gas_result_t r;
    (void)argc;
    (void)argv;

    if (!s_initialized)
    {
        rt_kprintf("MQ gas sensors not initialized\n");
        return -1;
    }

    mq_gas_get_result(&r);

    rt_kprintf("MQ Gas Sensor Data:\n");
    rt_kprintf("  MQ-4 (Methane): raw=%4u  %4umV  level=%s  DO=%s\n",
               r.mq4_raw, r.mq4_mv,
               mq_gas_level_str(r.mq4_level),
               r.mq4_do_alarm ? "ALARM!" : "ok");
    rt_kprintf("  MQ-7 (CO):      raw=%4u  %4umV  level=%s  DO=%s\n",
               r.mq7_raw, r.mq7_mv,
               mq_gas_level_str(r.mq7_level),
               r.mq7_do_alarm ? "ALARM!" : "ok");

    if (mq_gas_is_alarm())
        rt_kprintf("  >>> GAS ALARM ACTIVE! <<<\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_mq_gas, mq_gas, read MQ gas sensor data);

/* Test command to read raw ADC continuously */
static int _cmd_mq_raw(int argc, char **argv)
{
    int count = 10;
    int i;

    (void)argc;
    (void)argv;

    if (s_adc_dev == RT_NULL)
    {
        rt_kprintf("ADC device not found\n");
        return -1;
    }

    rt_kprintf("Reading MQ sensors 10 times...\n");
    for (i = 0; i < count; i++)
    {
        rt_uint32_t mq4 = _adc_read_channel(MQ4_ADC_CHANNEL);
        rt_uint32_t mq7 = _adc_read_channel(MQ7_ADC_CHANNEL);
        rt_kprintf("  [%2d] MQ4=%4u (%4umV)  MQ7=%4u (%4umV)\n",
                   i + 1,
                   mq4, mq4 * 3300 / 4095,
                   mq7, mq7 * 3300 / 4095);
        rt_thread_mdelay(500);
    }

    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_mq_raw, mq_raw, read MQ ADC raw values continuously);
