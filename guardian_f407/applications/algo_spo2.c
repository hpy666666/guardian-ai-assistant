/*
 * Heart Rate & SpO2 Algorithm Implementation
 * Ported from Maxim Integrated reference (AN6409).
 * All integer arithmetic — no float, safe on Cortex-M4 without FPU enabled.
 *
 * Key fixes vs. the vendor example:
 *   - No hard-coded "dis_hr - 20" correction.
 *   - Proper uint32_t types throughout (no implicit int sign extension).
 *   - Static scratch buffers prevent stack overflow in threaded context.
 */

#include "algo_spo2.h"
#include <rtthread.h>
#include <string.h>

/*---------------------------------------------------------------------------
 * Hamming window coefficients (5-tap, Q9 fixed-point, sum = 1146)
 * hamming(5) * 512, rounded to integers
 *---------------------------------------------------------------------------*/
static const rt_int32_t s_hamm[ALGO_HAMMING_SIZE] = { 41, 276, 512, 276, 41 };

/*---------------------------------------------------------------------------
 * SpO2 lookup table
 * Index = ratio*10, value = SpO2 %
 * Pre-computed from: spo2 = -45.060*r^2 + 30.354*r + 94.845
 * where r = (Red_AC/Red_DC) / (IR_AC/IR_DC) scaled to integer index
 *---------------------------------------------------------------------------*/
static const rt_uint8_t s_spo2_table[184] = {
     95,  95,  95,  96,  96,  96,  97,  97,  97,  97,
     97,  98,  98,  98,  98,  98,  99,  99,  99,  99,
     99,  99,  99,  99, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100,  99,  99,  99,  99,  99,  99,
     99,  99,  98,  98,  98,  98,  98,  98,  97,  97,
     97,  97,  96,  96,  96,  96,  95,  95,  95,  94,
     94,  94,  93,  93,  93,  92,  92,  92,  91,  91,
     90,  90,  89,  89,  89,  88,  88,  87,  87,  86,
     86,  85,  85,  84,  84,  83,  82,  82,  81,  81,
     80,  80,  79,  78,  78,  77,  76,  76,  75,  74,
     74,  73,  72,  72,  71,  70,  69,  69,  68,  67,
     66,  66,  65,  64,  63,  62,  62,  61,  60,  59,
     58,  57,  56,  56,  55,  54,  53,  52,  51,  50,
     49,  48,  47,  46,  45,  44,  43,  42,  41,  40,
     39,  38,  37,  36,  35,  34,  33,  31,  30,  29,
     28,  27,  26,  25,  23,  22,  21,  20,  19,  17,
     16,  15,  14,  12,  11,  10,   9,   7,   6,   5,
      3,   2,   1
};

/*---------------------------------------------------------------------------
 * Static scratch buffers — avoids large stack allocation in the thread
 *---------------------------------------------------------------------------*/
static rt_int32_t s_an_x[ALGO_BUFFER_SIZE];
static rt_int32_t s_an_y[ALGO_BUFFER_SIZE];
static rt_int32_t s_an_dx[ALGO_BUFFER_SIZE - ALGO_MA4_SIZE];

/*---------------------------------------------------------------------------
 * Internal helpers
 *---------------------------------------------------------------------------*/
static void _sort_ascend(rt_int32_t *pn_x, rt_int32_t n_size)
{
    rt_int32_t i, j, tmp;
    for (i = 1; i < n_size; i++)
    {
        tmp = pn_x[i];
        for (j = i; j > 0 && tmp < pn_x[j - 1]; j--)
            pn_x[j] = pn_x[j - 1];
        pn_x[j] = tmp;
    }
}

static void _sort_indices_descend(rt_int32_t *pn_x, rt_int32_t *pn_indx, rt_int32_t n_size)
{
    rt_int32_t i, j, tmp;
    for (i = 1; i < n_size; i++)
    {
        tmp = pn_indx[i];
        for (j = i; j > 0 && pn_x[tmp] > pn_x[pn_indx[j - 1]]; j--)
            pn_indx[j] = pn_indx[j - 1];
        pn_indx[j] = tmp;
    }
}

