/**
 * @file main/main.c
 * @brief Receiver node for the TDMA air‑quality monitoring system.
 *
 * Uses a reliable HD44780 LCD driver over I²C, passive buzzer, and
 * dynamic slot assignment with relaxed data acceptance for debugging.
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
#include "driver/gpio.h"
#include "driver/i2c.h"            /* legacy I2C */
#include "driver/ledc.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "cJSON.h"

#include "network.h"

static const char *TAG = "RX";

/* ---------- Hardware pins ---------- */
#define BUZZER_GPIO             GPIO_NUM_23
#define LCD_I2C_PORT            I2C_NUM_0
#define LCD_I2C_ADDR            0x27
#define LCD_COLS                20
#define LCD_ROWS                4

/* ---------- TDMA schedule ---------- */
#define TDMA_NUM_SLOTS          2
#define TDMA_SLOT_PERIOD_MS     10000
#define TDMA_SLOT_DURATION_MS   5000

/* ---------- Alert threshold ---------- */
#define ALERT_THRESHOLD_PPM     1000.0f

/* ---------- Buzzer LEDC config ---------- */
#define BUZZER_LEDC_TIMER       LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BUZZER_LEDC_RESOLUTION  LEDC_TIMER_10_BIT

/* ---------- Alert tone pattern ---------- */
#define TONE_ALERT_FREQ         2500
#define TONE_ALERT_ON_DURATION  300
#define TONE_ALERT_OFF_DURATION 200
#define TONE_ALERT_REPEAT       3

/* ---------- Slot entry ---------- */
typedef struct {
    char     node_id[16];
    bool     active;
    float    last_ppm;
    float    last_voltage;
    bool     alert_active;
} slot_entry_t;

static slot_entry_t slots[TDMA_NUM_SLOTS] = {0};

/* ---------- Buzzer state ---------- */
static bool buzzer_is_alerting = false;
static esp_timer_handle_t buzzer_timer = NULL;
static int alert_beep_count = 0;
static bool alert_beep_on = false;

/* ---------- LCD private definitions ---------- */
#define LCD_RS   (1<<0)
#define LCD_RW   (1<<1)
#define LCD_EN   (1<<2)
#define LCD_BL   (1<<3)

static SemaphoreHandle_t i2c_mutex = NULL;

/* Send one byte to the PCF8574 without pulsing EN */
static void lcd_write_byte(uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data | LCD_BL, true);   /* backlight on */
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(LCD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
}

/* Pulse EN: set data with EN high, then low */
static void lcd_pulse(uint8_t data)
{
    lcd_write_byte(data | LCD_EN);
    esp_rom_delay_us(1);
    lcd_write_byte(data);
    esp_rom_delay_us(50);
}

/* Send a 4-bit nibble (used after init) */
static void lcd_send_nibble(uint8_t nib, uint8_t rs)
{
    uint8_t d = (nib << 4) | rs;
    lcd_pulse(d);
}

/* Send a byte (two nibbles) */
static void lcd_send_byte(uint8_t byte, uint8_t rs)
{
    lcd_send_nibble(byte >> 4, rs);
    lcd_send_nibble(byte & 0x0F, rs);
}

static void lcd_cmd(uint8_t cmd) { lcd_send_byte(cmd, 0); }
static void lcd_data(uint8_t data) { lcd_send_byte(data, LCD_RS); }

