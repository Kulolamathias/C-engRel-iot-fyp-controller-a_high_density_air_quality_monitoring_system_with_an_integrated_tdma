/**
 * @file lcd_i2c_priv.h
 * @brief Private definitions for I2C LCD driver
 * @note Internal use only - not part of public API
 */

#ifndef LCD_I2C_PRIV_H
#define LCD_I2C_PRIV_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "driver/i2c.h"

/**
 * @brief LCD internal commands
 */
typedef enum {
    LCD_CMD_CLEAR        = 0x01,  /**< Clear display */
    LCD_CMD_HOME         = 0x02,  /**< Return home */
    LCD_CMD_ENTRY_MODE   = 0x04,  /**< Entry mode set */
    LCD_CMD_DISPLAY_CTRL = 0x08,  /**< Display control */
    LCD_CMD_SHIFT        = 0x10,  /**< Cursor/display shift */
    LCD_CMD_FUNCTION     = 0x20,  /**< Function set */
    LCD_CMD_CGRAM_ADDR   = 0x40,  /**< Set CGRAM address */
    LCD_CMD_DDRAM_ADDR   = 0x80   /**< Set DDRAM address */
} lcd_internal_cmd_t;

/**
 * @brief Function set flags
 */
typedef enum {
    LCD_FUNC_4BIT        = 0x00,  /**< 4-bit interface */
    LCD_FUNC_2LINE       = 0x08,  /**< 2-line display */
    LCD_FUNC_5X8DOTS     = 0x00,  /**< 5x8 dot characters */
    LCD_FUNC_5X10DOTS    = 0x04   /**< 5x10 dot characters */
} lcd_function_flags_t;

/**
 * @brief I2C control bits
 */
typedef enum {
    LCD_I2C_RS        = 0x01,  /**< Register Select (1=data, 0=instruction) */
    LCD_I2C_RW        = 0x02,  /**< Read/Write (1=read, 0=write) */
    LCD_I2C_EN        = 0x04,  /**< Enable pulse */
    LCD_I2C_BACKLIGHT = 0x08   /**< Backlight control */
} lcd_i2c_bits_t;

#endif /* LCD_I2C_PRIV_H */