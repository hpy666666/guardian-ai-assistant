/*
 * MAX30102 Heart Rate & SpO2 Sensor Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Architecture:
 *   - One RT-Thread thread (max30102_thread) owns all I2C access.
 *   - Sampling uses FIFO pointer polling (WR_PTR != RD_PTR) — no INT pin
 *     dependency. INT pin wiring is optional; the driver works without it.
 *   - Results are published into a mutex-protected structure.
 *   - Caller uses max30102_get_result() to read latest values at any time.
 *
 * Key fixes vs. vendor example:
 *   1. FIFO data mask explicitly 0x0003FFFF (18-bit, matches LED_PW=0x03).
 *   2. No "hr - 20" hack.
 *   3. I2C write delay reduced from 10 ms to 1 ms.
 *   4. Finger-presence detection guards against publishing garbage data.
 *   5. FIFO pointer polling — robust regardless of INT pin state.
 */

#include "sensor_max30102.h"
#include "algo_spo2.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#define DBG_TAG  "max30102"
#define DBG_LVL  DBG_INFO
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Thread / OS resources
 *---------------------------------------------------------------------------*/
#define MAX30102_THREAD_STACK   2048
#define MAX30102_THREAD_PRIO    15
#define MAX30102_THREAD_TICK    10

/* Mutex protecting the published result. */
static struct rt_mutex     s_result_mutex;

/* Latest published result. */
static max30102_result_t   s_result;

/* I2C bus handle. */
static struct rt_i2c_bus_device *s_i2c_bus = RT_NULL;

/* Thread handle. */
static rt_thread_t s_thread = RT_NULL;

/*---------------------------------------------------------------------------
 * Sample ring buffers (static — do NOT put these on stack)
 *---------------------------------------------------------------------------*/
static rt_uint32_t s_ir_buf[MAX30102_BUFFER_SIZE];
static rt_uint32_t s_red_buf[MAX30102_BUFFER_SIZE];

/*---------------------------------------------------------------------------
 * Low-level I2C helpers
 *---------------------------------------------------------------------------*/
rt_err_t max30102_write_reg(rt_uint8_t reg, rt_uint8_t data)
{
    rt_uint8_t buf[2] = { reg, data };
    struct rt_i2c_msg msg;

    msg.addr  = MAX30102_I2C_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = buf;
    msg.len   = 2;

    if (rt_i2c_transfer(s_i2c_bus, &msg, 1) != 1)
    {
        LOG_E("write reg 0x%02X failed", reg);
        return -RT_EIO;
    }
    rt_thread_mdelay(1);    /* MAX30102 register settle — 1 ms is enough */
    return RT_EOK;
}

rt_err_t max30102_read_reg(rt_uint8_t reg, rt_uint8_t *data)
{
    struct rt_i2c_msg msgs[2];

    /* Write register address */
    msgs[0].addr  = MAX30102_I2C_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    /* Read data byte */
    msgs[1].addr  = MAX30102_I2C_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = data;
    msgs[1].len   = 1;

    if (rt_i2c_transfer(s_i2c_bus, msgs, 2) != 2)
    {
        LOG_E("read reg 0x%02X failed", reg);
        return -RT_EIO;
    }
    return RT_EOK;
}

/**
 * @brief  Read 'count' consecutive bytes starting at register 'reg'.
 *         Used for burst FIFO reads (6 bytes per sample).
 */
static rt_err_t _read_bytes(rt_uint8_t reg, rt_uint8_t *buf, rt_uint16_t len)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = MAX30102_I2C_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    msgs[1].addr  = MAX30102_I2C_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    if (rt_i2c_transfer(s_i2c_bus, msgs, 2) != 2)
        return -RT_EIO;
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * MAX30102 chip-level operations
 *---------------------------------------------------------------------------*/
static rt_err_t _reset(void)
{
    rt_err_t ret;
    rt_uint8_t val;

    /* Write RESET bit */
    ret = max30102_write_reg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_RESET);
    if (ret != RT_EOK) return ret;

    /* Poll until RESET bit self-clears (typ < 1 ms) */
    rt_uint8_t timeout = 100;
    do {
        rt_thread_mdelay(1);
        ret = max30102_read_reg(MAX30102_REG_MODE_CONFIG, &val);
        if (ret != RT_EOK) return ret;
    } while ((val & MAX30102_MODE_RESET) && --timeout);

    if (timeout == 0)
    {
        LOG_E("reset timeout");
        return -RT_ETIMEOUT;
    }
    return RT_EOK;
}

