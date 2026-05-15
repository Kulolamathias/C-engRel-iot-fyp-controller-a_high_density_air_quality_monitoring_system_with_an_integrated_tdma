#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "lcd_i2c.h"

#define I2C_MASTER_SCL_IO          22
#define I2C_MASTER_SDA_IO          21
#define I2C_MASTER_NUM             I2C_NUM_0
#define I2C_MASTER_FREQ_HZ         100000

#define LCD_I2C_ADDR               0x27
#define LCD_COLS                   20
#define LCD_ROWS                   4

static const char *TAG = "MAIN";

void app_main(void) {
    // Initialize I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
    
    // Configure LCD
    lcd_config_t lcd_config = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = LCD_I2C_ADDR,
        .rows = LCD_ROWS,
        .cols = LCD_COLS,
        .backlight_enable = true,
        .i2c_timeout_ms = 1000,
        .cmd_delay_us = 50
    };
    
    // Initialize LCD
    lcd_handle_t* lcd = lcd_i2c_init(&lcd_config);
    if (!lcd) {
    ````````````````        ESP_LOGE(TAG, "LCD initialization failed!");
        return;
    }
    
    ESP_LOGI(TAG, "LCD 2004 initialized successfully!");
    
    // Demo all features
    lcd_clear(lcd);
    lcd_set_cursor(lcd, 0, 0);
    lcd_print_str(lcd, "LCD 2004 Working!");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    lcd_set_cursor(lcd, 1, 0);
    lcd_printf(lcd, "Int: %d", 12345);
    
    lcd_set_cursor(lcd, 2, 0);
    lcd_printf(lcd, "Hex: 0x%X", 0xABCD);
    
    lcd_set_cursor(lcd, 3, 0);
    lcd_printf(lcd, "Float: %.2f", 3.14159);
    
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // Scroll demo
    lcd_clear(lcd);
    lcd_set_cursor(lcd, 0, 0);
    lcd_print_str(lcd, "Scrolling Text for demo... --->");
    
    for (int i = 0; i < 5; i++) {
        lcd_scroll(lcd, LCD_SCROLL_LEFT, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Backlight control
    for (int i = 0; i < 5; i++) {
        lcd_backlight(lcd, LCD_BACKLIGHT_OFF);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_backlight(lcd, LCD_BACKLIGHT_ON);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // Final display
    lcd_clear(lcd);
    lcd_set_cursor(lcd, 0, 0);
    lcd_print_str(lcd, "Mathias & Constantine");
    lcd_set_cursor(lcd, 1, 0);
    lcd_print_str(lcd, "All Features OK");
    lcd_set_cursor(lcd, 2, 0);
    lcd_print_str(lcd, "ESP32 + LCD 2004");
    
    ESP_LOGI(TAG, "Demo complete!");
    
    // Keep running
    int counter = 0;
    while (1) {
        lcd_set_cursor(lcd, 3, 0);
        lcd_printf(lcd, "Counter: %d     ", counter++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}