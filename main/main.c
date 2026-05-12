/**
 * @file main/rx_main.c
 * @brief Receiver node for the TDMA air‑quality monitoring system.
 *
 * =============================================================================
 * ARCHITECTURAL ROLE
 * =============================================================================
 * This application runs on the ESP32 receiver. It manages TDMA slot assignments,
 * receives air‑quality data from two transmitters, verifies slot timing, drives
 * a 2004 LCD, and triggers a buzzer on alert conditions.
 *
 * =============================================================================
 * TDMA PROTOCOL (RECEIVER SIDE)
 * =============================================================================
 * - On boot, the receiver subscribes to airquality/request (for join requests)
 *   and airquality/data (for sensor readings).
 * - When a join request arrives, it allocates a free slot and publishes an
 *   assignment message on airquality/control.
 * - The slot table is fixed: two slots, each 5 s within a 10 s superframe.
 * - Incoming data messages are accepted only if they match an assigned
 *   transmitter AND arrive within the transmitter's slot window.
 * - The LCD shows the system status, active transmitter, and latest readings.
 * - If any reading exceeds a threshold, the buzzer is activated.
 *
 * =============================================================================
 * OWNERSHIP
 * =============================================================================
 * - Owns: slot table, LCD handle, buzzer GPIO.
 * - Provides: slot assignment service, display updater.
 * - Does NOT: read sensors, publish sensor data, or make system‑level decisions.
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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_i2c.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "network.h"    /**< Step 1 network module */

static const char *TAG = "RX";

/* -------------------------------------------------------------------------
 * Hardware pins / configuration
 * ------------------------------------------------------------------------- */
#define BUZZER_GPIO             GPIO_NUM_27   /**< Active‑high buzzer */
#define LCD_I2C_PORT            I2C_NUM_0
#define LCD_I2C_ADDR            0x27          /**< Typical for 2004 with PCF8574 */
#define LCD_COLS                20
#define LCD_ROWS                4

/* -------------------------------------------------------------------------
 * TDMA slot schedule
 * ------------------------------------------------------------------------- */
#define TDMA_NUM_SLOTS          2
#define TDMA_SLOT_PERIOD_MS     10000         /**< Super‑frame period */
#define TDMA_SLOT_DURATION_MS   5000          /**< Transmission window per slot */
#define TDMA_SLOT_GUARD_MS      200           /**< Guard time after window */

/* -------------------------------------------------------------------------
 * Alert threshold (CO₂ ppm)
 * ------------------------------------------------------------------------- */
#define ALERT_THRESHOLD_PPM     1000.0f

/* -------------------------------------------------------------------------
 * Slot table entry
 * ------------------------------------------------------------------------- */
typedef struct {
    char     node_id[16];          /**< TX_XXXX or empty */
    bool     active;               /**< True if assigned */
    int64_t  last_rx_time_us;      /**< Timestamp of last valid message */
    float    last_ppm;
    float    last_voltage;
    bool     alert_active;         /**< True if last reading exceeded threshold */
} slot_entry_t;

static slot_entry_t slots[TDMA_NUM_SLOTS] = {0};

/* LCD panel handle */
static esp_lcd_panel_handle_t lcd = NULL;

/* Buzzer state */
static bool buzzer_on = false;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */
static void init_lcd(void);
static void init_buzzer(void);
static void lcd_write(const char *line1, const char *line2,
                      const char *line3, const char *line4);
static void assign_slot(const char *node_id);
static void handle_request(const char *data, size_t data_len);
static void handle_data(const char *data, size_t data_len);
static void update_display(void);
static void set_buzzer(bool on);
static void check_alerts(void);

/* ===================================================================== */
void app_main(void)
{
    esp_err_t ret;

    /* ---- 1. Start network (Wi‑Fi, MQTT, NTP) ---- */
    ret = network_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_start failed: %s", esp_err_to_name(ret));
        return;
    }

    /* ---- 2. Initialise peripherals ---- */
    init_lcd();
    init_buzzer();

    /* ---- 3. Subscribe to MQTT topics ---- */
    network_mqtt_set_data_callback(
        [](const char *topic, const char *data, size_t data_len) {
            if (strcmp(topic, "airquality/request") == 0) {
                handle_request(data, data_len);
            } else if (strcmp(topic, "airquality/data") == 0) {
                handle_data(data, data_len);
            }
        }
    );

    network_mqtt_subscribe("airquality/request", 1);
    network_mqtt_subscribe("airquality/data", 1);
    ESP_LOGI(TAG, "Receiver ready, waiting for transmitters");

    /* ---- 4. Main loop – periodic display refresh & timeout checks ---- */
    while (1) {
        /* Check if any transmitter has timed out (no message for 2 superframes) */
        int64_t now_us = esp_timer_get_time();
        for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
            if (slots[i].active &&
                (now_us - slots[i].last_rx_time_us) > (2 * TDMA_SLOT_PERIOD_MS * 1000)) {
                ESP_LOGW(TAG, "Slot %d timed out, clearing %s", i, slots[i].node_id);
                memset(&slots[i], 0, sizeof(slot_entry_t));
            }
        }

        update_display();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =====================================================================
 * LCD Initialisation (I2C, HD44780 backpack)
 * ===================================================================== */