/* Initialisation sequence that absolutely works with PCF8574 + HD44780 */
static void init_lcd(void)
{
    /* Install I²C driver */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_21,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = GPIO_NUM_22,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };
    ESP_ERROR_CHECK(i2c_param_config(LCD_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(LCD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
    i2c_mutex = xSemaphoreCreateMutex();

    /* Power-on delay (at least 40 ms) */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 8-bit init sequence ---- */
    lcd_pulse(0x30);
    esp_rom_delay_us(4500);
    lcd_pulse(0x30);
    esp_rom_delay_us(150);
    lcd_pulse(0x30);
    esp_rom_delay_us(150);
    lcd_pulse(0x20);   /* switch to 4-bit mode */
    esp_rom_delay_us(150);

    /* ---- 4-bit configuration ---- */
    lcd_cmd(0x28);   /* 4-bit, 2 lines, 5x8 */
    lcd_cmd(0x08);   /* display off */
    lcd_cmd(0x01);   /* clear */
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_cmd(0x06);   /* entry mode */
    lcd_cmd(0x0C);   /* display on, cursor off, blink off */
}

/* Write one line to a specific row */
static void write_line(unsigned int row, const char *text)
{
    char buf[21];
    strncpy(buf, text, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    lcd_cmd(0x80 | row_offsets[row]);
    for (int col = 0; col < LCD_COLS && buf[col] != '\0'; col++) {
        lcd_data((uint8_t)buf[col]);
    }
}

static void lcd_write(const char *l1, const char *l2,
                      const char *l3, const char *l4)
{
    if (i2c_mutex) xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    write_line(0, l1);
    write_line(1, l2);
    write_line(2, l3);
    write_line(3, l4);
    if (i2c_mutex) xSemaphoreGive(i2c_mutex);
}

/* =====================================================================
 * Buzzer
 * ===================================================================== */
static void buzzer_tone_simple(uint32_t freq, uint32_t dur);
static void start_alert_pattern(void);
static void buzzer_silence(void);

static void buzzer_timer_cb(void *arg)
{
    if (alert_beep_on) {
        buzzer_silence();
        alert_beep_on = false;
        alert_beep_count++;
        if (alert_beep_count < TONE_ALERT_REPEAT) {
            esp_timer_start_once(buzzer_timer, TONE_ALERT_OFF_DURATION * 1000);
        } else {
            alert_beep_count = 0;
            buzzer_is_alerting = false;
        }
    } else if (alert_beep_count > 0) {
        buzzer_tone_simple(TONE_ALERT_FREQ, TONE_ALERT_ON_DURATION);
        alert_beep_on = true;
        esp_timer_start_once(buzzer_timer, TONE_ALERT_ON_DURATION * 1000);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    }
}

static void init_buzzer(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_RESOLUTION,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    esp_timer_create_args_t t_args = {
        .callback = buzzer_timer_cb,
        .arg = NULL,
        .name = "buzzer_off"
    };
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &buzzer_timer));
}

static void buzzer_tone_simple(uint32_t freq, uint32_t dur)
{
    esp_timer_stop(buzzer_timer);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_TIMER, freq);
    uint32_t duty = (1 << BUZZER_LEDC_RESOLUTION) / 2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    esp_timer_start_once(buzzer_timer, dur * 1000);
    alert_beep_count = 0;
    alert_beep_on = false;
}

static void start_alert_pattern(void)
{
    if (buzzer_is_alerting) return;
    buzzer_is_alerting = true;
    esp_timer_stop(buzzer_timer);
    alert_beep_count = 0;
    alert_beep_on = true;
    buzzer_tone_simple(TONE_ALERT_FREQ, TONE_ALERT_ON_DURATION);
    esp_timer_start_once(buzzer_timer, TONE_ALERT_ON_DURATION * 1000);
}

static void buzzer_silence(void)
{
    esp_timer_stop(buzzer_timer);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    buzzer_is_alerting = false;
    alert_beep_count = 0;
    alert_beep_on = false;
}

/* =====================================================================
 * MQTT data handling (relaxed: any data updates the first empty slot)
 * ===================================================================== */
