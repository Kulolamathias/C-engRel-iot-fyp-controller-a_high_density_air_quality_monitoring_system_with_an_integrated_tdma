/**
 * @file lcd_i2c.h
 * @brief I2C LCD Driver for ESP-IDF (Supports 1602 and 2004 LCDs)
 * @author Mathias Kulola
 * @date 2024
 * @version 1.1.0
 * 
 * @copyright MIT License
 * 
 * This driver provides thread-safe operations for HD44780-based LCD displays
 * connected via I2C interface. It supports both 16x2 and 20x4 LCDs.
 */

#ifndef LCD_I2C_H
#define LCD_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "driver/i2c.h"

/**
 * @brief LCD handle (opaque type)
 */
typedef struct lcd_handle_t lcd_handle_t;

/**
 * @brief LCD error codes
 */
typedef enum {
    LCD_OK = 0,              /**< Operation successful */
    LCD_ERR_INVALID_ARG,     /**< Invalid argument */
    LCD_ERR_I2C,             /**< I2C communication error */
    LCD_ERR_TIMEOUT,         /**< Operation timeout */
    LCD_ERR_MUTEX,           /**< Mutex error */
    LCD_ERR_MEMORY,          /**< Memory allocation error */
    LCD_ERR_NOT_INIT,         /**< LCD not initialized */
    LCD_ERR_FORMAT           /**< Format string error */
} lcd_error_t;

/**
 * @brief Backlight control states
 */
typedef enum {
    LCD_BACKLIGHT_OFF = 0,   /**< Backlight off */
    LCD_BACKLIGHT_ON = 1     /**< Backlight on */
} lcd_backlight_t;

/**
 * @brief Scroll directions
 */
typedef enum {
    LCD_SCROLL_LEFT = 0,     /**< Scroll left */
    LCD_SCROLL_RIGHT = 1     /**< Scroll right */
} lcd_scroll_dir_t;

/**
 * @brief Entry mode directions
 */
typedef enum {
    LCD_ENTRY_RIGHT = 0x00,  /**< Text flows right */
    LCD_ENTRY_LEFT = 0x02    /**< Text flows left */
} lcd_entry_mode_t;

/**
 * @brief Display control flags
 */
typedef enum {
    LCD_DISPLAY_OFF = 0x00,  /**< Display off */
    LCD_DISPLAY_ON  = 0x04,  /**< Display on */
    LCD_CURSOR_OFF  = 0x00,  /**< Cursor off */
    LCD_CURSOR_ON   = 0x02,  /**< Cursor on */
    LCD_BLINK_OFF   = 0x00,  /**< Cursor blink off */
    LCD_BLINK_ON    = 0x01   /**< Cursor blink on */
} lcd_display_ctrl_t;

/**
 * @brief Print format types for lcd_print_mixed
 */
typedef enum {
    LCD_PRINT_STRING,        /**< Print string */
    LCD_PRINT_INT,           /**< Print integer */
    LCD_PRINT_HEX,           /**< Print hexadecimal */
    LCD_PRINT_FLOAT,         /**< Print float */
    LCD_PRINT_CHAR,          /**< Print single character */
    LCD_PRINT_CUSTOM_CHAR    /**< Print custom character (0-7) */
} lcd_print_type_t;

/**
 * @brief Print data union for mixed printing
 */
typedef union {
    const char* str;         /**< String pointer */
    int32_t integer;         /**< Integer value */
    uint32_t hex;            /**< Hexadecimal value */
    float floating;          /**< Float value */
    char character;          /**< Single character */
    uint8_t custom_char;     /**< Custom character index (0-7) */
} lcd_print_data_t;

/**
 * @brief Print format item for mixed printing
 */
typedef struct {
    lcd_print_type_t type;   /**< Type of data to print */
    lcd_print_data_t data;   /**< Data to print */
    uint8_t decimals;        /**< Decimal places for floats (optional) */
} lcd_print_item_t;

/**
 * @brief LCD configuration structure
 */
typedef struct {
    i2c_port_t i2c_port;        /**< I2C port number (I2C_NUM_0 or I2C_NUM_1) */
    uint8_t i2c_addr;           /**< I2C device address (typically 0x27 or 0x3F) */
    uint8_t rows;               /**< Number of LCD rows (2 for 1602, 4 for 2004) */
    uint8_t cols;               /**< Number of LCD columns (16 for 1602, 20 for 2004) */
    bool backlight_enable;      /**< Enable backlight control */
    uint32_t i2c_timeout_ms;    /**< I2C operation timeout in milliseconds */
    uint32_t cmd_delay_us;      /**< Command execution delay in microseconds */
} lcd_config_t;

/**
 * @brief Custom character definition (5x8 pixels)
 */
typedef struct {
    uint8_t data[8];            /**< Character pixel data (8 rows of 5 bits) */
} lcd_custom_char_t;

/**
 * @brief Initialize the LCD with given configuration
 * 
 * @param config Pointer to LCD configuration structure
 * @return lcd_handle_t* LCD handle on success, NULL on failure
 * 
 * @note This function must be called before any other LCD operations
 */
lcd_handle_t* lcd_i2c_init(const lcd_config_t* config);

