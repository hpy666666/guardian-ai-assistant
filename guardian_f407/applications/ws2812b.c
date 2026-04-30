/*
 * WS2812B LED Strip Driver
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   PB1 -> TIM3_CH4 -> WS2812B Data In
 *
 * TIM3 clock source: APB1 * 2 = 84MHz (when HCLK=168MHz, APB1 prescaler=4)
 * PWM period: 105 ticks -> 800kHz
 *   T0H = 35 (~417ns), T1H = 70 (~833ns)
 *
 * DMA: DMA1_Stream2, Channel5 (TIM3_CH4)
 *   - No conflict with SDIO (which uses DMA2_Stream3/6)
 *
 * Change Logs:
 * Date         Notes
 * 2026-04-03   first version
 */

#include "ws2812b.h"
#include <rtthread.h>
#include <rtdevice.h>
#include "stm32f4xx_hal.h"

#define DBG_TAG "ws2812b"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Timing constants (TIM3 @ 84MHz, period=105 => 800kHz)
 *---------------------------------------------------------------------------*/
#define WS2812B_TIM_PERIOD      104     /* Auto-reload: period = 105 ticks  */
#define WS2812B_T0H             35      /* 0-bit high time (~417ns)         */
#define WS2812B_T1H             70      /* 1-bit high time (~833ns)         */
#define WS2812B_RESET_SLOTS     50      /* Reset: 50 * 1.25us = 62.5us      */

/* Each LED = 24 bits. Buffer = all bits + reset slots */
#define WS2812B_BITS_PER_LED    24
#define WS2812B_BUF_SIZE        (WS2812B_NUM_LEDS * WS2812B_BITS_PER_LED + WS2812B_RESET_SLOTS)

/*---------------------------------------------------------------------------
 * Internal state
 *---------------------------------------------------------------------------*/
static TIM_HandleTypeDef  s_htim3;
static DMA_HandleTypeDef  s_hdma_tim3_ch4;

/* Color buffer: 24-bit GRB per LED (WS2812B uses GRB order!) */
static rt_uint32_t        s_color_buf[WS2812B_NUM_LEDS];

/* DMA pulse-width buffer */
static rt_uint16_t        s_dma_buf[WS2812B_BUF_SIZE];

/* Brightness scale (0-255, default 128 = ~50%) */
static rt_uint8_t         s_brightness = 128;

/* DMA transfer complete flag */
static volatile rt_bool_t s_dma_done = RT_TRUE;

/* Semaphore for DMA completion */
static struct rt_semaphore s_dma_sem;
static rt_bool_t           s_sem_inited = RT_FALSE;

/*---------------------------------------------------------------------------
 * Internal helpers
 *---------------------------------------------------------------------------*/

/**
 * @brief  Apply brightness scaling to a color component
 */
static rt_uint8_t scale_brightness(rt_uint8_t val)
{
    return (rt_uint8_t)(((rt_uint16_t)val * s_brightness) >> 8);
}

/**
 * @brief  Encode one LED's 24-bit GRB color into the DMA buffer
 * @param  buf_offset  starting index in s_dma_buf
 * @param  color       24-bit RGB color (0x00RRGGBB)
 */
static void encode_led(rt_uint16_t buf_offset, rt_uint32_t color)
{
    rt_uint8_t r = scale_brightness((color >> 16) & 0xFF);
    rt_uint8_t g = scale_brightness((color >>  8) & 0xFF);
    rt_uint8_t b = scale_brightness((color      ) & 0xFF);

    /* WS2812B protocol: GRB order, MSB first */
    rt_uint32_t grb = ((rt_uint32_t)g << 16) |
                      ((rt_uint32_t)r <<  8) |
                      ((rt_uint32_t)b      );

    for (int bit = 23; bit >= 0; bit--)
    {
        s_dma_buf[buf_offset + (23 - bit)] =
            (grb & (1UL << bit)) ? WS2812B_T1H : WS2812B_T0H;
    }
}

/**
 * @brief  Rebuild the entire DMA buffer from s_color_buf
 */
static void rebuild_dma_buf(void)
{
    for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
    {
        encode_led(i * WS2812B_BITS_PER_LED, s_color_buf[i]);
    }
    /* Reset slots: PWM compare = 0 (line stays low) */
    rt_uint16_t reset_start = WS2812B_NUM_LEDS * WS2812B_BITS_PER_LED;
    for (rt_uint16_t i = reset_start; i < WS2812B_BUF_SIZE; i++)
    {
        s_dma_buf[i] = 0;
    }
}

