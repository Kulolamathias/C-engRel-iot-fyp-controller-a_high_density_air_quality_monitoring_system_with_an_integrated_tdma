/**
 * @file components/mq135/mq135.h
 * @brief MQ135 gas sensor interface – ADC readout, resistance, and approximate ppm.
 *
 * =============================================================================
 * ARCHITECTURAL ROLE
 * =============================================================================
 * This module initialises the ADC oneshot unit and provides functions to read
 * the raw ADC value, convert it to voltage, compute sensor resistance (Rs),
 * the normalised Rs/R0 ratio, and an approximate CO₂ concentration (ppm).
 *
 * Calibration: after warm‑up, call mq135_calibrate() in clean air to record
 * the reference resistance R0. This value is stored statically and used in
 * subsequent ppm calculations.
 *
 * =============================================================================
 * OWNERSHIP
 * =============================================================================
 * - Owns: static ADC oneshot handle, static R0 calibration value.
 * - Provides: init, read raw, voltage, resistance, ratio, ppm, calibrate.
 * - Does NOT: handle heater state, apply temperature/humidity compensation,
 *   or perform multi‑gas identification.
 *
 * =============================================================================
 * INVARIANTS
 * =============================================================================
 * - mq135_init() is called exactly once before any other function.
 * - mq135_calibrate() must be called at least once before ppm functions.
 * - The load resistor value RL and circuit voltage Vc are constants and must
 *   match the actual hardware.
 *
 * @author AI Assistant
 * @date 2026/05/11
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the ADC unit and configure the MQ135 input channel.
 *
 * Must be called once after system startup and before any mq135_read_raw().
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_FAIL or ESP_ERR_NO_MEM if configuration fails
 */
esp_err_t mq135_init(void);

/**
 * @brief Read the raw 12‑bit ADC value from the MQ135 sensor.
 *
 * @param[out] raw  Pointer to store the raw ADC value (0–4095).
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if raw is NULL
 *   - ESP_ERR_INVALID_STATE if module is not initialised
 */
esp_err_t mq135_read_raw(uint32_t *raw);

/**
 * @brief Convert a raw ADC value to a voltage (volts).
 *
 * @param raw          12‑bit raw ADC value (0–4095)
 * @param[out] voltage_volts  Pointer to store the voltage in volts.
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_ARG if voltage_volts is NULL
 */
esp_err_t mq135_raw_to_voltage(uint32_t raw, float *voltage_volts);

/**
 * @brief Compute the sensor resistance Rs from a raw ADC value.
 *
 * Uses the load resistor RL and circuit voltage Vc to derive Rs.
 * Rs = RL * (Vc - Vout) / Vout
 *
 * @param raw     12‑bit raw ADC value
 * @param[out] rs_ohms  Pointer to store the sensor resistance in ohms.
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_ARG if rs_ohms is NULL
 */
esp_err_t mq135_get_resistance(uint32_t raw, float *rs_ohms);

/**
 * @brief Calibrate the sensor by recording the current Rs as the reference R0.
 *
 * Call this function after the sensor has stabilised in clean air (ideally
 * 10‑20 minutes after power‑up). It takes one reading and stores the
 * corresponding Rs as R0. Later ppm calculations use this reference.
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_STATE if module not initialised
 *   - ESP_FAIL if ADC read fails
 */
esp_err_t mq135_calibrate(void);

/**
 * @brief Get the normalised resistance ratio Rs/R0.
 *
 * Requires prior calibration (mq135_calibrate()).
 *
 * @param raw          Current raw ADC value.
 * @param[out] ratio   Pointer to store Rs/R0 (dimensionless).
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_STATE if R0 has not been set
 *   - ESP_ERR_INVALID_ARG if ratio is NULL
 */
esp_err_t mq135_get_ratio(uint32_t raw, float *ratio);

/**
 * @brief Estimate CO₂ concentration in ppm using a power‑law model.
 *
 * The model is: ppm = A * (Rs/R0)^B
 * Default parameters for CO₂ are A = 116.6020682, B = -2.769034857.
 * These values are typical for the MQ135 under standard conditions.
 *
 * Requires calibration (mq135_calibrate()). The result is an approximation
 * and should not be used for safety‑critical applications.
 *
 * @param raw        Current raw ADC value.
 * @param[out] ppm   Pointer to store the estimated CO₂ concentration (ppm).
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_STATE if not calibrated
 *   - ESP_ERR_INVALID_ARG if ppm is NULL
 */
esp_err_t mq135_get_ppm_co2(uint32_t raw, float *ppm);

#ifdef __cplusplus
}
#endif