/**
 * @brief Deinitialize LCD and free resources
 * 
 * @param lcd LCD handle
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_i2c_deinit(lcd_handle_t* lcd);

/**
 * @brief Clear the entire LCD display
 * 
 * @param lcd LCD handle
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_clear(lcd_handle_t* lcd);

/**
 * @brief Return cursor to home position (0,0)
 * 
 * @param lcd LCD handle
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_home(lcd_handle_t* lcd);

/**
 * @brief Set cursor position
 * 
 * @param lcd LCD handle
 * @param row Row position (0-indexed)
 * @param col Column position (0-indexed)
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_set_cursor(lcd_handle_t* lcd, uint8_t row, uint8_t col);

/**
 * @brief Control display features
 * 
 * @param lcd LCD handle
 * @param display Enable/disable display
 * @param cursor Show/hide cursor
 * @param blink Enable/disable cursor blink
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_display_control(lcd_handle_t* lcd, bool display, bool cursor, bool blink);

/**
 * @brief Set text entry mode
 * 
 * @param lcd LCD handle
 * @param direction Text direction (left or right)
 * @param shift Enable automatic display shift
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_entry_mode(lcd_handle_t* lcd, lcd_entry_mode_t direction, bool shift);

/**
 * @brief Print string at current cursor position
 * 
 * @param lcd LCD handle
 * @param str Null-terminated string to print
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_print_str(lcd_handle_t* lcd, const char* str);

/**
 * @brief Print formatted string (similar to printf)
 * 
 * @param lcd LCD handle
 * @param format Format string
 * @param ... Variable arguments
 * @return lcd_error_t Operation status
 * 
 * @note Buffer limited to 128 characters
 */
lcd_error_t lcd_printf(lcd_handle_t* lcd, const char* format, ...);

/**
 * @brief Print mixed data types flexibly
 * 
 * @param lcd LCD handle
 * @param items Array of print items
 * @param count Number of items to print
 * @return lcd_error_t Operation status
 * 
 * @example 
 * // Print "Temp: 25.5°C" where ° is a custom character
 * lcd_print_item_t items[] = {
 *     {LCD_PRINT_STRING, .data.str = "Temp: "},
 *     {LCD_PRINT_FLOAT, .data.floating = 25.5, .decimals = 1},
 *     {LCD_PRINT_CUSTOM_CHAR, .data.custom_char = 0}, // degree symbol
 *     {LCD_PRINT_STRING, .data.str = "C"}
 * };
 * lcd_print_mixed(lcd, items, 4);
 */
lcd_error_t lcd_print_mixed(lcd_handle_t* lcd, const lcd_print_item_t* items, uint8_t count);

/**
 * @brief Print integer value
 * 
 * @param lcd LCD handle
 * @param num Integer to print
 * @param base Number base (10 for decimal, 16 for hexadecimal)
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_print_int(lcd_handle_t* lcd, int32_t num, uint8_t base);

/**
 * @brief Print floating-point number
 * 
 * @param lcd LCD handle
 * @param num Floating-point number to print
 * @param decimals Number of decimal places
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_print_float(lcd_handle_t* lcd, float num, uint8_t decimals);

/**
 * @brief Print single character
 * 
 * @param lcd LCD handle
 * @param c Character to print
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_print_char(lcd_handle_t* lcd, char c);

/**
 * @brief Create custom character
 * 
 * @param lcd LCD handle
 * @param location Character location (0-7)
 * @param char_data Character pixel data
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_create_char(lcd_handle_t* lcd, uint8_t location, const lcd_custom_char_t* char_data);

/**
 * @brief Print custom character
 * 
 * @param lcd LCD handle
 * @param location Custom character location (0-7)
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_print_custom_char(lcd_handle_t* lcd, uint8_t location);

/**
 * @brief Scroll display content
 * 
 * @param lcd LCD handle
 * @param direction Scroll direction (left or right)
 * @param count Number of positions to scroll
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_scroll(lcd_handle_t* lcd, lcd_scroll_dir_t direction, uint8_t count);

/**
 * @brief Control LCD backlight
 * 
 * @param lcd LCD handle
 * @param state Backlight state (on or off)
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_backlight(lcd_handle_t* lcd, lcd_backlight_t state);

/**
 * @brief Get LCD dimensions
 * 
 * @param lcd LCD handle
 * @param rows Output: number of rows
 * @param cols Output: number of columns
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_get_dimensions(lcd_handle_t* lcd, uint8_t* rows, uint8_t* cols);

/**
 * @brief Move cursor to beginning of specified row
 * 
 * @param lcd LCD handle
 * @param row Row number (0-indexed)
 * @return lcd_error_t Operation status
 */
lcd_error_t lcd_goto_row(lcd_handle_t* lcd, uint8_t row);

/**
 * @brief Create and print mixed content in one call (convenience function)
 * 
 * @param lcd LCD handle
 * @param format Format string with types: %s=string, %d=int, %x=hex, %f=float, %c=char, %C=custom_char
 * @param ... Arguments corresponding to format specifiers
 * @return lcd_error_t Operation status
 * 
 * @example
 * lcd_print_flex(lcd, "Temp: %f%C%s", 25.5, 0, "C"); // Prints "Temp: 25.5°C"
 */
lcd_error_t lcd_print_flex(lcd_handle_t* lcd, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* LCD_I2C_H */