static void _peaks_above_min_height(
    rt_int32_t *pn_locs, rt_int32_t *pn_npks,
    rt_int32_t *pn_x, rt_int32_t n_size, rt_int32_t n_min_height)
{
    rt_int32_t i = 1, n_width;
    *pn_npks = 0;
    while (i < n_size - 1)
    {
        if (pn_x[i] > n_min_height && pn_x[i] > pn_x[i - 1])
        {
            n_width = 1;
            while (i + n_width < n_size && pn_x[i] == pn_x[i + n_width])
                n_width++;
            if (pn_x[i] > pn_x[i + n_width] && (*pn_npks) < 15)
            {
                pn_locs[(*pn_npks)++] = i;
                i += n_width + 1;
            }
            else
                i += n_width;
        }
        else
            i++;
    }
}

static void _remove_close_peaks(
    rt_int32_t *pn_locs, rt_int32_t *pn_npks,
    rt_int32_t *pn_x, rt_int32_t n_min_distance)
{
    rt_int32_t i, j, n_old_npks, n_dist;
    _sort_indices_descend(pn_x, pn_locs, *pn_npks);
    for (i = -1; i < *pn_npks; i++)
    {
        n_old_npks = *pn_npks;
        *pn_npks   = i + 1;
        for (j = i + 1; j < n_old_npks; j++)
        {
            n_dist = pn_locs[j] - (i == -1 ? -1 : pn_locs[i]);
            if (n_dist > n_min_distance || n_dist < -n_min_distance)
                pn_locs[(*pn_npks)++] = pn_locs[j];
        }
    }
    _sort_ascend(pn_locs, *pn_npks);
}

void algo_find_peaks(
    rt_int32_t *pn_locs, rt_int32_t *pn_npks,
    rt_int32_t *pn_x, rt_int32_t n_size,
    rt_int32_t n_min_height, rt_int32_t n_min_distance, rt_int32_t n_max_num)
{
    _peaks_above_min_height(pn_locs, pn_npks, pn_x, n_size, n_min_height);
    _remove_close_peaks(pn_locs, pn_npks, pn_x, n_min_distance);
    if (*pn_npks > n_max_num)
        *pn_npks = n_max_num;
}

/*---------------------------------------------------------------------------
 * Main algorithm
 *---------------------------------------------------------------------------*/
