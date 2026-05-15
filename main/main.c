/**
 * @file main/main.c
 * @brief TDMA Air‑Quality Receiver – using proven lcd_i2c driver.
 *
 * @author Matthithyahu
 * @date 2026/05/12
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "cJSON.h"

#include "network.h"
#include "lcd_i2c.h"
#include "passive_buzzer.h"

static const char *TAG = "RX";

/* ---------- I2C & LCD config (same as your working code) ---------- */
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  100000

#define LCD_I2C_ADDR        0x27
#define LCD_ROWS            4
#define LCD_COLS            20

static lcd_handle_t *lcd = NULL;
static bool was_connected = false;

/* ---------- TDMA constants ---------- */
#define NUM_SLOTS         2
#define SLOT_PERIOD_MS    10000
#define SLOT_DURATION_MS  5000

#define ALERT_PPM_THRESHOLD 1000.0f

/* ---------- Slot table ---------- */
typedef struct {
    char  id[16];
    bool  active;
    int64_t  last_rx_time_us;
    float ppm;
    float voltage;
    bool  alert;
} slot_t;

static slot_t slots[NUM_SLOTS] = {0};

/* ---------- Helper: find or allocate slot ---------- */
static int find_or_alloc_slot(const char *id)
{
    for (int i = 0; i < NUM_SLOTS; i++)
        if (slots[i].active && strcmp(slots[i].id, id) == 0)
            return i;
    for (int i = 0; i < NUM_SLOTS; i++)
        if (!slots[i].active) {
            slots[i].active = true;
            strlcpy(slots[i].id, id, sizeof(slots[i].id));
            return i;
        }
    return -1;
}

/* ---------- MQTT data callback ---------- */
static void on_data(const char *topic, const char *data, size_t len)
{
    if (strcmp(topic, "airquality/data") != 0) return;

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *ppm = cJSON_GetObjectItem(root, "ppm");
    cJSON *v   = cJSON_GetObjectItem(root, "V");
    if (!cJSON_IsString(id) || !cJSON_IsNumber(ppm) || !cJSON_IsNumber(v)) {
        cJSON_Delete(root);
        return;
    }

    int idx = find_or_alloc_slot(id->valuestring);
    if (idx < 0) {
        cJSON_Delete(root);
        return;
    }

    slots[idx].ppm = (float)ppm->valuedouble;
    slots[idx].last_rx_time_us = esp_timer_get_time();
    slots[idx].voltage = (float)v->valuedouble;
    cJSON_Delete(root);

    /* Alert evaluation */
    bool any_alert = false;
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (slots[i].active && slots[i].ppm > ALERT_PPM_THRESHOLD) {
            slots[i].alert = true;
            any_alert = true;
        } else {
            slots[i].alert = false;
        }
    }
    if (any_alert) buzzer_alert_pattern();
    else           buzzer_stop();
}

/* ---------- Display update – using your driver's lcd_printf ---------- */
static void update_display(void)
{
    if (!lcd) return;

    int  active = -1;
    int64_t now_ms = esp_timer_get_time() / 1000;

    /* Time‑based active slot detection */
    for (int i = 0; i < NUM_SLOTS; i++) {
        uint32_t off = i * SLOT_DURATION_MS;
        int64_t s = (now_ms / SLOT_PERIOD_MS) * SLOT_PERIOD_MS + off;
        int64_t e = s + SLOT_DURATION_MS;
        if (now_ms >= s && now_ms < e) { active = i; break; }
    }

    /* Line 1 – fixed title */
    lcd_set_cursor(lcd, 0, 0);
    lcd_print_str(lcd, "TDMA Air Monitor  ");

    /* Line 2 – active transmitter indicator */
    lcd_set_cursor(lcd, 1, 0);
    if (active >= 0 && slots[active].active) {
        lcd_printf(lcd, ">> %.7s TX <<", slots[active].id);
    } else {
        lcd_print_str(lcd, "   Waiting...      ");
    }

    /* Line 3 – active transmitter data (ppm only) */
    lcd_set_cursor(lcd, 2, 0);
    if (active >= 0 && slots[active].active) {
        /* %.7s limits ID to 7 chars, then colon, space, ppm value + unit */
        lcd_printf(lcd, "%.7s: %5.1fppm", 
                   slots[active].id,
                   (double)slots[active].ppm);
    } else {
        lcd_print_str(lcd, "                    ");  /* clear line */
    }

    /* Line 4 – other transmitter (only if active) */
    lcd_set_cursor(lcd, 3, 0);
    int other = (active == 0) ? 1 : 0;
    if (slots[other].active) {
        lcd_printf(lcd, "%.7s: %5.1fppm",
                   slots[other].id,
                   (double)slots[other].ppm);
    } else {
        lcd_print_str(lcd, "                    ");  /* clear line */
    }
}

/* ===================================================================== */
void app_main(void)
{
    ESP_ERROR_CHECK(network_start());

    /* ---- I2C initialisation (exactly like your working code) ---- */
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

    /* ---- LCD config and init ---- */
    lcd_config_t lcd_config = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = LCD_I2C_ADDR,
        .rows = LCD_ROWS,
        .cols = LCD_COLS,
        .backlight_enable = true,
        .i2c_timeout_ms = 1000,
        .cmd_delay_us = 50
    };
    lcd = lcd_i2c_init(&lcd_config);
    if (!lcd) {
        ESP_LOGE(TAG, "LCD initialization failed!");
        return;
    }

    /* ---- Buzzer ---- */
    ESP_ERROR_CHECK(buzzer_init());
    buzzer_tone(1500, 200);   /* startup beep */

    /* ---- MQTT ---- */
    network_mqtt_set_data_callback(on_data);
    network_mqtt_subscribe("airquality/data", 1);
    ESP_LOGI(TAG, "Receiver ready");

    /* ---- Main loop ---- */
    while (1) {
        bool connected = network_mqtt_is_connected();

        /* If just reconnected, re‑subscribe to keep receiving data */
        if (connected && !was_connected) {
            network_mqtt_subscribe("airquality/data", 1);
            ESP_LOGI(TAG, "Re‑subscribed after MQTT reconnection");
        }
        was_connected = connected;

        /* Timeout inactive slots (unchanged) */
        int64_t now_us = esp_timer_get_time();
        for (int i = 0; i < NUM_SLOTS; i++) {
            if (slots[i].active &&
                (now_us - slots[i].last_rx_time_us) > (2 * SLOT_PERIOD_MS * 1000)) {
                ESP_LOGW(TAG, "Slot %d timed out, clearing %s", i, slots[i].id);
                memset(&slots[i], 0, sizeof(slot_t));
            }
        }

        update_display();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}