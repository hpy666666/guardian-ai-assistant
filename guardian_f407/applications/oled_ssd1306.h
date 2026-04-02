/*
 * OLED SSD1306 Driver (128x64, I2C)
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   OLED VCC -> 3.3V
 *   OLED GND -> GND
 *   OLED SCL -> PB8 (I2C1_SCL)
 *   OLED SDA -> PB9 (I2C1_SDA)
 *
 * I2C Address: 0x3C (most common 4-pin modules)
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-21   first version for Guardian project
 */

#ifndef __OLED_SSD1306_H__
#define __OLED_SSD1306_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Hardware definitions
 *---------------------------------------------------------------------------*/
#define OLED_I2C_BUS_NAME   "i2c1"
#define OLED_I2C_ADDR       0x3C    /* 7-bit address */

#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_PAGES          (OLED_HEIGHT / 8)   /* 8 pages */

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

/**
 * @brief  Initialize OLED display
 * @return RT_EOK on success, negative on error
 */
rt_err_t oled_init(void);

/**
 * @brief  Clear the entire display (fill with black)
 */
void oled_clear(void);

/**
 * @brief  Fill the entire display (fill with white)
 */
void oled_fill(void);

/**
 * @brief  Update display from frame buffer
 *         Call this after drawing to actually show content
 */
void oled_refresh(void);

/**
 * @brief  Set a single pixel in frame buffer
 * @param  x     X coordinate (0-127)
 * @param  y     Y coordinate (0-63)
 * @param  color 1=white, 0=black
 */
void oled_draw_pixel(rt_uint8_t x, rt_uint8_t y, rt_uint8_t color);

/**
 * @brief  Draw a character at specified position
 * @param  x     X coordinate (column, 0-127)
 * @param  y     Y coordinate (row in pixels, 0-63)
 * @param  ch    ASCII character
 * @param  size  Font size: 8 (6x8) or 16 (8x16), other values default to 6x8
 */
void oled_draw_char(rt_uint8_t x, rt_uint8_t y, char ch, rt_uint8_t size);

/**
 * @brief  Draw a string at specified position
 * @param  x     X coordinate
 * @param  y     Y coordinate
 * @param  str   Null-terminated string
 * @param  size  Font size: 8 (6x8) or 16 (8x16)
 */
void oled_draw_string(rt_uint8_t x, rt_uint8_t y, const char *str, rt_uint8_t size);

/**
 * @brief  Draw a horizontal line
 * @param  x     Start X coordinate
 * @param  y     Y coordinate
 * @param  len   Length in pixels
 */
void oled_draw_hline(rt_uint8_t x, rt_uint8_t y, rt_uint8_t len);

/**
 * @brief  Draw a rectangle (outline only)
 * @param  x1,y1 Top-left corner
 * @param  x2,y2 Bottom-right corner
 */
void oled_draw_rect(rt_uint8_t x1, rt_uint8_t y1, rt_uint8_t x2, rt_uint8_t y2);

/**
 * @brief  Set display on/off
 * @param  on  1=display on, 0=display off (low power)
 */
void oled_display_on(rt_uint8_t on);

/**
 * @brief  Set display contrast
 * @param  contrast 0-255
 */
void oled_set_contrast(rt_uint8_t contrast);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_SSD1306_H__ */