/*---------------------------------------------------------------------------
 * DMA / TIM callbacks
 *---------------------------------------------------------------------------*/

/**
 * @brief  DMA transfer complete callback
 *         Stop the timer PWM output and signal waiting thread
 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        HAL_TIM_PWM_Stop_DMA(&s_htim3, TIM_CHANNEL_4);
        s_dma_done = RT_TRUE;
        if (s_sem_inited)
        {
            rt_sem_release(&s_dma_sem);
        }
    }
}

/*---------------------------------------------------------------------------
 * HAL MSP (GPIO + DMA init called by HAL_TIM_PWM_Init)
 *---------------------------------------------------------------------------*/
void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        GPIO_InitTypeDef GPIO_InitStruct = {0};

        /* Enable clocks */
        __HAL_RCC_TIM3_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_DMA1_CLK_ENABLE();

        /* PB1 -> TIM3_CH4, AF2 */
        GPIO_InitStruct.Pin       = GPIO_PIN_1;
        GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull      = GPIO_NOPULL;
        GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* DMA1_Stream2, Channel5 -> TIM3_CH4 */
        s_hdma_tim3_ch4.Instance                 = DMA1_Stream2;
        s_hdma_tim3_ch4.Init.Channel             = DMA_CHANNEL_5;
        s_hdma_tim3_ch4.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        s_hdma_tim3_ch4.Init.PeriphInc           = DMA_PINC_DISABLE;
        s_hdma_tim3_ch4.Init.MemInc              = DMA_MINC_ENABLE;
        s_hdma_tim3_ch4.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        s_hdma_tim3_ch4.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
        s_hdma_tim3_ch4.Init.Mode                = DMA_NORMAL;
        s_hdma_tim3_ch4.Init.Priority            = DMA_PRIORITY_HIGH;
        s_hdma_tim3_ch4.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        HAL_DMA_Init(&s_hdma_tim3_ch4);

        /* Link DMA to TIM3 channel 4 */
        __HAL_LINKDMA(htim, hdma[TIM_DMA_ID_CC4], s_hdma_tim3_ch4);

        /* DMA interrupt for transfer-complete callback */
        HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 7, 0);
        HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
    }
}

void HAL_TIM_PWM_MspDeInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        __HAL_RCC_TIM3_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOB, GPIO_PIN_1);
        HAL_DMA_DeInit(&s_hdma_tim3_ch4);
        HAL_NVIC_DisableIRQ(DMA1_Stream2_IRQn);
    }
}

/* DMA1_Stream2 interrupt handler - must be in a .c file that the linker sees */
void DMA1_Stream2_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_tim3_ch4);
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

rt_err_t ws2812b_init(void)
{
    TIM_OC_InitTypeDef oc_config = {0};

    /* Init completion semaphore */
    rt_sem_init(&s_dma_sem, "ws2812b", 0, RT_IPC_FLAG_FIFO);
    s_sem_inited = RT_TRUE;

    /* Clear color buffer */
    rt_memset(s_color_buf, 0, sizeof(s_color_buf));
    rt_memset(s_dma_buf, 0, sizeof(s_dma_buf));

    /* TIM3 base: 84MHz / 1 = 84MHz, period = 105 => 800kHz */
    s_htim3.Instance               = TIM3;
    s_htim3.Init.Prescaler         = 0;
    s_htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim3.Init.Period            = WS2812B_TIM_PERIOD;
    s_htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_PWM_Init(&s_htim3) != HAL_OK)
    {
        LOG_E("TIM3 PWM init failed");
        return -RT_ERROR;
    }

    /* Channel 4 PWM config */
    oc_config.OCMode       = TIM_OCMODE_PWM1;
    oc_config.Pulse        = 0;
    oc_config.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc_config.OCFastMode   = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&s_htim3, &oc_config, TIM_CHANNEL_4) != HAL_OK)
    {
        LOG_E("TIM3 CH4 config failed");
        return -RT_ERROR;
    }

    LOG_I("WS2812B init OK (PB1=TIM3_CH4, %d LEDs)", WS2812B_NUM_LEDS);

    /* Turn off all LEDs on startup */
    ws2812b_clear();
    ws2812b_show();

    return RT_EOK;
}

void ws2812b_set(rt_uint16_t index, rt_uint32_t color)
{
    if (index < WS2812B_NUM_LEDS)
    {
        s_color_buf[index] = color;
    }
}

void ws2812b_fill(rt_uint32_t color)
{
    for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
    {
        s_color_buf[i] = color;
    }
}

