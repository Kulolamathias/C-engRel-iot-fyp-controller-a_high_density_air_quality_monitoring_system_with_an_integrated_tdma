    /**
     * @file lcd_i2c.c
     * @brief I2C LCD Driver Implementation for ESP-IDF
     * @author Your Name
     * @date 2024
     * @version 1.0.0
     */

    #include <stdio.h>
    #include <string.h>
    #include <stdarg.h>
    #include <inttypes.h>
    #include <math.h>

    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "esp_log.h"
    #include "esp_err.h"
    #include "esp_timer.h"

    #include "lcd_i2c_priv.h"
    #include "lcd_i2c.h"

    static const char *TAG = "LCD_I2C";

    /**
     * @brief LCD handle structure
     */
    struct lcd_handle_t {
        lcd_config_t config;          /**< LCD configuration */
        SemaphoreHandle_t mutex;      /**< Mutex for thread safety */
        bool backlight_state;         /**< Current backlight state */
        bool display_on;              /**< Display enabled flag */
        bool cursor_on;               /**< Cursor enabled flag */
        bool blink_on;                /**< Cursor blink enabled flag */
        uint8_t entry_mode;           /**< Current entry mode setting */
        uint8_t function_set;         /**< Current function set */
    };

    /**
     * @brief Delay for specified microseconds
     * 
     * @param us Microseconds to delay
     */
    static void lcd_delay_us(uint32_t us) {
        if (us < 1000) {
            esp_rom_delay_us(us);
        } else {
            vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
        }
    }

    /**
     * @brief Send data via I2C with retry mechanism
     * 
     * @param lcd LCD handle
     * @param data Data to send
     * @return esp_err_t ESP error code
     */
    static esp_err_t lcd_i2c_send(lcd_handle_t* lcd, uint8_t data) {
        esp_err_t err;
        uint8_t buffer[1] = {data};
        
        for (int retry = 0; retry < 3; retry++) {
            err = i2c_master_write_to_device(lcd->config.i2c_port,
                                            lcd->config.i2c_addr,
                                            buffer, 1,
                                            pdMS_TO_TICKS(lcd->config.i2c_timeout_ms));
            if (err == ESP_OK) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(err));
        return err;
    }

    /**
     * @brief Write 4-bit nibble to LCD
     * 
     * @param lcd LCD handle
     * @param nibble 4-bit data nibble
     * @param rs Register Select (true=data, false=command)
     * @return lcd_error_t Operation status
     */
    static lcd_error_t lcd_write_nibble(lcd_handle_t* lcd, uint8_t nibble, bool rs) {
        uint8_t data = nibble & 0x0F;
        
        // Set data bits (D4-D7 on P4-P7)
        data <<= 4;
        
        // Set RS bit
        if (rs) {
            data |= LCD_I2C_RS;
        }
        
        // Set backlight bit
        if (lcd->backlight_state && lcd->config.backlight_enable) {
            data |= LCD_I2C_BACKLIGHT;
        }
        
        // Generate enable pulse
        uint8_t data_en = data | LCD_I2C_EN;
        uint8_t data_no_en = data & ~LCD_I2C_EN;
        
        esp_err_t err;
        err = lcd_i2c_send(lcd, data_en);
        if (err != ESP_OK) return LCD_ERR_I2C;
        
        esp_rom_delay_us(1);  // Enable pulse width > 450ns
        
        err = lcd_i2c_send(lcd, data_no_en);
        if (err != ESP_OK) return LCD_ERR_I2C;
        
        return LCD_OK;
    }

    /**
     * @brief Write byte to LCD (command or data)
     * 
     * @param lcd LCD handle
     * @param byte Byte to write
     * @param rs Register Select (true=data, false=command)
     * @return lcd_error_t Operation status
     */
    static lcd_error_t lcd_write_byte(lcd_handle_t* lcd, uint8_t byte, bool rs) {
        lcd_error_t err;
        
        // Write high nibble
        err = lcd_write_nibble(lcd, byte >> 4, rs);
        if (err != LCD_OK) return err;
        
        // Write low nibble
        err = lcd_write_nibble(lcd, byte & 0x0F, rs);
        if (err != LCD_OK) return err;
        
        // Command delay
        if (!rs && byte < 4) {  // Clear and home commands need longer delay
            lcd_delay_us(2000);
        } else {
            lcd_delay_us(lcd->config.cmd_delay_us);
        }
        
        return LCD_OK;
    }

    lcd_handle_t* lcd_i2c_init(const lcd_config_t* config) {
        if (config == NULL) {
            ESP_LOGE(TAG, "Configuration is NULL");
            return NULL;
        }
        
        if (config->rows == 0 || config->cols == 0) {
            ESP_LOGE(TAG, "Invalid dimensions: %dx%d", config->cols, config->rows);
            return NULL;
        }
        
        // Allocate LCD handle
        lcd_handle_t* lcd = (lcd_handle_t*)malloc(sizeof(lcd_handle_t));
        if (lcd == NULL) {
            ESP_LOGE(TAG, "Memory allocation failed");
            return NULL;
        }
        
        // Initialize with zeros
        memset(lcd, 0, sizeof(lcd_handle_t));
        
        // Copy configuration
        memcpy(&lcd->config, config, sizeof(lcd_config_t));
        
        // Set default values if not provided
        if (lcd->config.i2c_timeout_ms == 0) {
            lcd->config.i2c_timeout_ms = 100;
        }
        if (lcd->config.cmd_delay_us == 0) {
            lcd->config.cmd_delay_us = 50;
        }
        
        // Create mutex for thread safety
        lcd->mutex = xSemaphoreCreateMutex();
        if (lcd->mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            free(lcd);
            return NULL;
        }
        
        // Initialize LCD (4-bit mode initialization sequence)
        
        // Wait for LCD to power up (>40ms)
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Initialization sequence for 4-bit mode
        lcd_write_nibble(lcd, 0x03, false);  // Function set (8-bit)
        vTaskDelay(pdMS_TO_TICKS(5));
        
        lcd_write_nibble(lcd, 0x03, false);  // Function set (8-bit)
        esp_rom_delay_us(150);
        
        lcd_write_nibble(lcd, 0x03, false);  // Function set (8-bit)
        esp_rom_delay_us(150);
        
        lcd_write_nibble(lcd, 0x02, false);  // Function set (4-bit)
        esp_rom_delay_us(150);
        
        // Set function: 4-bit, 2 lines, 5x8 dots
        uint8_t function = LCD_CMD_FUNCTION | LCD_FUNC_4BIT;
        if (lcd->config.rows > 1) {
            function |= LCD_FUNC_2LINE;
        }
        lcd->function_set = function;
        lcd_write_byte(lcd, function, false);
        
        // Display off
        lcd->display_on = false;
        lcd->cursor_on = false;
        lcd->blink_on = false;
        lcd_write_byte(lcd, LCD_CMD_DISPLAY_CTRL, false);
        
        // Clear display
        lcd_write_byte(lcd, LCD_CMD_CLEAR, false);
        vTaskDelay(pdMS_TO_TICKS(2));
        
        // Entry mode set: increment, no shift
        lcd->entry_mode = LCD_CMD_ENTRY_MODE | LCD_ENTRY_LEFT;
        lcd_write_byte(lcd, lcd->entry_mode, false);
        
        // Display on, cursor off, blink off
        lcd->display_on = true;
        uint8_t display_ctrl = LCD_CMD_DISPLAY_CTRL | LCD_DISPLAY_ON;
        lcd_write_byte(lcd, display_ctrl, false);
        
        // Backlight on by default if enabled
        lcd->backlight_state = lcd->config.backlight_enable;
        
        ESP_LOGI(TAG, "LCD initialized: %dx%d, I2C addr: 0x%02X", 
                config->cols, config->rows, config->i2c_addr);
        
        return lcd;
    }

    lcd_error_t lcd_i2c_deinit(lcd_handle_t* lcd) {
        if (lcd == NULL) {
            return LCD_ERR_INVALID_ARG;
        }
        
        // Turn off display
        lcd_write_byte(lcd, LCD_CMD_DISPLAY_CTRL, false);
        
        // Turn off backlight
        if (lcd->config.backlight_enable) {
            lcd->backlight_state = false;
            lcd_i2c_send(lcd, 0);
        }
        
        // Free resources
        if (lcd->mutex) {
            vSemaphoreDelete(lcd->mutex);
        }
        free(lcd);
        
        return LCD_OK;
    }

    lcd_error_t lcd_clear(lcd_handle_t* lcd) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = lcd_write_byte(lcd, LCD_CMD_CLEAR, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_home(lcd_handle_t* lcd) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = lcd_write_byte(lcd, LCD_CMD_HOME, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_set_cursor(lcd_handle_t* lcd, uint8_t row, uint8_t col) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        if (row >= lcd->config.rows || col >= lcd->config.cols) {
            return LCD_ERR_INVALID_ARG;
        }
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        // DDRAM addresses for different LCD types
        static const uint8_t row_offsets_2004[] = {0x00, 0x40, 0x14, 0x54};
        static const uint8_t row_offsets_1602[] = {0x00, 0x40};
        
        uint8_t address = col;
        if (lcd->config.rows == 4 && lcd->config.cols == 20) {
            // 2004 LCD
            if (row < 4) address += row_offsets_2004[row];
        } else if (lcd->config.rows == 2 && lcd->config.cols == 16) {
            // 1602 LCD
            if (row < 2) address += row_offsets_1602[row];
        } else {
            // Custom LCD - assume linear addressing
            address += row * lcd->config.cols;
        }
        
        lcd_error_t result = lcd_write_byte(lcd, LCD_CMD_DDRAM_ADDR | address, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_display_control(lcd_handle_t* lcd, bool display, bool cursor, bool blink) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd->display_on = display;
        lcd->cursor_on = cursor;
        lcd->blink_on = blink;
        
        uint8_t cmd = LCD_CMD_DISPLAY_CTRL;
        if (display) cmd |= LCD_DISPLAY_ON;
        if (cursor) cmd |= LCD_CURSOR_ON;
        if (blink) cmd |= LCD_BLINK_ON;
        
        lcd_error_t result = lcd_write_byte(lcd, cmd, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_entry_mode(lcd_handle_t* lcd, lcd_entry_mode_t direction, bool shift) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd->entry_mode = LCD_CMD_ENTRY_MODE | direction;
        if (shift) {
            lcd->entry_mode |= 0x01;  // Display shift
        }
        
        lcd_error_t result = lcd_write_byte(lcd, lcd->entry_mode, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_print_str(lcd_handle_t* lcd, const char* str) {
        if (lcd == NULL || str == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = LCD_OK;
        for (size_t i = 0; str[i] != '\0'; i++) {
            result = lcd_write_byte(lcd, str[i], true);
            if (result != LCD_OK) {
                break;
            }
        }
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    // lcd_error_t lcd_printf(lcd_handle_t* lcd, const char* format, ...) {
    //     if (lcd == NULL || format == NULL) return LCD_ERR_INVALID_ARG;
        
    //     char buffer[64];
    //     va_list args;
    //     va_start(args, format);
    //     vsnprintf(buffer, sizeof(buffer), format, args);
    //     va_end(args);
        
    //     return lcd_print_str(lcd, buffer);
    // }

    lcd_error_t lcd_print_int(lcd_handle_t* lcd, int32_t num, uint8_t base) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        if (base != 10 && base != 16) {
            return LCD_ERR_INVALID_ARG;
        }
        
        char buffer[33];
        
        if (base == 10) {
            snprintf(buffer, sizeof(buffer), "%" PRId32, num);
        } else {
            snprintf(buffer, sizeof(buffer), "%" PRIX32, num);
        }
        
        return lcd_print_str(lcd, buffer);
    }

    lcd_error_t lcd_print_float(lcd_handle_t* lcd, float num, uint8_t decimals) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        if (decimals > 6) {
            return LCD_ERR_INVALID_ARG;
        }
        
        char buffer[32];
        char format[10];
        
        snprintf(format, sizeof(format), "%%.%df", decimals);
        snprintf(buffer, sizeof(buffer), format, num);
        
        return lcd_print_str(lcd, buffer);
    }

    lcd_error_t lcd_create_char(lcd_handle_t* lcd, uint8_t location, const lcd_custom_char_t* char_data) {
        if (lcd == NULL || char_data == NULL) return LCD_ERR_INVALID_ARG;
        if (location > 7) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        // Set CGRAM address
        lcd_error_t result = lcd_write_byte(lcd, LCD_CMD_CGRAM_ADDR | (location << 3), false);
        
        if (result == LCD_OK) {
            // Write character data
            for (int i = 0; i < 8; i++) {
                result = lcd_write_byte(lcd, char_data->data[i], true);
                if (result != LCD_OK) {
                    break;
                }
            }
            
            // Return to DDRAM
            if (result == LCD_OK) {
                result = lcd_write_byte(lcd, LCD_CMD_DDRAM_ADDR, false);
            }
        }
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_scroll(lcd_handle_t* lcd, lcd_scroll_dir_t direction, uint8_t count) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = LCD_OK;
        for (uint8_t i = 0; i < count; i++) {
            if (direction == LCD_SCROLL_LEFT) {
                result = lcd_write_byte(lcd, 0x18, false);  // Scroll left command
            } else {
                result = lcd_write_byte(lcd, 0x1C, false);  // Scroll right command
            }
            if (result != LCD_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_backlight(lcd_handle_t* lcd, lcd_backlight_t state) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        if (!lcd->config.backlight_enable) {
            return LCD_ERR_INVALID_ARG;
        }
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd->backlight_state = (state == LCD_BACKLIGHT_ON);
        
        // Send a dummy write to update backlight state
        lcd_error_t result = lcd_write_byte(lcd, 0x00, false);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    lcd_error_t lcd_get_dimensions(lcd_handle_t* lcd, uint8_t* rows, uint8_t* cols) {
        if (lcd == NULL || rows == NULL || cols == NULL) {
            return LCD_ERR_INVALID_ARG;
        }
        
        *rows = lcd->config.rows;
        *cols = lcd->config.cols;
        
        return LCD_OK;
    }

    lcd_error_t lcd_goto_row(lcd_handle_t* lcd, uint8_t row) {
        return lcd_set_cursor(lcd, row, 0);
    }




    /**
     * @brief Print mixed data types flexibly
     */
    lcd_error_t lcd_print_mixed(lcd_handle_t* lcd, const lcd_print_item_t* items, uint8_t count) {
        if (lcd == NULL || items == NULL || count == 0) {
            return LCD_ERR_INVALID_ARG;
        }
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = LCD_OK;
        
        for (uint8_t i = 0; i < count; i++) {
            const lcd_print_item_t* item = &items[i];
            
            switch (item->type) {
                case LCD_PRINT_STRING:
                    if (item->data.str != NULL) {
                        for (size_t j = 0; item->data.str[j] != '\0'; j++) {
                            result = lcd_write_byte(lcd, item->data.str[j], true);
                            if (result != LCD_OK) break;
                        }
                    }
                    break;
                    
                case LCD_PRINT_INT:
                    {
                        char buffer[12];
                        snprintf(buffer, sizeof(buffer), "%" PRId32, item->data.integer);
                        for (size_t j = 0; buffer[j] != '\0'; j++) {
                            result = lcd_write_byte(lcd, buffer[j], true);
                            if (result != LCD_OK) break;
                        }
                    }
                    break;
                    
                case LCD_PRINT_HEX:
                    {
                        char buffer[10];
                        snprintf(buffer, sizeof(buffer), "0x%lX", item->data.hex);
                        for (size_t j = 0; buffer[j] != '\0'; j++) {
                            result = lcd_write_byte(lcd, buffer[j], true);
                            if (result != LCD_OK) break;
                        }
                    }
                    break;
                    
                case LCD_PRINT_FLOAT:
                    {
                        char buffer[16];
                        char format[8];
                        uint8_t decimals = (item->decimals > 6) ? 6 : item->decimals;
                        snprintf(format, sizeof(format), "%%.%df", decimals);
                        snprintf(buffer, sizeof(buffer), format, item->data.floating);
                        for (size_t j = 0; buffer[j] != '\0'; j++) {
                            result = lcd_write_byte(lcd, buffer[j], true);
                            if (result != LCD_OK) break;
                        }
                    }
                    break;
                    
                case LCD_PRINT_CHAR:
                    result = lcd_write_byte(lcd, item->data.character, true);
                    break;
                    
                case LCD_PRINT_CUSTOM_CHAR:
                    if (item->data.custom_char <= 7) {
                        result = lcd_write_byte(lcd, item->data.custom_char, true);
                    } else {
                        result = LCD_ERR_INVALID_ARG;
                    }
                    break;
                    
                default:
                    result = LCD_ERR_INVALID_ARG;
                    break;
            }
            
            if (result != LCD_OK) {
                break;
            }
        }
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    /**
     * @brief Print single character
     */
    lcd_error_t lcd_print_char(lcd_handle_t* lcd, char c) {
        if (lcd == NULL) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = lcd_write_byte(lcd, c, true);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    /**
     * @brief Print custom character
     */
    lcd_error_t lcd_print_custom_char(lcd_handle_t* lcd, uint8_t location) {
        if (lcd == NULL || location > 7) return LCD_ERR_INVALID_ARG;
        
        if (xSemaphoreTake(lcd->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return LCD_ERR_MUTEX;
        }
        
        lcd_error_t result = lcd_write_byte(lcd, location, true);
        
        xSemaphoreGive(lcd->mutex);
        return result;
    }

    /**
     * @brief Create and print mixed content in one call (convenience function)
     */
    lcd_error_t lcd_print_flex(lcd_handle_t* lcd, const char* format, ...) {
        if (lcd == NULL || format == NULL) return LCD_ERR_INVALID_ARG;
        
        // Count format specifiers to allocate array
        uint8_t spec_count = 0;
        for (const char* p = format; *p != '\0'; p++) {
            if (*p == '%' && *(p + 1) != '\0') {
                spec_count++;
                p++; // Skip the format char
            }
        }
        
        if (spec_count == 0) {
            // No format specifiers, just print the string
            return lcd_print_str(lcd, format);
        }
        
        // Parse format and collect data
        va_list args;
        va_start(args, format);
        
        lcd_error_t result = LCD_OK;
        const char* current = format;
        
        while (*current != '\0' && result == LCD_OK) {
            if (*current == '%' && *(current + 1) != '\0') {
                current++; // Move to format char
                
                switch (*current) {
                    case 's': { // String
                        const char* str = va_arg(args, const char*);
                        if (str) result = lcd_print_str(lcd, str);
                        break;
                    }
                    case 'd': { // Integer
                        int32_t num = va_arg(args, int32_t);
                        result = lcd_print_int(lcd, num, 10);
                        break;
                    }
                    case 'x': { // Hexadecimal
                        uint32_t hex = va_arg(args, uint32_t);
                        result = lcd_print_int(lcd, (int32_t)hex, 16);
                        break;
                    }
                    case 'f': { // Float
                        double num = va_arg(args, double);
                        uint8_t decimals = 2; // Default
                        // Check for precision specifier like %.1f
                        if (current - format >= 2 && *(current - 2) == '.') {
                            decimals = *(current - 1) - '0';
                            if (decimals > 6) decimals = 6;
                        }
                        result = lcd_print_float(lcd, (float)num, decimals);
                        break;
                    }
                    case 'c': { // Character
                        char c = (char)va_arg(args, int);
                        result = lcd_print_char(lcd, c);
                        break;
                    }
                    case 'C': { // Custom character
                        uint8_t custom = (uint8_t)va_arg(args, int);
                        result = lcd_print_custom_char(lcd, custom);
                        break;
                    }
                    case '%': { // Percent sign
                        result = lcd_print_char(lcd, '%');
                        break;
                    }
                    default:
                        result = LCD_ERR_FORMAT;
                        break;
                }
                current++;
            } else {
                // Print regular character
                result = lcd_print_char(lcd, *current);
                current++;
            }
        }
        
        va_end(args);
        return result;
    }

    /**
     * @brief Enhanced printf with larger buffer
     */
    lcd_error_t lcd_printf(lcd_handle_t* lcd, const char* format, ...) {
        if (lcd == NULL || format == NULL) return LCD_ERR_INVALID_ARG;
        
        char buffer[128];  // Increased buffer size
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        
        return lcd_print_str(lcd, buffer);
    }