void algo_heart_rate_and_spo2(
    rt_uint32_t *pun_ir_buffer,
    rt_int32_t   n_ir_buffer_length,
    rt_uint32_t *pun_red_buffer,
    rt_int32_t  *pn_spo2,
    rt_int8_t   *pch_spo2_valid,
    rt_int32_t  *pn_heart_rate,
    rt_int8_t   *pch_hr_valid)
{
    rt_uint32_t un_ir_mean;
    rt_int32_t  k, n_i_ratio_count;
    rt_int32_t  i, s, m, n_exact_ir_valley_locs_count, n_middle_idx;
    rt_int32_t  n_th1, n_npks, n_c_min;
    rt_int32_t  an_ir_valley_locs[15];
    rt_int32_t  an_exact_ir_valley_locs[15];
    rt_int32_t  an_dx_peak_locs[15];
    rt_int32_t  n_peak_interval_sum;
    rt_int32_t  n_y_ac, n_x_ac;
    rt_int32_t  n_spo2_calc;
    rt_int32_t  n_y_dc_max, n_x_dc_max;
    rt_int32_t  n_y_dc_max_idx, n_x_dc_max_idx;
    rt_int32_t  an_ratio[5], n_ratio_average;
    rt_int32_t  n_nume, n_denom;
    rt_uint32_t un_only_once;

    /* ---- step 1: remove IR DC baseline ---------------------------------- */
    un_ir_mean = 0;
    for (k = 0; k < n_ir_buffer_length; k++)
        un_ir_mean += pun_ir_buffer[k];
    un_ir_mean /= (rt_uint32_t)n_ir_buffer_length;

    for (k = 0; k < n_ir_buffer_length; k++)
        s_an_x[k] = (rt_int32_t)pun_ir_buffer[k] - (rt_int32_t)un_ir_mean;

    /* ---- step 2: 4-point moving average --------------------------------- */
    for (k = 0; k < ALGO_BUFFER_SIZE - ALGO_MA4_SIZE; k++)
    {
        n_denom = s_an_x[k] + s_an_x[k+1] + s_an_x[k+2] + s_an_x[k+3];
        s_an_x[k] = n_denom / 4;
    }

    /* ---- step 3: first-difference --------------------------------------- */
    for (k = 0; k < ALGO_BUFFER_SIZE - ALGO_MA4_SIZE - 1; k++)
        s_an_dx[k] = s_an_x[k+1] - s_an_x[k];

    /* ---- step 4: 2-pt moving average of derivative ---------------------- */
    for (k = 0; k < ALGO_BUFFER_SIZE - ALGO_MA4_SIZE - 2; k++)
        s_an_dx[k] = (s_an_dx[k] + s_an_dx[k+1]) / 2;

    /* ---- step 5: Hamming window + flip (valley detection as peak) ------- */
    for (i = 0; i < ALGO_BUFFER_SIZE - ALGO_HAMMING_SIZE - ALGO_MA4_SIZE - 2; i++)
    {
        s = 0;
        for (k = i; k < i + ALGO_HAMMING_SIZE; k++)
            s -= s_an_dx[k] * s_hamm[k - i];
        s_an_dx[i] = s / 1146;
    }

    /* ---- step 6: threshold and peak detect ------------------------------ */
    n_th1 = 0;
    for (k = 0; k < ALGO_BUFFER_SIZE - ALGO_HAMMING_SIZE; k++)
        n_th1 += (s_an_dx[k] > 0) ? s_an_dx[k] : -s_an_dx[k];
    n_th1 /= (ALGO_BUFFER_SIZE - ALGO_HAMMING_SIZE);

    algo_find_peaks(an_dx_peak_locs, &n_npks,
                    s_an_dx, ALGO_BUFFER_SIZE - ALGO_HAMMING_SIZE,
                    n_th1, 8, 5);

    /* ---- step 7: heart rate from peak intervals ------------------------- */
    n_peak_interval_sum = 0;
    if (n_npks >= 2)
    {
        for (k = 1; k < n_npks; k++)
            n_peak_interval_sum += (an_dx_peak_locs[k] - an_dx_peak_locs[k - 1]);
        n_peak_interval_sum /= (n_npks - 1);
        /* HR(bpm) = 60s * sample_rate / samples_per_beat
         *         = 6000 / interval  (at 100 sps) */
        *pn_heart_rate = 6000 / n_peak_interval_sum;
        *pch_hr_valid  = 1;
    }
    else
    {
        *pn_heart_rate = -999;
        *pch_hr_valid  = 0;
    }

    /* map dx peak locations back to IR valley locations */
    for (k = 0; k < n_npks; k++)
        an_ir_valley_locs[k] = an_dx_peak_locs[k] + ALGO_HAMMING_SIZE / 2;

    /* ---- step 8: SpO2 — find precise valleys ---------------------------- */
    for (k = 0; k < n_ir_buffer_length; k++)
    {
        s_an_x[k] = (rt_int32_t)pun_ir_buffer[k];
        s_an_y[k] = (rt_int32_t)pun_red_buffer[k];
    }

    n_exact_ir_valley_locs_count = 0;
    for (k = 0; k < n_npks; k++)
    {
        un_only_once = 1;
        m = an_ir_valley_locs[k];
        n_c_min = 16777216; /* 2^24 */
        if (m + 5 < ALGO_BUFFER_SIZE - ALGO_HAMMING_SIZE && m - 5 > 0)
        {
            for (i = m - 5; i < m + 5; i++)
            {
                if (s_an_x[i] < n_c_min)
                {
                    if (un_only_once) un_only_once = 0;
                    n_c_min = s_an_x[i];
                    an_exact_ir_valley_locs[k] = i;
                }
            }
            if (!un_only_once)
                n_exact_ir_valley_locs_count++;
        }
    }

    if (n_exact_ir_valley_locs_count < 2)
    {
        *pn_spo2       = -999;
        *pch_spo2_valid = 0;
        return;
    }

    /* ---- step 9: 4-pt MA on raw IR and Red ------------------------------ */
    for (k = 0; k < ALGO_BUFFER_SIZE - ALGO_MA4_SIZE; k++)
    {
        s_an_x[k] = (s_an_x[k] + s_an_x[k+1] + s_an_x[k+2] + s_an_x[k+3]) / 4;
        s_an_y[k] = (s_an_y[k] + s_an_y[k+1] + s_an_y[k+2] + s_an_y[k+3]) / 4;
    }

    /* ---- step 10: compute AC/DC ratio between each pair of valleys ------ */
    n_ratio_average  = 0;
    n_i_ratio_count  = 0;
    for (k = 0; k < 5; k++) an_ratio[k] = 0;

    for (k = 0; k < n_exact_ir_valley_locs_count; k++)
    {
        if (an_exact_ir_valley_locs[k] > ALGO_BUFFER_SIZE)
        {
            *pn_spo2       = -999;
            *pch_spo2_valid = 0;
            return;
        }
    }

    for (k = 0; k < n_exact_ir_valley_locs_count - 1; k++)
    {
        n_y_dc_max = -16777216;
        n_x_dc_max = -16777216;

        if (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k] > 10)
        {
            for (i = an_exact_ir_valley_locs[k]; i < an_exact_ir_valley_locs[k+1]; i++)
            {
                if (s_an_x[i] > n_x_dc_max) { n_x_dc_max = s_an_x[i]; n_x_dc_max_idx = i; }
                if (s_an_y[i] > n_y_dc_max) { n_y_dc_max = s_an_y[i]; n_y_dc_max_idx = i; }
            }

            /* Red AC (linear-detrended) */
            n_y_ac = (s_an_y[an_exact_ir_valley_locs[k+1]] - s_an_y[an_exact_ir_valley_locs[k]])
                     * (n_y_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_y_ac = s_an_y[an_exact_ir_valley_locs[k]]
                     + n_y_ac / (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k]);
            n_y_ac = s_an_y[n_y_dc_max_idx] - n_y_ac;

            /* IR AC (linear-detrended) */
            n_x_ac = (s_an_x[an_exact_ir_valley_locs[k+1]] - s_an_x[an_exact_ir_valley_locs[k]])
                     * (n_x_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_x_ac = s_an_x[an_exact_ir_valley_locs[k]]
                     + n_x_ac / (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k]);
            n_x_ac = s_an_x[n_y_dc_max_idx] - n_x_ac;

            /* ratio = (Red_AC * IR_DC) / (IR_AC * Red_DC)  scaled *20 */
            n_nume  = (n_y_ac * n_x_dc_max) >> 7;
            n_denom = (n_x_ac * n_y_dc_max) >> 7;

            if (n_denom > 0 && n_i_ratio_count < 5 && n_nume != 0)
            {
                an_ratio[n_i_ratio_count++] = (n_nume * 20) / n_denom;
            }
        }
    }

    /* median of ratios */
    _sort_ascend(an_ratio, n_i_ratio_count);
    n_middle_idx = n_i_ratio_count / 2;
    if (n_middle_idx > 1)
        n_ratio_average = (an_ratio[n_middle_idx - 1] + an_ratio[n_middle_idx]) / 2;
    else
        n_ratio_average = an_ratio[n_middle_idx];

    /* table lookup */
    if (n_ratio_average > 2 && n_ratio_average < 184)
    {
        n_spo2_calc = (rt_int32_t)s_spo2_table[n_ratio_average];
        *pn_spo2       = n_spo2_calc;
        *pch_spo2_valid = 1;
    }
    else
    {
        *pn_spo2       = -999;
        *pch_spo2_valid = 0;
    }
}