void ws2812b_clear(void)
{
    rt_memset(s_color_buf, 0, sizeof(s_color_buf));
}

void ws2812b_set_brightness(rt_uint8_t brightness)
{
    s_brightness = brightness;
}

void ws2812b_show(void)
{
    /* Wait for any previous transfer to finish */
    if (!s_dma_done)
    {
        rt_sem_take(&s_dma_sem, rt_tick_from_millisecond(10));
    }

    s_dma_done = RT_FALSE;
    rebuild_dma_buf();

    /* Start DMA-driven PWM */
    HAL_TIM_PWM_Start_DMA(&s_htim3, TIM_CHANNEL_4,
                           (uint32_t *)s_dma_buf, WS2812B_BUF_SIZE);

    /* Wait for completion (max 10ms for 30 LEDs, normally ~1.5ms) */
    rt_sem_take(&s_dma_sem, rt_tick_from_millisecond(10));
}

/*---------------------------------------------------------------------------
 * Color utilities (internal)
 *---------------------------------------------------------------------------*/

/** HSV to RGB conversion. h: 0-359, s: 0-255, v: 0-255 */
static rt_uint32_t hsv_to_rgb(rt_uint16_t h, rt_uint8_t s, rt_uint8_t v)
{
    rt_uint8_t r, g, b;
    rt_uint8_t region, remainder, p, q, t;

    if (s == 0)
    {
        return WS2812B_COLOR(v, v, v);
    }

    region    = h / 60;
    remainder = (rt_uint8_t)((h - (region * 60)) * 255 / 60);

    p = (rt_uint8_t)((rt_uint16_t)v * (255 - s) >> 8);
    q = (rt_uint8_t)((rt_uint16_t)v * (255 - ((s * remainder) >> 8)) >> 8);
    t = (rt_uint8_t)((rt_uint16_t)v * (255 - ((s * (255 - remainder)) >> 8)) >> 8);

    switch (region)
    {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }

    return WS2812B_COLOR(r, g, b);
}

/*---------------------------------------------------------------------------
 * Built-in effects
 *---------------------------------------------------------------------------*/

void ws2812b_effect_rainbow(rt_uint32_t speed_ms, rt_uint32_t cycles)
{
    if (speed_ms == 0) speed_ms = 20;
    rt_uint32_t count = 0;

    while (cycles == 0 || count < cycles)
    {
        for (rt_uint16_t offset = 0; offset < 360; offset++)
        {
            for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
            {
                rt_uint16_t hue = (offset + i * 360 / WS2812B_NUM_LEDS) % 360;
                ws2812b_set(i, hsv_to_rgb(hue, 255, 200));
            }
            ws2812b_show();
            rt_thread_mdelay(speed_ms);
        }
        count++;
    }
}

void ws2812b_effect_breathe(rt_uint32_t color, rt_uint32_t cycles)
{
    rt_uint8_t r = (color >> 16) & 0xFF;
    rt_uint8_t g = (color >>  8) & 0xFF;
    rt_uint8_t b = (color      ) & 0xFF;

    rt_uint32_t count = 0;

    while (cycles == 0 || count < cycles)
    {
        /* Fade in */
        for (rt_uint16_t bright = 0; bright <= 255; bright += 3)
        {
            rt_uint8_t rb = (rt_uint8_t)((rt_uint16_t)r * bright >> 8);
            rt_uint8_t gb = (rt_uint8_t)((rt_uint16_t)g * bright >> 8);
            rt_uint8_t bb = (rt_uint8_t)((rt_uint16_t)b * bright >> 8);
            ws2812b_fill(WS2812B_COLOR(rb, gb, bb));
            ws2812b_show();
            rt_thread_mdelay(10);
        }
        /* Fade out */
        for (rt_int16_t bright = 255; bright >= 0; bright -= 3)
        {
            rt_uint8_t rb = (rt_uint8_t)((rt_uint16_t)r * bright >> 8);
            rt_uint8_t gb = (rt_uint8_t)((rt_uint16_t)g * bright >> 8);
            rt_uint8_t bb = (rt_uint8_t)((rt_uint16_t)b * bright >> 8);
            ws2812b_fill(WS2812B_COLOR(rb, gb, bb));
            ws2812b_show();
            rt_thread_mdelay(10);
        }
        rt_thread_mdelay(200);  /* pause at dark */
        count++;
    }
}