static void init_lcd(void)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = LCD_I2C_ADDR,
        .control_phase_bytes = 1,               /* typical */
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(LCD_I2C_PORT, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,    /* no reset pin */
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 1,     /* for HD44780, not used */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_hd44780(io_handle, &panel_config, &lcd));

    /* Reset and initialise the character LCD */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd, true));
    ESP_ERROR_CHECK(esp_lcd_panel_backlight_on_off(lcd, true));

    /* Clear the screen */
    esp_lcd_panel_clear_screen(lcd);
}

/* =====================================================================
 * Buzzer initialisation
 * ===================================================================== */
static void init_buzzer(void)
{
    gpio_config_t buzzer_cfg = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&buzzer_cfg);
    set_buzzer(false);
}

/* =====================================================================
 * Simple LCD writer – clears and writes 4 lines
 * ===================================================================== */
static void lcd_write(const char *line1, const char *line2,
                      const char *line3, const char *line4)
{
    if (!lcd) return;

    /* Move cursor to home and clear */
    esp_lcd_panel_clear_screen(lcd);

    /* Helper to write a line at a specific row */
    auto write_line = [](int row, const char *text) {
        char buf[21] = {0};
        strncpy(buf, text, sizeof(buf) - 1);
        esp_lcd_panel_set_cursor(lcd, 0, row);
        esp_lcd_panel_print(lcd, buf, strlen(buf));
    };

    write_line(0, line1);
    write_line(1, line2);
    write_line(2, line3);
    write_line(3, line4);
}

/* =====================================================================
 * Slot assignment publisher
 * ===================================================================== */
static void publish_assignment(const char *node_id, int slot, uint32_t offset_ms)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "assign");
    cJSON_AddStringToObject(root, "id", node_id);
    cJSON_AddNumberToObject(root, "slot", slot);
    cJSON_AddNumberToObject(root, "offset_ms", offset_ms);
    cJSON_AddNumberToObject(root, "duration_ms", TDMA_SLOT_DURATION_MS);
    cJSON_AddNumberToObject(root, "period_ms", TDMA_SLOT_PERIOD_MS);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        network_mqtt_publish("airquality/control", json, strlen(json), 1, false);
        ESP_LOGI(TAG, "Assigned slot %d to %s", slot, node_id);
        free(json);
    }
}

/* =====================================================================
 * Handle a join request: allocate a free slot
 * ===================================================================== */
static void assign_slot(const char *node_id)
{
    /* Look for an existing active entry with the same ID – ignore duplicate */
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        if (slots[i].active && strcmp(slots[i].node_id, node_id) == 0) {
            ESP_LOGW(TAG, "Node %s already has slot %d, ignoring", node_id, i);
            return;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        if (!slots[i].active) {
            strlcpy(slots[i].node_id, node_id, sizeof(slots[i].node_id));
            slots[i].active = true;
            slots[i].last_rx_time_us = esp_timer_get_time();
            uint32_t offset = i * TDMA_SLOT_DURATION_MS;
            publish_assignment(node_id, i, offset);
            return;
        }
    }

    ESP_LOGW(TAG, "No free slot for %s – all slots occupied", node_id);
}

/* =====================================================================
 * Process airquality/request message
 * ===================================================================== */
static void handle_request(const char *data, size_t data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) return;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    cJSON *id   = cJSON_GetObjectItem(root, "id");

    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "join") == 0 &&
        cJSON_IsString(id)) {
        assign_slot(id->valuestring);
    }

    cJSON_Delete(root);
}

/* =====================================================================
 * Process airquality/data message – must be from an assigned TX and
 * within its time window.
 * ===================================================================== */
