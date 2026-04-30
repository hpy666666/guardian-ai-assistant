/*
 * WS2812B LED Strip Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   PB1 -> TIM3_CH4 -> WS2812B Data In
 *   (Red=5V, White=GND, Green=Data)
 *
 * Method: TIM3 PWM + DMA2 (avoid conflict with SDIO on DMA2_Stream3/6)
 *   Actually uses DMA1_Stream2 (TIM3_CH4) - no conflict with SDIO DMA2
 *
 * Timing (800kHz, period=105 @ 84MHz TIM3 clock):
 *   T0H = 35 counts (~417ns),  T0L = 70 counts (~833ns)
 *   T1H = 70 counts (~833ns),  T1L = 35 counts (~417ns)
 *   Reset = 50us low (hold DMA buffer at 0)
 *
 * Change Logs:
 * Date         Notes
 * 2026-04-03   first version, TIM3_CH4 + DMA1_Stream2
 */

#ifndef __WS2812B_H__
#define __WS2812B_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Configuration
 *---------------------------------------------------------------------------*/
#define WS2812B_NUM_LEDS        30      /* Number of LEDs in the strip */

/*---------------------------------------------------------------------------
 * Color helper macros
 *---------------------------------------------------------------------------*/
#define WS2812B_COLOR(r, g, b)  (((rt_uint32_t)(r) << 16) | \
                                  ((rt_uint32_t)(g) <<  8) | \
                                  ((rt_uint32_t)(b)))

#define WS2812B_RED             WS2812B_COLOR(255,   0,   0)
#define WS2812B_GREEN           WS2812B_COLOR(  0, 255,   0)
#define WS2812B_BLUE            WS2812B_COLOR(  0,   0, 255)
#define WS2812B_WHITE           WS2812B_COLOR(255, 255, 255)
#define WS2812B_YELLOW          WS2812B_COLOR(255, 255,   0)
#define WS2812B_CYAN            WS2812B_COLOR(  0, 255, 255)
#define WS2812B_MAGENTA         WS2812B_COLOR(255,   0, 255)
#define WS2812B_OFF             WS2812B_COLOR(  0,   0,   0)
#define WS2812B_ORANGE          WS2812B_COLOR(255, 128,   0)
#define WS2812B_WARM_WHITE      WS2812B_COLOR(255, 200, 100)

/*---------------------------------------------------------------------------
 * API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize WS2812B driver (TIM3_CH4 + DMA1, PB1)
 * @return RT_EOK on success
 */
rt_err_t ws2812b_init(void);

/**
 * @brief  Set a single LED color (buffered, call ws2812b_show() to apply)
 * @param  index  LED index (0 ~ WS2812B_NUM_LEDS-1)
 * @param  color  24-bit RGB color (use WS2812B_COLOR(r,g,b) macro)
 */
void ws2812b_set(rt_uint16_t index, rt_uint32_t color);

/**
 * @brief  Set all LEDs to the same color (buffered)
 */
void ws2812b_fill(rt_uint32_t color);

/**
 * @brief  Clear all LEDs (turn off)
 */
void ws2812b_clear(void);

/**
 * @brief  Push buffer to hardware via DMA
 *         Blocks until DMA transfer completes (~1.3ms for 30 LEDs)
 */
void ws2812b_show(void);

/**
 * @brief  Set global brightness scale (0-255)
 *         Applied at ws2812b_show() time, does not modify the color buffer
 */
void ws2812b_set_brightness(rt_uint8_t brightness);

/*---------------------------------------------------------------------------
 * Built-in effects (blocking, run in a dedicated thread or call once)
 *---------------------------------------------------------------------------*/

/** @brief  Rainbow cycle across all LEDs, cycles continuously
 *  @param  speed_ms  delay between frames in ms (smaller = faster)
 *  @param  cycles    number of full rainbow cycles (0 = infinite)
 */
void ws2812b_effect_rainbow(rt_uint32_t speed_ms, rt_uint32_t cycles);

/** @brief  Single color breathing effect
 *  @param  color     base color
 *  @param  cycles    number of breath cycles (0 = infinite)
 */
void ws2812b_effect_breathe(rt_uint32_t color, rt_uint32_t cycles);

/** @brief  Running light (single pixel chasing)
 *  @param  color     pixel color
 *  @param  speed_ms  delay between steps
 *  @param  cycles    number of full rounds (0 = infinite)
 */
void ws2812b_effect_chase(rt_uint32_t color, rt_uint32_t speed_ms, rt_uint32_t cycles);

/** @brief  Theater chase (every 3rd LED chases)
 *  @param  color     pixel color
 *  @param  speed_ms  delay between steps
 *  @param  cycles    number of full rounds (0 = infinite)
 */
void ws2812b_effect_theater(rt_uint32_t color, rt_uint32_t speed_ms, rt_uint32_t cycles);

/** @brief  Color wipe (fill LEDs one by one)
 *  @param  color     fill color
 *  @param  speed_ms  delay between each LED
 */
void ws2812b_effect_wipe(rt_uint32_t color, rt_uint32_t speed_ms);

/** @brief  Fire simulation effect
 *  @param  duration_ms  how long to run (0 = 5000ms default)
 */
void ws2812b_effect_fire(rt_uint32_t duration_ms);

/** @brief  Startup RGB chase: red -> green -> blue, one pass each color
 *          Designed to be called once at boot before entering main loop.
 *  @param  speed_ms  delay between steps in ms (default 40 if 0)
 */
void ws2812b_effect_chase_startup(rt_uint32_t speed_ms);

#ifdef __cplusplus
}
#endif

#endif /* __WS2812B_H__ */
