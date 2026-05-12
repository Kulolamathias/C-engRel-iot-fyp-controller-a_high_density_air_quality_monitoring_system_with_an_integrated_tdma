/**
 * @file components/mq135/mq135.c
 * @brief Implementation of the MQ135 proof‑of‑concept module.
 *
 * Includes resistance, ratio, and approximate CO₂ ppm calculations.
 *
 * @author AI Assistant
 * @date 2026/05/11
 */

#include "mq135.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_check.h"
#include <inttypes.h>
#include <math.h>       // for powf()

static const char *TAG = "mq135";

/* -------------------------------------------------------------------------
 * Hardware / ADC configuration constants
 * ------------------------------------------------------------------------- */
#define MQ135_ADC_UNIT          ADC_UNIT_1      /**< ADC unit 1 */
#define MQ135_ADC_CHANNEL       ADC_CHANNEL_6   /**< GPIO34 on ESP32 */
#define MQ135_ADC_ATTEN         ADC_ATTEN_DB_11 /**< Full‑scale ~3.3 V */
#define MQ135_ADC_BITWIDTH      ADC_BITWIDTH_12 /**< 12‑bit resolution */
#define MQ135_ADC_MAX_RAW       4095            /**< (1 << 12) – 1 */
#define MQ135_REFERENCE_VOLTAGE 3.3f            /**< ADC reference voltage (V) */

/* -------------------------------------------------------------------------
 * Circuit constants – must match your hardware
 * ------------------------------------------------------------------------- */
#define MQ135_LOAD_RESISTOR_OHM  1000.0f        /**< RL in ohms (typical 1k) */
#define MQ135_CIRCUIT_VOLTAGE    5.0f           /**< Vc supplied to sensor */

/* -------------------------------------------------------------------------
 * CO₂ curve parameters (typical for MQ135)
 * ------------------------------------------------------------------------- */
#define MQ135_CO2_A  116.6020682f               /**< Scaling factor */
#define MQ135_CO2_B  (-2.769034857f)            /**< Exponent */

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
static adc_oneshot_unit_handle_t adc_handle = NULL;
static float r0_value = 0.0f;                   /**< Calibrated R0 (ohms) */

/* ========================================================================= */
esp_err_t mq135_init(void)
{
    esp_err_t ret;

    /* ADC unit init */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = MQ135_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&unit_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Channel config */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = MQ135_ADC_ATTEN,
        .bitwidth = MQ135_ADC_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(adc_handle, MQ135_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Initialised – ADC%d channel %d, atten %d, bitwidth %d",
             (int)MQ135_ADC_UNIT, (int)MQ135_ADC_CHANNEL,
             (int)MQ135_ADC_ATTEN, (int)MQ135_ADC_BITWIDTH);
    return ESP_OK;
}

/* ========================================================================= */
esp_err_t mq135_read_raw(uint32_t *raw)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG, "raw is NULL");
    ESP_RETURN_ON_FALSE(adc_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Module not initialised – call mq135_init() first");

    int raw_val;
    esp_err_t ret = adc_oneshot_read(adc_handle, MQ135_ADC_CHANNEL, &raw_val);
    if (ret == ESP_OK) {
        *raw = (uint32_t)raw_val;
    } else {
        ESP_LOGE(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ========================================================================= */
esp_err_t mq135_raw_to_voltage(uint32_t raw, float *voltage_volts)
{
    ESP_RETURN_ON_FALSE(voltage_volts != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "voltage_volts is NULL");
    *voltage_volts = (float)raw * MQ135_REFERENCE_VOLTAGE / (float)MQ135_ADC_MAX_RAW;
    return ESP_OK;
}

/* ========================================================================= */
esp_err_t mq135_get_resistance(uint32_t raw, float *rs_ohms)
{
    ESP_RETURN_ON_FALSE(rs_ohms != NULL, ESP_ERR_INVALID_ARG, TAG, "rs_ohms is NULL");

    float vout;
    esp_err_t ret = mq135_raw_to_voltage(raw, &vout);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Avoid division by zero if sensor output is at rail */
    if (vout >= MQ135_CIRCUIT_VOLTAGE) {
        *rs_ohms = 0.0f;   /* sensor saturated low resistance */
    } else if (vout <= 0.001f) {
        *rs_ohms = 1e6f;   /* clamp high resistance */
    } else {
        *rs_ohms = MQ135_LOAD_RESISTOR_OHM *
                   (MQ135_CIRCUIT_VOLTAGE - vout) / vout;
    }
    return ESP_OK;
}

/* ========================================================================= */
esp_err_t mq135_calibrate(void)
{
    ESP_RETURN_ON_FALSE(adc_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Module not initialised");

    uint32_t raw;
    esp_err_t ret = mq135_read_raw(&raw);
    if (ret != ESP_OK) {
        return ret;
    }

    float rs;
    ret = mq135_get_resistance(raw, &rs);
    if (ret != ESP_OK) {
        return ret;
    }

    r0_value = rs;
    ESP_LOGI(TAG, "Calibration complete – R0 = %.2f ohms", (double)r0_value);
    return ESP_OK;
}

/* ========================================================================= */
esp_err_t mq135_get_ratio(uint32_t raw, float *ratio)
{
    ESP_RETURN_ON_FALSE(ratio != NULL, ESP_ERR_INVALID_ARG, TAG, "ratio is NULL");
    if (r0_value <= 0.0f) {
        ESP_LOGE(TAG, "R0 not set – run mq135_calibrate() first");
        return ESP_ERR_INVALID_STATE;
    }

    float rs;
    esp_err_t ret = mq135_get_resistance(raw, &rs);
    if (ret != ESP_OK) {
        return ret;
    }

    *ratio = rs / r0_value;
    return ESP_OK;
}

/* ========================================================================= */
esp_err_t mq135_get_ppm_co2(uint32_t raw, float *ppm)
{
    float ratio;
    esp_err_t ret = mq135_get_ratio(raw, &ratio);
    if (ret != ESP_OK) {
        return ret;
    }

    /* PPM = A * (ratio)^B */
    *ppm = MQ135_CO2_A * powf(ratio, MQ135_CO2_B);
    return ESP_OK;
}