static void handle_data(const char *data, size_t data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *ppm = cJSON_GetObjectItem(root, "ppm");
    cJSON *vol = cJSON_GetObjectItem(root, "V");

    if (!cJSON_IsString(id) || !cJSON_IsNumber(ppm) || !cJSON_IsNumber(vol)) {
        cJSON_Delete(root);
        return;
    }

    /* Find the transmitter's slot */
    int slot_idx = -1;
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        if (slots[i].active && strcmp(slots[i].node_id, id->valuestring) == 0) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        ESP_LOGW(TAG, "Data from unassigned node %s – ignored", id->valuestring);
        cJSON_Delete(root);
        return;
    }

    /* Check if message arrived within the slot window */
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;
    uint64_t superframe_start = (now_ms / TDMA_SLOT_PERIOD_MS) * TDMA_SLOT_PERIOD_MS;
    uint32_t offset_ms = slot_idx * TDMA_SLOT_DURATION_MS;
    int64_t slot_start_ms = superframe_start + offset_ms;
    int64_t slot_end_ms = slot_start_ms + TDMA_SLOT_DURATION_MS + TDMA_SLOT_GUARD_MS;

    if (now_ms < slot_start_ms || now_ms > slot_end_ms) {
        ESP_LOGW(TAG, "Message from %s outside slot window (slot %d) – ignored",
                 id->valuestring, slot_idx);
        cJSON_Delete(root);
        return;
    }

    /* Valid data – update slot entry */
    slots[slot_idx].last_rx_time_us = now_us;
    slots[slot_idx].last_ppm = (float)ppm->valuedouble;
    slots[slot_idx].last_voltage = (float)vol->valuedouble;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Received from %s: %.1f ppm, %.3f V",
             slots[slot_idx].node_id,
             (double)slots[slot_idx].last_ppm,
             (double)slots[slot_idx].last_voltage);

    /* Check if alert needed */
    check_alerts();
}

/* =====================================================================
 * Buzzer control
 * ===================================================================== */
static void set_buzzer(bool on)
{
    gpio_set_level(BUZZER_GPIO, on ? 1 : 0);
    buzzer_on = on;
}

/* =====================================================================
 * Check all active slots for alert condition and trigger buzzer
 * ===================================================================== */
static void check_alerts(void)
{
    bool any_alert = false;
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        if (slots[i].active && slots[i].last_ppm > ALERT_THRESHOLD_PPM) {
            slots[i].alert_active = true;
            any_alert = true;
        } else {
            slots[i].alert_active = false;
        }
    }

    set_buzzer(any_alert);
}

/* =====================================================================
 * Update the 4‑line LCD with system status
 * ===================================================================== */
static void update_display(void)
{
    char line1[21], line2[21], line3[21], line4[21];
    int active_idx = -1;
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;

    /* Determine which slot is currently active (time‑based) */
    for (int i = 0; i < TDMA_NUM_SLOTS; i++) {
        uint32_t offset = i * TDMA_SLOT_DURATION_MS;
        int64_t slot_start = (now_ms / TDMA_SLOT_PERIOD_MS) * TDMA_SLOT_PERIOD_MS + offset;
        int64_t slot_end = slot_start + TDMA_SLOT_DURATION_MS;
        if (now_ms >= slot_start && now_ms < slot_end) {
            active_idx = i;
            break;
        }
    }

    /* Line 1: System info */
    snprintf(line1, sizeof(line1), "TDMA Air Monitor  ");

    /* Line 2: Active transmitter */
    if (active_idx >= 0 && slots[active_idx].active) {
        snprintf(line2, sizeof(line2), ">> %s SENDING << ", slots[active_idx].node_id);
    } else {
        snprintf(line2, sizeof(line2), "   Waiting...      ");
    }

    /* Line 3: Data of active transmitter (or last known) */
    if (active_idx >= 0 && slots[active_idx].active) {
        snprintf(line3, sizeof(line3), "%s:%.0fppm %.2fV",
                 slots[active_idx].node_id,
                 (double)slots[active_idx].last_ppm,
                 (double)slots[active_idx].last_voltage);
    } else {
        snprintf(line3, sizeof(line3), "No active TX");
    }

    /* Line 4: Last known data of the other transmitter */
    int other_idx = (active_idx == 0) ? 1 : 0;
    if (slots[other_idx].active) {
        snprintf(line4, sizeof(line4), "%s:%.0fppm %.2fV",
                 slots[other_idx].node_id,
                 (double)slots[other_idx].last_ppm,
                 (double)slots[other_idx].last_voltage);
    } else {
        snprintf(line4, sizeof(line4), "                    ");
    }

    /* Append alert indicator */
    if (buzzer_on) {
        strcat(line1, " ALERT!");   /* shorten first line */
    }

    lcd_write(line1, line2, line3, line4);
}