static rt_err_t _configure(void)
{
    rt_err_t ret = RT_EOK;

    /* Clear FIFO pointers */
    ret |= max30102_write_reg(MAX30102_REG_FIFO_WR_PTR, 0x00);
    ret |= max30102_write_reg(MAX30102_REG_OVF_COUNTER,  0x00);
    ret |= max30102_write_reg(MAX30102_REG_FIFO_RD_PTR, 0x00);

    /*
     * FIFO_CONFIG = 0x0F
     *   SMP_AVE[7:5]       = 000  → no averaging (1 sample)
     *   FIFO_ROLLOVER_EN[4]= 0    → stop at full, do not overwrite
     *   FIFO_A_FULL[3:0]   = 1111 → interrupt when 17 empty slots remain
     */
    ret |= max30102_write_reg(MAX30102_REG_FIFO_CONFIG, 0x0F);

    /*
     * MODE_CONFIG = 0x03  → SpO2 mode (Red LED + IR LED both active)
     */
    ret |= max30102_write_reg(MAX30102_REG_MODE_CONFIG, MAX30102_MODE_SPO2);

    /*
     * SPO2_CONFIG = 0x27
     *   SPO2_ADC_RGE[6:5] = 01  → 4096 nA full scale  (best SNR for finger)
     *   SPO2_SR[4:2]       = 001 → 100 samples/s
     *   LED_PW[1:0]        = 11  → 411 µs pulse → 18-bit ADC resolution
     *
     * The 18-bit choice means raw FIFO values must be masked with 0x0003FFFF.
     */
    ret |= max30102_write_reg(MAX30102_REG_SPO2_CONFIG,
                              MAX30102_ADC_RGE_4096 |
                              MAX30102_SR_100        |
                              MAX30102_PW_411);

    /*
     * LED currents
     *   0x24 = 36 steps × 0.2 mA/step = 7.2 mA  (good for most fingers)
     *   Increase to 0x3F (~12.6 mA) for dark skin or thick fingers.
     */
    ret |= max30102_write_reg(MAX30102_REG_LED1_PA,  0x24);   /* Red */
    ret |= max30102_write_reg(MAX30102_REG_LED2_PA,  0x24);   /* IR  */
    ret |= max30102_write_reg(MAX30102_REG_PILOT_PA, 0x7F);   /* 25 mA pilot (multi-LED mode, harmless in SpO2 mode) */

    /*
     * Interrupts: disabled — we use FIFO pointer polling instead.
     * The INT pin is still pulled low by the chip when data is ready,
     * which can be optionally used in a future optimisation.
     */
    ret |= max30102_write_reg(MAX30102_REG_INT_ENABLE1, 0x00);
    ret |= max30102_write_reg(MAX30102_REG_INT_ENABLE2, 0x00);

    return ret;
}

/*---------------------------------------------------------------------------
 * Finger presence detection
 * IR signal > ~50 000 raw counts typically indicates finger contact.
 * Below this threshold results are meaningless noise.
 *---------------------------------------------------------------------------*/
#define FINGER_THRESHOLD    50000UL

/*---------------------------------------------------------------------------
 * Smoothing filter state
 *
 * Two-layer filter applied before publishing results:
 *
 * Layer 1 — Median filter (window = 5)
 *   Keep the last 5 valid raw algorithm outputs.
 *   Publish the median, which kills single-sample spikes (181, 133 bpm).
 *
 * Layer 2 — Step-change suppression
 *   If the new median jumps > HR_MAX_STEP bpm from the last published value,
 *   hold the old value and increment a counter.  After HR_MAX_HOLD consecutive
 *   suppressions the new value is accepted anyway (handles genuine fast changes).
 *---------------------------------------------------------------------------*/
#define SMOOTH_WIN          5       /* median window size (must be odd) */
#define HR_MAX_STEP         20      /* bpm — max tolerated jump per update */
#define HR_MAX_HOLD         5       /* consecutive suppressions before forced accept */
#define SPO2_MAX_STEP       5       /* % — max tolerated SpO2 jump per update */
#define SPO2_MAX_HOLD       5

/* Physiological plausibility gate before entering median window */
#define HR_PLAUS_MIN        40      /* bpm — below this is impossible at rest */
#define HR_PLAUS_MAX        150     /* bpm — above this treated as noise spike */

