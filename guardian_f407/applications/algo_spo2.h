/*
 * Heart Rate & SpO2 Algorithm
 * Ported from Maxim Integrated / MBED reference implementation.
 * Original author: Maxim Integrated (app note AN6409)
 * Adaptation for RT-Thread / STM32F407: Guardian project 2026-03-17
 *
 * Algorithm summary:
 *   1. Remove DC baseline from IR signal.
 *   2. Apply 4-point moving average + derivative.
 *   3. Hamming-window the derivative and find peaks (valley in raw = peak
 *      after flip) → inter-peak interval → heart rate.
 *   4. Around each IR valley, locate AC/DC components of both IR and Red.
 *   5. ratio = (Red_AC / Red_DC) / (IR_AC / IR_DC)
 *   6. SpO2 = lookup table uch_spo2_table[ratio] (pre-computed from the
 *      Beer-Lambert equation).
 */

#ifndef __ALGO_SPO2_H__
#define __ALGO_SPO2_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Constants — do NOT change unless you change the algorithm internals
 *---------------------------------------------------------------------------*/
#define ALGO_BUFFER_SIZE    500   /* must match MAX30102_BUFFER_SIZE */
#define ALGO_MA4_SIZE       4     /* moving-average window, fixed */
#define ALGO_HAMMING_SIZE   5     /* Hamming window length, fixed */
#define ALGO_HR_FIFO_SIZE   7

/*---------------------------------------------------------------------------
 * Main algorithm entry point
 *
 * @param pun_ir_buffer      Pointer to 500-sample IR raw data array (uint32)
 * @param n_ir_buffer_length Number of valid samples (should be 500)
 * @param pun_red_buffer     Pointer to 500-sample Red raw data array (uint32)
 * @param pn_spo2            [OUT] SpO2 percentage, or -999 if invalid
 * @param pch_spo2_valid     [OUT] 1 if SpO2 is valid
 * @param pn_heart_rate      [OUT] Heart rate in BPM, or -999 if invalid
 * @param pch_hr_valid       [OUT] 1 if heart rate is valid
 *---------------------------------------------------------------------------*/
void algo_heart_rate_and_spo2(
    rt_uint32_t *pun_ir_buffer,
    rt_int32_t   n_ir_buffer_length,
    rt_uint32_t *pun_red_buffer,
    rt_int32_t  *pn_spo2,
    rt_int8_t   *pch_spo2_valid,
    rt_int32_t  *pn_heart_rate,
    rt_int8_t   *pch_hr_valid);

/* Internal helpers — exposed only for unit testing */
void algo_find_peaks(
    rt_int32_t *pn_locs,
    rt_int32_t *pn_npks,
    rt_int32_t *pn_x,
    rt_int32_t  n_size,
    rt_int32_t  n_min_height,
    rt_int32_t  n_min_distance,
    rt_int32_t  n_max_num);

#ifdef __cplusplus
}
#endif

#endif /* __ALGO_SPO2_H__ */