void ws2812b_effect_chase(rt_uint32_t color, rt_uint32_t speed_ms, rt_uint32_t cycles)
{
    if (speed_ms == 0) speed_ms = 50;
    rt_uint32_t count = 0;

    while (cycles == 0 || count < cycles)
    {
        for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
        {
            ws2812b_clear();
            /* Head pixel: full brightness */
            ws2812b_set(i, color);
            /* Tail: fading trail (3 pixels) */
            if (i >= 1) ws2812b_set(i - 1, WS2812B_COLOR(
                ((color >> 16) & 0xFF) >> 1,
                ((color >>  8) & 0xFF) >> 1,
                ((color      ) & 0xFF) >> 1));
            if (i >= 2) ws2812b_set(i - 2, WS2812B_COLOR(
                ((color >> 16) & 0xFF) >> 2,
                ((color >>  8) & 0xFF) >> 2,
                ((color      ) & 0xFF) >> 2));
            ws2812b_show();
            rt_thread_mdelay(speed_ms);
        }
        count++;
    }
}

void ws2812b_effect_theater(rt_uint32_t color, rt_uint32_t speed_ms, rt_uint32_t cycles)
{
    if (speed_ms == 0) speed_ms = 100;
    rt_uint32_t count = 0;

    while (cycles == 0 || count < cycles)
    {
        for (rt_uint8_t phase = 0; phase < 3; phase++)
        {
            ws2812b_clear();
            for (rt_uint16_t i = phase; i < WS2812B_NUM_LEDS; i += 3)
            {
                ws2812b_set(i, color);
            }
            ws2812b_show();
            rt_thread_mdelay(speed_ms);
        }
        count++;
    }
}

void ws2812b_effect_wipe(rt_uint32_t color, rt_uint32_t speed_ms)
{
    if (speed_ms == 0) speed_ms = 50;

    for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
    {
        ws2812b_set(i, color);
        ws2812b_show();
        rt_thread_mdelay(speed_ms);
    }
}

void ws2812b_effect_fire(rt_uint32_t duration_ms)
{
    if (duration_ms == 0) duration_ms = 5000;

    static rt_uint8_t heat[WS2812B_NUM_LEDS];
    rt_uint32_t start = rt_tick_get();

    rt_memset(heat, 0, sizeof(heat));

    while (rt_tick_get() - start < rt_tick_from_millisecond(duration_ms))
    {
        /* Step 1: Cool down every cell a little */
        for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
        {
            rt_uint8_t cool = (rt_uint8_t)(rt_tick_get() & 0x07); /* pseudo-random 0-7 */
            heat[i] = (heat[i] > cool + 2) ? heat[i] - cool - 2 : 0;
        }

        /* Step 2: Heat drifts up and diffuses */
        for (rt_int16_t i = WS2812B_NUM_LEDS - 1; i >= 2; i--)
        {
            heat[i] = (heat[i - 1] + heat[i - 2] + heat[i - 2]) / 3;
        }

        /* Step 3: Randomly ignite new sparks near the bottom */
        if ((rt_tick_get() & 0x03) == 0)
        {
            rt_uint8_t y = rt_tick_get() % (WS2812B_NUM_LEDS / 4);
            heat[y] = (heat[y] + 160 < 255) ? heat[y] + 160 : 255;
        }

        /* Step 4: Map heat to fire colors (black -> red -> yellow -> white) */
        for (rt_uint16_t i = 0; i < WS2812B_NUM_LEDS; i++)
        {
            rt_uint8_t h = heat[i];
            rt_uint8_t r, g, b;
            if (h < 85)
            {
                r = h * 3; g = 0; b = 0;
            }
            else if (h < 170)
            {
                r = 255; g = (h - 85) * 3; b = 0;
            }
            else
            {
                r = 255; g = 255; b = (h - 170) * 3;
            }
            ws2812b_set(i, WS2812B_COLOR(r, g, b));
        }

        ws2812b_show();
        rt_thread_mdelay(30);
    }
}

void ws2812b_effect_chase_startup(rt_uint32_t speed_ms)
{
    if (speed_ms == 0) speed_ms = 40;

    /* Three passes: red -> green -> blue */
    const rt_uint32_t colors[3] = { WS2812B_RED, WS2812B_GREEN, WS2812B_BLUE };
    rt_uint8_t c;

    for (c = 0; c < 3; c++)
    {
        rt_uint32_t color = colors[c];
        rt_uint16_t i;

        for (i = 0; i < WS2812B_NUM_LEDS; i++)
        {
            ws2812b_clear();

            /* Head pixel: full brightness */
            ws2812b_set(i, color);

            /* Tail: 3-pixel fading trail */
            if (i >= 1) ws2812b_set(i - 1, WS2812B_COLOR(
                ((color >> 16) & 0xFF) >> 1,
                ((color >>  8) & 0xFF) >> 1,
                ((color      ) & 0xFF) >> 1));
            if (i >= 2) ws2812b_set(i - 2, WS2812B_COLOR(
                ((color >> 16) & 0xFF) >> 2,
                ((color >>  8) & 0xFF) >> 2,
                ((color      ) & 0xFF) >> 2));

            ws2812b_show();
            rt_thread_mdelay(speed_ms);
        }

        /* Brief pause between colors */
        ws2812b_clear();
        ws2812b_show();
        rt_thread_mdelay(100);
    }
}

