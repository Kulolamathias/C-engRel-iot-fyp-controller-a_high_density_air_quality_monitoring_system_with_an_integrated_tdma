/**
 * @file components/buzzer_passive/buzzer_passive.c
 * @brief Implementation using LEDC and esp_timer.
 *
 * @author Matthithyahu
 * @date 2026/05/12
 */

#include "passive_buzzer.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"

#define GPIO_BUZZER           GPIO_NUM_27
#define LEDC_TIMER            LEDC_TIMER_0
#define LEDC_CHANNEL          LEDC_CHANNEL_0
#define LEDC_RESOLUTION       LEDC_TIMER_10_BIT

#define ALERT_FREQ            2500
#define ALERT_ON_MS           300
#define ALERT_OFF_MS          200
#define ALERT_REPEAT          3

static esp_timer_handle_t buzzer_timer = NULL;
static int  beep_count = 0;
static bool beep_on    = false;

static void timer_callback(void *arg);

/* ============================================= */
esp_err_t buzzer_init(void)
{
    ledc_timer_config_t tmr = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tmr));

    ledc_channel_config_t ch = {
        .gpio_num = GPIO_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    esp_timer_create_args_t args = {
        .callback = timer_callback,
        .arg = NULL,
        .name = "buzzer"
    };
    return esp_timer_create(&args, &buzzer_timer);
}

/* ============================================= */
void buzzer_tone(uint32_t freq, uint32_t dur_ms)
{
    esp_timer_stop(buzzer_timer);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER, freq);
    uint32_t duty = (1 << LEDC_RESOLUTION) / 2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    esp_timer_start_once(buzzer_timer, dur_ms * 1000);
    beep_count = 0;
    beep_on = false;
}

/* ============================================= */
void buzzer_alert_pattern(void)
{
    buzzer_tone(ALERT_FREQ, ALERT_ON_MS);
    beep_count = 0;
    beep_on = true;
    /* Timer will chain the rest */
    esp_timer_start_once(buzzer_timer, ALERT_ON_MS * 1000);
}

/* ============================================= */
void buzzer_stop(void)
{
    esp_timer_stop(buzzer_timer);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    beep_count = 0;
    beep_on = false;
}

/* ============================================= */
static void timer_callback(void *arg)
{
    if (beep_on) {
        /* Beep just finished → silence */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
        beep_on = false;
        beep_count++;
        if (beep_count < ALERT_REPEAT) {
            esp_timer_start_once(buzzer_timer, ALERT_OFF_MS * 1000);
        } else {
            beep_count = 0;
        }
    } else if (beep_count > 0) {
        /* Silence finished → next beep */
        buzzer_tone(ALERT_FREQ, ALERT_ON_MS);
        beep_on = true;
        esp_timer_start_once(buzzer_timer, ALERT_ON_MS * 1000);
    } else {
        /* Single‑shot tone finished → stop */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    }
}