/* Circular buffers for the last SMOOTH_WIN valid raw values */
static rt_int32_t s_hr_win[SMOOTH_WIN];
static rt_int32_t s_spo2_win[SMOOTH_WIN];
static rt_uint8_t s_hr_win_cnt   = 0;   /* how many slots are filled */
static rt_uint8_t s_spo2_win_cnt = 0;
static rt_uint8_t s_hr_win_idx   = 0;   /* next write position */
static rt_uint8_t s_spo2_win_idx = 0;

/* Last published (post-filter) values for step-change check */
static rt_int32_t s_hr_last   = -999;
static rt_int32_t s_spo2_last = -999;

/* Suppression counters */
static rt_uint8_t s_hr_hold_cnt   = 0;
static rt_uint8_t s_spo2_hold_cnt = 0;

/* Insert a copy of arr[], sort ascending in-place (insertion sort, n<=5) */
static rt_int32_t _median(rt_int32_t *arr, rt_uint8_t n)
{
    rt_int32_t tmp[SMOOTH_WIN];
    rt_int32_t t;
    rt_uint8_t i, j;
    for (i = 0; i < n; i++) tmp[i] = arr[i];
    for (i = 1; i < n; i++)
    {
        t = tmp[i];
        for (j = i; j > 0 && tmp[j-1] > t; j--)
            tmp[j] = tmp[j-1];
        tmp[j] = t;
    }
    return tmp[n / 2];
}

/**
 * @brief  Push a new raw value into the median window and return the median.
 *         win[]  — circular buffer
 *         cnt    — how many slots are currently filled (saturates at SMOOTH_WIN)
 *         idx    — next write index
 */
static rt_int32_t _push_and_median(rt_int32_t val,
                                   rt_int32_t *win,
                                   rt_uint8_t *cnt,
                                   rt_uint8_t *idx)
{
    win[*idx] = val;
    *idx = (rt_uint8_t)((*idx + 1) % SMOOTH_WIN);
    if (*cnt < SMOOTH_WIN) (*cnt)++;
    return _median(win, *cnt);
}

/**
 * @brief  Apply step-change suppression.
 *         Returns the value that should actually be published.
 *         last_pub  — last published value (updated in-place when accepted)
 *         hold_cnt  — suppression counter (updated in-place)
 *         max_step  — maximum allowed jump
 *         max_hold  — forced-accept after this many suppressions
 */
static rt_int32_t _suppress_step(rt_int32_t new_val,
                                 rt_int32_t *last_pub,
                                 rt_uint8_t *hold_cnt,
                                 rt_int32_t  max_step,
                                 rt_uint8_t  max_hold)
{
    /* If no previous value, always accept */
    if (*last_pub == -999)
    {
        *last_pub  = new_val;
        *hold_cnt  = 0;
        return new_val;
    }

    rt_int32_t diff = new_val - *last_pub;
    if (diff < 0) diff = -diff;

    if (diff <= max_step || *hold_cnt >= max_hold)
    {
        /* Accept */
        *last_pub  = new_val;
        *hold_cnt  = 0;
        return new_val;
    }
    else
    {
        /* Suppress: return last published value */
        (*hold_cnt)++;
        return *last_pub;
    }
}

static rt_bool_t _finger_present(void)
{
    rt_uint32_t sum = 0;
    rt_uint32_t count = 50;  /* average last 50 samples */
    if (count > MAX30102_BUFFER_SIZE) count = MAX30102_BUFFER_SIZE;
    for (rt_uint32_t i = MAX30102_BUFFER_SIZE - count; i < MAX30102_BUFFER_SIZE; i++)
        sum += s_ir_buf[i];
    return (sum / count) > FINGER_THRESHOLD;
}

/*---------------------------------------------------------------------------
 * FIFO pointer polling
 *
 * Returns the number of unread samples currently in the FIFO.
 * Formula: num_samples = (WR_PTR - RD_PTR) & 0x1F
 * The FIFO is 32 slots deep (indices 0–31, wrapping modulo 32).
 *---------------------------------------------------------------------------*/
static rt_uint8_t _fifo_available(void)
{
    rt_uint8_t wr = 0, rd = 0;
    max30102_read_reg(MAX30102_REG_FIFO_WR_PTR, &wr);
    max30102_read_reg(MAX30102_REG_FIFO_RD_PTR, &rd);
    return (rt_uint8_t)((wr - rd) & 0x1FU);
}

