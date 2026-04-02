/*
 * MQ Gas Sensor Driver (MQ-4 Methane, MQ-7 Carbon Monoxide)
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   MQ-4 VCC  -> 5V (heater needs 5V)
 *   MQ-4 GND  -> GND
 *   MQ-4 AO   -> PA4 (ADC1_CH4)
 *   MQ-4 DO   -> PC0 (optional, digital alarm)
 *
 *   MQ-7 VCC  -> 5V (heater needs 5V)
 *   MQ-7 GND  -> GND
 *   MQ-7 AO   -> PA5 (ADC1_CH5)
 *   MQ-7 DO   -> PC1 (optional, digital alarm)
 *
 * Notes:
 *   - MQ sensors need 24-48 hours preheat for accurate readings
 *   - AO output is typically 0-3.3V (module has voltage divider)
 *   - DO is active LOW when gas concentration exceeds threshold
 *   - Adjust on-board potentiometer to set DO trigger threshold
 *   - PB12 is reserved for SD card SPI CS pin
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-21   first version for Guardian project
 * 2026-03-22   fix: change DO pins from PB12/PB13 to PC0/PC1 to avoid SPI2 conflict
 */

#ifndef __SENSOR_MQ_GAS_H__
#define __SENSOR_MQ_GAS_H__

#include <rtthread.h>
#include <rtdevice.h>
#include <drv_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware pin definitions
 *---------------------------------------------------------------------------*/
#define MQ4_ADC_CHANNEL         4       /* PA4 = ADC1_CH4 */
#define MQ7_ADC_CHANNEL         5       /* PA5 = ADC1_CH5 */

#define MQ4_DO_PIN              GET_PIN(C, 0)   /* PC0: Digital output (optional) */
#define MQ7_DO_PIN              GET_PIN(C, 1)   /* PC1: Digital output (optional) */

/*---------------------------------------------------------------------------
 * Alarm thresholds (ADC raw value, 12-bit: 0-4095)
 * These are approximate and should be calibrated for your environment
 * 12-bit ADC: 0-4095, 3.3V reference
 * 1000 raw ~ 0.8V, 2000 raw ~ 1.6V, 3000 raw ~ 2.4V
 *---------------------------------------------------------------------------*/
#define MQ4_WARNING_THRESHOLD   1500    /* ~1.2V - elevated methane */
#define MQ4_DANGER_THRESHOLD    2500    /* ~2.0V - dangerous methane level */
#define MQ7_WARNING_THRESHOLD   1500    /* ~1.2V - elevated CO */
#define MQ7_DANGER_THRESHOLD    2500    /* ~2.0V - dangerous CO level */

/*---------------------------------------------------------------------------
 * Gas sensor type
 *---------------------------------------------------------------------------*/
typedef enum
{
    GAS_TYPE_MQ4_METHANE = 0,   /* MQ-4: Methane, natural gas */
    GAS_TYPE_MQ7_CO,            /* MQ-7: Carbon monoxide */
    GAS_TYPE_MAX
} gas_type_t;

/*---------------------------------------------------------------------------
 * Alarm level
 *---------------------------------------------------------------------------*/
typedef enum
{
    GAS_LEVEL_NORMAL = 0,       /* Safe level */
    GAS_LEVEL_WARNING,          /* Elevated, be cautious */
    GAS_LEVEL_DANGER            /* Dangerous, take action */
} gas_level_t;

/*---------------------------------------------------------------------------
 * Result structure
 *---------------------------------------------------------------------------*/
typedef struct
{
    /* MQ-4 Methane sensor */
    rt_uint16_t mq4_raw;        /* ADC raw value (0-4095) */
    rt_uint16_t mq4_mv;         /* Voltage in mV (0-3300) */
    gas_level_t mq4_level;      /* Alarm level */
    rt_bool_t   mq4_do_alarm;   /* DO pin alarm state (active low) */

    /* MQ-7 CO sensor */
    rt_uint16_t mq7_raw;        /* ADC raw value (0-4095) */
    rt_uint16_t mq7_mv;         /* Voltage in mV (0-3300) */
    gas_level_t mq7_level;      /* Alarm level */
    rt_bool_t   mq7_do_alarm;   /* DO pin alarm state (active low) */

    /* Status */
    rt_bool_t valid;
    rt_uint32_t timestamp;
} mq_gas_result_t;

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize MQ gas sensors (ADC + GPIO)
 * @return RT_EOK on success, negative error code on failure
 */
rt_err_t mq_gas_init(void);

/**
 * @brief  Get latest sensor readings (thread-safe)
 * @param  result  Output buffer
 * @return RT_EOK on success
 */
rt_err_t mq_gas_get_result(mq_gas_result_t *result);

/**
 * @brief  Check if any gas alarm is active
 * @return RT_TRUE if alarm, RT_FALSE if safe
 */
rt_bool_t mq_gas_is_alarm(void);

/**
 * @brief  Get gas level name string
 * @param  level  Gas level enum
 * @return String representation
 */
const char *mq_gas_level_str(gas_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_MQ_GAS_H__ */
