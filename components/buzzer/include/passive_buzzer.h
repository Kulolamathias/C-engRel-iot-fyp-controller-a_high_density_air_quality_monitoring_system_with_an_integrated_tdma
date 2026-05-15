/**
 * @file components/buzzer_passive/buzzer_passive.h
 * @brief Passive buzzer driver – LEDC PWM for variable tones/patterns.
 *
 * @author Matthithyahu
 * @date 2026/05/12
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the passive buzzer on the default GPIO.
 */
esp_err_t buzzer_init(void);

/**
 * @brief Play a single tone for a given duration (non‑blocking).
 * @param freq_hz     Frequency in Hz.
 * @param duration_ms Duration in milliseconds.
 */
void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief Start a repeating alert pattern (3 beeps).
 */
void buzzer_alert_pattern(void);

/**
 * @brief Immediately silence the buzzer.
 */
void buzzer_stop(void);

#ifdef __cplusplus
}
#endif