/*---------------------------------------------------------------------------
 * Block until at least one sample is available in the FIFO.
 * Polls every 5 ms; at 100 sps a new sample arrives every 10 ms,
 * so we will never wait more than ~15 ms.
 * Returns RT_EOK when data is ready, -RT_ETIMEOUT after ~5 seconds.
 *---------------------------------------------------------------------------*/
static rt_err_t _wait_for_sample(void)
{
    rt_uint16_t retries = 1000;   /* 1000 × 5 ms = 5 s max */
    while (retries--)
    {
        if (_fifo_available() > 0)
            return RT_EOK;
        rt_thread_mdelay(5);
    }
    return -RT_ETIMEOUT;
}

/*---------------------------------------------------------------------------
 * Read one FIFO sample (6 bytes → Red 18-bit + IR 18-bit).
 * Caller must ensure a sample is available before calling.
 *---------------------------------------------------------------------------*/
static rt_err_t _read_fifo_sample(rt_uint32_t *red, rt_uint32_t *ir)
{
    rt_uint8_t raw[6];

    if (_read_bytes(MAX30102_REG_FIFO_DATA, raw, 6) != RT_EOK)
        return -RT_EIO;

    /*
     * Byte layout per sample (SpO2 mode, 18-bit ADC, LED_PW=0x03):
     *   raw[0] = RED[17:16]  (only bits [1:0] are data; [7:2] are zero)
     *   raw[1] = RED[15:8]
     *   raw[2] = RED[7:0]
     *   raw[3] = IR[17:16]
     *   raw[4] = IR[15:8]
     *   raw[5] = IR[7:0]
     *
     * Always mask with MAX30102_DATA_MASK (0x0003FFFF) to strip any
     * reserved bits that might accidentally be set.
     */
    *red = (((rt_uint32_t)(raw[0] & 0x03U)) << 16)
         | (((rt_uint32_t)raw[1]) << 8)
         |  ((rt_uint32_t)raw[2]);
    *red &= MAX30102_DATA_MASK;

    *ir  = (((rt_uint32_t)(raw[3] & 0x03U)) << 16)
         | (((rt_uint32_t)raw[4]) << 8)
         |  ((rt_uint32_t)raw[5]);
    *ir  &= MAX30102_DATA_MASK;

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Sensor thread
 *---------------------------------------------------------------------------*/
static void _max30102_thread_entry(void *param)
{
    (void)param;
    rt_uint32_t red_sample, ir_sample;
    rt_int32_t  hr, spo2;
    rt_int8_t   hr_valid, spo2_valid;
    rt_uint32_t i;

    LOG_I("thread started");

    /* ---- Phase 1: fill buffer with initial 500 samples ------------------ */
    for (i = 0; i < MAX30102_BUFFER_SIZE; i++)
    {
        /* Wait for at least one sample in FIFO (polls every 5 ms) */
        if (_wait_for_sample() != RT_EOK)
        {
            LOG_E("FIFO wait timeout at slot %d — check wiring", i);
            i--;    /* retry this slot */
            continue;
        }

        if (_read_fifo_sample(&red_sample, &ir_sample) != RT_EOK)
        {
            LOG_E("FIFO read error at slot %d", i);
            i--;
            continue;
        }

        s_red_buf[i] = red_sample;
        s_ir_buf[i]  = ir_sample;

        /* Progress log every 100 samples so the user can see it working */
        if ((i + 1) % 100 == 0)
            LOG_I("buffering... %d/500", i + 1);
    }

    LOG_I("buffer full, starting algorithm");

    /* First algorithm run on the initial 500-sample buffer */
    algo_heart_rate_and_spo2(s_ir_buf, MAX30102_BUFFER_SIZE, s_red_buf,
                             &spo2, &spo2_valid, &hr, &hr_valid);

    /* ---- Phase 2: sliding-window loop ----------------------------------- */
    while (1)
    {
        /*
         * Slide: drop oldest 100 samples, shift 100–499 → 0–399,
         * then fill positions 400–499 with 100 new samples.
         */
        for (i = 100; i < MAX30102_BUFFER_SIZE; i++)
        {
            s_red_buf[i - 100] = s_red_buf[i];
            s_ir_buf[i  - 100] = s_ir_buf[i];
        }

        for (i = (MAX30102_BUFFER_SIZE - 100); i < MAX30102_BUFFER_SIZE; i++)
        {
            if (_wait_for_sample() != RT_EOK)
            {
                LOG_W("sample wait timeout in sliding window, zero-padding");
                s_red_buf[i] = 0;
                s_ir_buf[i]  = 0;
                continue;
            }

            if (_read_fifo_sample(&red_sample, &ir_sample) != RT_EOK)
            {
                s_red_buf[i] = 0;
                s_ir_buf[i]  = 0;
            }
            else
            {
                s_red_buf[i] = red_sample;
                s_ir_buf[i]  = ir_sample;
            }
        }

        /* Run algorithm on the refreshed 500-sample window */
        algo_heart_rate_and_spo2(s_ir_buf, MAX30102_BUFFER_SIZE, s_red_buf,
                                 &spo2, &spo2_valid, &hr, &hr_valid);

        /* Publish result, gated by finger-presence check */
        rt_mutex_take(&s_result_mutex, RT_WAITING_FOREVER);

        s_result.timestamp = rt_tick_get();

        if (_finger_present())
        {
            /* --- Heart rate --- */
            if (hr_valid && hr > 30 && hr < 220)
            {
                /*
                 * Plausibility gate: only feed physiologically reasonable
                 * values into the median window.  Values like 200/333 bpm
                 * (algorithm cold-start or motion artefact) are discarded
                 * here and do not corrupt the window.
                 * If the window is still empty (cnt==0) we accept 40-150;
                 * once we have a baseline we accept 40-150 unconditionally
                 * (step suppression handles the rest).
                 */
                if (hr >= HR_PLAUS_MIN && hr <= HR_PLAUS_MAX)
                {
                    /* Layer 1: median of last 5 plausible raw values */
                    rt_int32_t hr_med = _push_and_median(hr,
                                            s_hr_win,
                                            &s_hr_win_cnt,
                                            &s_hr_win_idx);

                    /* Layer 2: step-change suppression */
                    rt_int32_t hr_out = _suppress_step(hr_med,
                                            &s_hr_last,
                                            &s_hr_hold_cnt,
                                            HR_MAX_STEP,
                                            HR_MAX_HOLD);

                    s_result.heart_rate = hr_out;
                    s_result.hr_valid   = 1;
                }
                else
                {
                    /* Out-of-range spike — silently discard, keep last good */
                    if (s_hr_last != -999)
                    {
                        s_result.heart_rate = s_hr_last;
                        s_result.hr_valid   = 1;
                    }
                    else
                    {
                        s_result.heart_rate = -999;
                        s_result.hr_valid   = 0;
                    }
                }
            }
            else
            {
                /* Raw algorithm says invalid — don't update, keep last good */
                if (s_hr_last != -999)
                {
                    s_result.heart_rate = s_hr_last;
                    s_result.hr_valid   = 1;
                }
                else
                {
                    s_result.heart_rate = -999;
                    s_result.hr_valid   = 0;
                }
            }

            /* --- SpO2 --- */
            if (spo2_valid && spo2 >= 70 && spo2 <= 100)
            {
                rt_int32_t spo2_med = _push_and_median(spo2,
                                          s_spo2_win,
                                          &s_spo2_win_cnt,
                                          &s_spo2_win_idx);

                rt_int32_t spo2_out = _suppress_step(spo2_med,
                                          &s_spo2_last,
                                          &s_spo2_hold_cnt,
                                          SPO2_MAX_STEP,
                                          SPO2_MAX_HOLD);

                s_result.spo2       = spo2_out;
                s_result.spo2_valid = 1;
            }
            else
            {
                if (s_spo2_last != -999)
                {
                    s_result.spo2       = s_spo2_last;
                    s_result.spo2_valid = 1;
                }
                else
                {
                    s_result.spo2       = -999;
                    s_result.spo2_valid = 0;
                }
            }
        }
        else
        {
            /* Finger removed — clear output and reset all filter state */
            s_result.heart_rate = -999;
            s_result.hr_valid   = 0;
            s_result.spo2       = -999;
            s_result.spo2_valid = 0;

            s_hr_last       = -999;
            s_spo2_last     = -999;
            s_hr_win_cnt    = 0;
            s_spo2_win_cnt  = 0;
            s_hr_win_idx    = 0;
            s_spo2_win_idx  = 0;
            s_hr_hold_cnt   = 0;
            s_spo2_hold_cnt = 0;
        }

        rt_mutex_release(&s_result_mutex);

        LOG_D("raw HR=%d(%d) SpO2=%d(%d) → pub HR=%d SpO2=%d",
              hr, hr_valid, spo2, spo2_valid,
              s_result.heart_rate, s_result.spo2);
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t max30102_init(void)
{
    rt_err_t ret;
    rt_uint8_t part_id;

    /* Find the soft-I2C bus registered by drv_soft_i2c.c */
    s_i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(MAX30102_I2C_BUS_NAME);
    if (s_i2c_bus == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found", MAX30102_I2C_BUS_NAME);
        return -RT_ENOSYS;
    }

    /* Verify chip identity: PART_ID register must read 0x15 */
    ret = max30102_read_reg(MAX30102_REG_PART_ID, &part_id);
    if (ret != RT_EOK || part_id != 0x15)
    {
        LOG_E("PART_ID mismatch: got 0x%02X, expected 0x15", part_id);
        return -RT_ERROR;
    }
    LOG_I("MAX30102 detected (PART_ID=0x%02X)", part_id);

    /* Reset chip to known state */
    ret = _reset();
    if (ret != RT_EOK) return ret;

    /* Apply our configuration */
    ret = _configure();
    if (ret != RT_EOK)
    {
        LOG_E("configure failed");
        return ret;
    }

    /* Initialise result to invalid */
    rt_memset(&s_result, 0, sizeof(s_result));
    s_result.heart_rate = -999;
    s_result.spo2       = -999;

    /* Create result mutex */
    ret = rt_mutex_init(&s_result_mutex, "max_res", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK) return ret;

    /* Create and start the measurement thread */
    s_thread = rt_thread_create("max30102",
                                _max30102_thread_entry,
                                RT_NULL,
                                MAX30102_THREAD_STACK,
                                MAX30102_THREAD_PRIO,
                                MAX30102_THREAD_TICK);
    if (s_thread == RT_NULL)
    {
        LOG_E("thread create failed");
        return -RT_ENOMEM;
    }

    rt_thread_startup(s_thread);
    LOG_I("init OK — thread running at priority %d", MAX30102_THREAD_PRIO);

    return RT_EOK;
}

rt_err_t max30102_get_result(max30102_result_t *result)
{
    RT_ASSERT(result != RT_NULL);
    rt_mutex_take(&s_result_mutex, RT_WAITING_FOREVER);
    *result = s_result;
    rt_mutex_release(&s_result_mutex);
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Optional MSH debug commands
 *---------------------------------------------------------------------------*/
#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_max30102(int argc, char **argv)
{
    (void)argc; (void)argv;
    max30102_result_t r;
    max30102_get_result(&r);
    rt_kprintf("[MAX30102] HR=%d bpm (valid=%d)  SpO2=%d%% (valid=%d)  tick=%u\n",
               r.heart_rate, r.hr_valid,
               r.spo2,       r.spo2_valid,
               r.timestamp);
}
MSH_CMD_EXPORT_ALIAS(cmd_max30102, max30102, read MAX30102 heart rate and SpO2);

/**
 * max30102_raw — diagnostic command
 * Reads 20 consecutive raw IR+Red samples directly from FIFO and prints them.
 * Use this to check signal quality BEFORE the algorithm runs:
 *
 *   Good signal (finger properly placed):
 *     IR values should be large (50000-200000) and vary smoothly ±1000-5000
 *     between samples.
 *
 *   Bad signal (no finger / bad contact / ambient light):
 *     IR values near 0, or jumping wildly (e.g. 0, 262143, 0, 262143).
 */
static void cmd_max30102_raw(int argc, char **argv)
{
    (void)argc; (void)argv;
    rt_uint32_t red, ir;
    rt_uint8_t  i;

    rt_kprintf("[MAX30102 RAW] Reading 20 FIFO samples (place finger now)...\n");
    for (i = 0; i < 20; i++)
    {
        /* Wait up to 200 ms for one sample */
        rt_uint8_t retry = 40;
        while (_fifo_available() == 0 && retry--)
            rt_thread_mdelay(5);

        if (_fifo_available() == 0)
        {
            rt_kprintf("  [%2d] TIMEOUT — no data in FIFO\n", i);
            continue;
        }
        _read_fifo_sample(&red, &ir);
        rt_kprintf("  [%2d] IR=%6u  Red=%6u\n", i, ir, red);
    }
    rt_kprintf("[MAX30102 RAW] Done.\n");
    rt_kprintf("  Expected: IR > 50000, values vary smoothly ±few thousand\n");
    rt_kprintf("  If IR < 10000 or jumping 0<->262143: bad contact or no finger\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_max30102_raw, max30102_raw, print 20 raw FIFO samples for signal diagnosis);

#endif /* RT_USING_FINSH */
