// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file sensor.h
 * @brief AS7058 PPG+ECG sensor bring-up (NSX port).
 *
 * Ported from legacy heartkit-vitals-demo src/sensor.c. Phase 6 update:
 * sensor_configure() now applies the real dual-wavelength (Red PPG1_SUB1 +
 * IR PPG1_SUB2) + ECG "click golden" profile via as7058_get_active_profile()
 * (respecting AS7058_APP_PROFILE, constants.h), instead of the earlier
 * single-wavelength JSON bring-up profile -- see as7058_profiles.c. The
 * callback extracts both PPG wavelengths plus ECG.
 */
#ifndef __APP_SENSOR_H
#define __APP_SENSOR_H

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "error_codes.h"

#include "bio_spo2_a0_typedefs.h"
#include "ringbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initialized;
    /* Mirrors legacy app_state_t.inputSource (constants.h: NUM_INPUT_PTS
     * canned stimulus slots, LIVE_INPUT_MODE = live AS7058 sensor).
     * Non-live values substitute canned patient stimulus playback for the
     * live AS7058 FIFO data in the sensor callback (load_patient_data,
     * sensor.c) -- paced by the real sensor sample clock, matching legacy. */
    uint8_t inputSource;
} sensor_context_t;

err_code_t sensor_init(sensor_context_t *ctx);
err_code_t sensor_configure(void);
err_code_t sensor_start(void);
err_code_t sensor_stop(void);

uint32_t sensor_get_as7058_int_isr_count(void);
uint32_t sensor_get_as7058_isr_min_interval_ms(void);
uint32_t sensor_get_as7058_isr_max_interval_ms(void);
void sensor_reset_as7058_isr_interval_stats(void);
uint32_t sensor_get_irq_notify_missed_count(void);
uint32_t sensor_get_ppg_push_count(void);
uint32_t sensor_get_ppg_drop_count(void);
uint32_t sensor_get_ecg_push_count(void);
uint32_t sensor_get_ecg_drop_count(void);

/**
 * @brief Get the SpO2 calibration coefficients (a/b/c + dc_comp_red/ir) from
 * the active profile, if it has spo2_present && spo2_enabled set.
 *
 * @param p_cfg Output buffer for the config.
 * @return true if the active profile has a valid SpO2 config (p_cfg filled),
 * false otherwise (p_cfg untouched).
 */
bool sensor_get_spo2_config(bio_spo2_a0_configuration_t *p_cfg);

void sensor_set_irq_task_handle(TaskHandle_t handle);
void sensor_notify_irq_from_isr(BaseType_t *p_higher_priority_task_woken);
void sensor_process_irq_events(void);

extern rb_config_t rbPpg1Sensor; /* Red (PPG1_SUB1) */
extern rb_config_t rbPpg2Sensor; /* IR  (PPG1_SUB2) */
extern rb_config_t rbEcgSensor;

#ifdef __cplusplus
}
#endif

#endif // __APP_SENSOR_H