static void handle_data(const char *data, size_t data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) return;
    cJSON *i = cJSON_GetObjectItem(root, "id");
    cJSON *p = cJSON_GetObjectItem(root, "ppm");
    cJSON *v = cJSON_GetObjectItem(root, "V");
    if (!cJSON_IsString(i) || !cJSON_IsNumber(p) || !cJSON_IsNumber(v)) {
        cJSON_Delete(root);
        return;
    }

    /* Find existing slot or first empty one */
    int slot_idx = -1;
    for (int j = 0; j < TDMA_NUM_SLOTS; j++) {
        if (slots[j].active && strcmp(slots[j].node_id, i->valuestring) == 0) {
            slot_idx = j;
            break;
        }
    }
    if (slot_idx < 0) {
        for (int j = 0; j < TDMA_NUM_SLOTS; j++) {
            if (!slots[j].active) {
                slot_idx = j;
                strlcpy(slots[j].node_id, i->valuestring, sizeof(slots[j].node_id));
                slots[j].active = true;
                break;
            }
        }
    }
    if (slot_idx < 0) {
        ESP_LOGE(TAG, "No free slot for %s", i->valuestring);
        cJSON_Delete(root);
        return;
    }

    slots[slot_idx].last_ppm = (float)p->valuedouble;
    slots[slot_idx].last_voltage = (float)v->valuedouble;
    cJSON_Delete(root);

    /* Alert check */
    bool any_alert = false;
    for (int j = 0; j < TDMA_NUM_SLOTS; j++) {
        if (slots[j].active && slots[j].last_ppm > ALERT_THRESHOLD_PPM) {
            slots[j].alert_active = true;
            any_alert = true;
        } else {
            slots[j].alert_active = false;
        }
    }
    if (any_alert) {
        if (!buzzer_is_alerting) start_alert_pattern();
    } else {
        buzzer_silence();
    }
}

/* =====================================================================
 * Display update
 * ===================================================================== */
static void update_display(void)
{
    char l1[21], l2[21], l3[21], l4[21];
    int active = -1;
    int64_t now_ms = esp_timer_get_time() / 1000;

    /* Determine active slot by time */
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        uint32_t off = i * TDMA_SLOT_DURATION_MS;
        int64_t s = (now_ms / TDMA_SLOT_PERIOD_MS) * TDMA_SLOT_PERIOD_MS + off;
        int64_t e = s + TDMA_SLOT_DURATION_MS;
        if (now_ms >= s && now_ms < e) { active = i; break; }
    }

    strlcpy(l1, "TDMA Air Monitor  ", sizeof(l1));
    if (active >= 0 && slots[active].active) {
        snprintf(l2, sizeof(l2), ">> %.7s TX <<", slots[active].node_id);
        snprintf(l3, sizeof(l3), "%.7s:%4.0fP %3.2fV",
                 slots[active].node_id,
                 (double)slots[active].last_ppm,
                 (double)slots[active].last_voltage);
    } else {
        strlcpy(l2, "   Waiting...      ", sizeof(l2));
        strlcpy(l3, "No active TX       ", sizeof(l3));
    }
    int other = (active == 0) ? 1 : 0;
    if (slots[other].active) {
        snprintf(l4, sizeof(l4), "%.7s:%4.0fP %3.2fV",
                 slots[other].node_id,
                 (double)slots[other].last_ppm,
                 (double)slots[other].last_voltage);
    } else {
        strlcpy(l4, "                    ", sizeof(l4));
    }
    lcd_write(l1, l2, l3, l4);
}

static void mqtt_data_handler(const char *topic, const char *data, size_t len)
{
    if (strcmp(topic, "airquality/data") == 0) {
        handle_data(data, len);
    }
}

/* ===================================================================== */
void app_main(void)
{
    ESP_ERROR_CHECK(network_start());

    init_lcd();
    init_buzzer();

    /* Short startup beep */
    buzzer_tone_simple(1500, 200);

    network_mqtt_set_data_callback(mqtt_data_handler);
    network_mqtt_subscribe("airquality/data", 1);
    ESP_LOGI(TAG, "Receiver ready");

    /* Pre-populate slot 0 with dummy? No. */
    while (1) {
        update_display();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}