/*---------------------------------------------------------------------------
 * MSH test commands
 *---------------------------------------------------------------------------*/
#ifdef RT_USING_FINSH

static int cmd_led_rainbow(int argc, char **argv)
{
    rt_uint32_t speed = 20;
    if (argc > 1) speed = atoi(argv[1]);
    rt_kprintf("Rainbow effect (speed=%ums, 3 cycles). Press reset to stop.\n", speed);
    ws2812b_effect_rainbow(speed, 3);
    ws2812b_clear();
    ws2812b_show();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_rainbow, led_rainbow, WS2812B rainbow [speed_ms]);

static int cmd_led_breathe(int argc, char **argv)
{
    rt_uint32_t color = WS2812B_CYAN;
    if (argc > 1)
    {
        /* Accept color name */
        if      (rt_strcmp(argv[1], "red")     == 0) color = WS2812B_RED;
        else if (rt_strcmp(argv[1], "green")   == 0) color = WS2812B_GREEN;
        else if (rt_strcmp(argv[1], "blue")    == 0) color = WS2812B_BLUE;
        else if (rt_strcmp(argv[1], "cyan")    == 0) color = WS2812B_CYAN;
        else if (rt_strcmp(argv[1], "white")   == 0) color = WS2812B_WHITE;
        else if (rt_strcmp(argv[1], "yellow")  == 0) color = WS2812B_YELLOW;
        else if (rt_strcmp(argv[1], "magenta") == 0) color = WS2812B_MAGENTA;
        else if (rt_strcmp(argv[1], "orange")  == 0) color = WS2812B_ORANGE;
    }
    rt_kprintf("Breathe effect (3 cycles).\n");
    ws2812b_effect_breathe(color, 3);
    ws2812b_clear();
    ws2812b_show();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_breathe, led_breathe, WS2812B breathe [color]);

static int cmd_led_chase(int argc, char **argv)
{
    rt_kprintf("Chase effect (2 rounds).\n");
    ws2812b_effect_chase(WS2812B_COLOR(0, 180, 255), 40, 2);
    ws2812b_clear();
    ws2812b_show();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_chase, led_chase, WS2812B chase effect);

static int cmd_led_theater(int argc, char **argv)
{
    rt_kprintf("Theater chase effect (5 rounds).\n");
    ws2812b_effect_theater(WS2812B_COLOR(255, 120, 0), 80, 5);
    ws2812b_clear();
    ws2812b_show();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_theater, led_theater, WS2812B theater chase);

static int cmd_led_fire(int argc, char **argv)
{
    rt_kprintf("Fire effect (5 seconds).\n");
    ws2812b_effect_fire(5000);
    ws2812b_clear();
    ws2812b_show();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_fire, led_fire, WS2812B fire simulation);

static int cmd_led_color(int argc, char **argv)
{
    if (argc < 4)
    {
        rt_kprintf("Usage: led_color <R> <G> <B>\n");
        return -1;
    }
    rt_uint8_t r = atoi(argv[1]);
    rt_uint8_t g = atoi(argv[2]);
    rt_uint8_t b = atoi(argv[3]);
    ws2812b_fill(WS2812B_COLOR(r, g, b));
    ws2812b_show();
    rt_kprintf("All LEDs set to R=%d G=%d B=%d\n", r, g, b);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_color, led_color, Set all LEDs: led_color <R> <G> <B>);

static int cmd_led_off(int argc, char **argv)
{
    ws2812b_clear();
    ws2812b_show();
    rt_kprintf("LEDs off.\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_off, led_off, Turn off all WS2812B LEDs);

static int cmd_led_bright(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: led_bright <0-255>\n");
        return -1;
    }
    rt_uint8_t b = atoi(argv[1]);
    ws2812b_set_brightness(b);
    rt_kprintf("Brightness set to %d\n", b);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_led_bright, led_bright, Set brightness: led_bright <0-255>);

#endif /* RT_USING_FINSH */
